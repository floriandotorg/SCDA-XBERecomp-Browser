#pragma once
#include "cpu/x86_context.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

#ifdef __wasm_tail_call__
#define BROWSER_GUEST_TAIL [[clang::musttail]] return
#else
#define BROWSER_GUEST_TAIL return
#endif

namespace browser {

struct Fault : std::runtime_error {
  std::shared_ptr<const X86Context> context;
  uint32_t address;
  size_t size;
  Fault(const X86Context& ctx, uint32_t address, size_t size, const char* reason);
};

struct Cancelled : Fault {
  explicit Cancelled(const X86Context& ctx) : Fault(ctx, ctx.last_icall, 0, "guest runtime cancelled") {}
};

class Memory {
  template <typename T>
  struct PageAllocator {
    using value_type = T;
    PageAllocator() = default;
    template <typename U> PageAllocator(const PageAllocator<U>&) {}
    T* allocate(size_t count) { return static_cast<T*>(::operator new(count * sizeof(T), std::align_val_t(4096))); }
    void deallocate(T* pointer, size_t) { ::operator delete(pointer, std::align_val_t(4096)); }
    template <typename U> bool operator==(const PageAllocator<U>&) const { return true; }
  };
  struct Storage {
    std::vector<uint8_t, PageAllocator<uint8_t>> bytes;
    std::vector<uint8_t> committed;
    explicit Storage(uint32_t size);
  };
  struct Region {
    uint32_t address;
    std::shared_ptr<Storage> storage;
  };
  mutable std::recursive_mutex memoryLock_;
  size_t leases_ = 0;
  bool physicalSpan_ = false;
  static constinit thread_local size_t threadBorrows_;
  static constinit thread_local Memory* borrowedMemory_;
  static constinit thread_local Memory* mutatedMemory_;
  bool HasAccess() const { return borrowedMemory_ == this || mutatedMemory_ == this; }
  std::vector<Region> regions_;
  std::array<size_t, 256> regionLookup_{};
  std::shared_ptr<Storage> physicalStorage_;
  std::vector<uint64_t> physicalVersions_;
  std::vector<uint64_t> physicalBlockVersions_;
  void BumpPhysical(const X86Context& ctx, uint32_t address, size_t size, size_t page) {
    if (physicalEpoch_ == UINT64_MAX) { WriteVersionExhausted(ctx, address, size); }
    physicalVersions_[page] = physicalBlockVersions_[page / 8] = ++physicalEpoch_;
  }
  uint64_t physicalEpoch_ = 0;
  struct Page {
    uint8_t* bytes = nullptr;
    uint64_t* version = nullptr;
  };
  struct PageTable {
    Memory& memory;
    std::array<Page, 1u << 20> entries{};
    explicit PageTable(Memory& owner) : memory(owner) {}
  };
  std::unique_ptr<PageTable> pages_;
public:
  static constexpr size_t kFlatWrittenOffset = sizeof(uintptr_t) << 20;
  static constexpr uintptr_t kFlatUnmapped = uintptr_t(0) - 0x10000;
private:
  struct FlatTable {
    Memory* owner;
    alignas(64) std::array<uintptr_t, 1u << 20> delta;
    std::array<uint8_t, 1u << 20> written;
  };
  std::unique_ptr<FlatTable> flat_;
  void Flatten(uint32_t page, uint8_t* host) {
    flat_->delta[page] = host ? reinterpret_cast<uintptr_t>(host) - uintptr_t(page) * 4096 : kFlatUnmapped - uintptr_t(page) * 4096;
  }
  uint8_t* FlatSpan(uint32_t address, size_t size) const;
  bool TakeFlatWrite(const uint8_t* host) {
    auto& written = flat_->written[(reinterpret_cast<uintptr_t>(host) >> 12) & ((1u << 20) - 1)];
    if (!written) { return false; }
    written = 0;
    return true;
  }
  void InitializePages();
  static Page* PageAt(Page* pages, uint32_t address, size_t size) {
    if (!pages || !size || size > 16 || size > 4096 - (address & 4095)) { return nullptr; }
    auto& page = pages[address >> 12];
    return page.bytes ? &page : nullptr;
  }
  Page* PageAt(uint32_t address, size_t size) { return PageAt(pages_ ? pages_->entries.data() : nullptr, address, size); }
  [[noreturn, gnu::cold, gnu::noinline]] void WriteVersionExhausted(const X86Context& ctx, uint32_t address, size_t size);
  void MarkPageWrite(const X86Context& ctx, uint32_t address, size_t size, Page& page) {
    if (!page.version) { return; }
#ifdef X86_FLAT_MEMORY
    flat_->written[(reinterpret_cast<uintptr_t>(page.bytes) >> 12) & ((1u << 20) - 1)] = 1;
    return;
#endif
    if (physicalEpoch_ == UINT64_MAX) { WriteVersionExhausted(ctx, address, size); }
    *page.version = ++physicalEpoch_;
  }
  void MarkPhysicalWrite(const X86Context& ctx, const Region& region, size_t offset, size_t size);
  void Repeat(X86Context& ctx, uint32_t width, bool fill);
  void RequireMutable(uint32_t address, size_t size);
  Region& RegionAt(const X86Context& ctx, uint32_t address, size_t size);
  void Validate(const X86Context& ctx, uint32_t address, size_t size);
  void Pages(uint32_t address, uint32_t size, bool committed);
public:
  class Lease {
    Memory& memory_;
    std::unique_lock<std::recursive_mutex> lock_;
    Memory* previous_;
  public:
    explicit Lease(Memory& memory) : memory_(memory), lock_(memory.memoryLock_), previous_(borrowedMemory_) { ++memory_.leases_; ++threadBorrows_; borrowedMemory_ = &memory_; }
    ~Lease() { borrowedMemory_ = previous_; --threadBorrows_; --memory_.leases_; if (!memory_.leases_) { memory_.physicalSpan_ = false; } }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
  };
  class Mutation {
    std::unique_lock<std::recursive_mutex> lock_;
    Memory* previous_;
  public:
    explicit Mutation(Memory& memory) : lock_(memory.memoryLock_), previous_(mutatedMemory_) {
      memory.RequireMutable(0, 0);
      mutatedMemory_ = &memory;
    }
    ~Mutation() { mutatedMemory_ = previous_; }
    Mutation(const Mutation&) = delete;
    Mutation& operator=(const Mutation&) = delete;
  };
  class PauseExecution;
  class Execution {
    friend class Memory;
    friend class PauseExecution;
    static constinit thread_local Execution* current_;
    Memory* memory_ = nullptr;
    Execution* previous_ = nullptr;
    std::optional<Lease> lease_;
    uint32_t checkpoints_ = 0;
  public:
    explicit Execution(Memory* memory);
    ~Execution();
    Execution(const Execution&) = delete;
    Execution& operator=(const Execution&) = delete;
  };
  class PauseExecution {
    Execution* execution_ = nullptr;
  public:
    PauseExecution();
    ~PauseExecution();
    PauseExecution(const PauseExecution&) = delete;
    PauseExecution& operator=(const PauseExecution&) = delete;
  };
  static void YieldExecution();
  static void YieldExecutionNow();
  Mutation Mutate() { return Mutation(*this); }
  Lease Borrow() { return Lease(*this); }
  PageTable* BeginBlock();
  void EndBlock() noexcept;
  class BlockScope {
    PageTable& table_;
  public:
    explicit BlockScope(Memory& memory) : table_(*memory.BeginBlock()) {}
    ~BlockScope() { table_.memory.EndBlock(); }
    BlockScope(const BlockScope&) = delete;
    BlockScope& operator=(const BlockScope&) = delete;
    template <typename T> T Load(const X86Context& ctx, uint32_t address) {
      T value;
      auto* page = Memory::PageAt(table_.entries.data(), address, sizeof(T));
      if (page) { std::memcpy(&value, page->bytes + (address & 4095), sizeof(T)); }
      else { table_.memory.Read(ctx, address, &value, sizeof(T)); }
      return value;
    }
    template <typename T> void Store(const X86Context& ctx, uint32_t address, T value) {
      auto* page = Memory::PageAt(table_.entries.data(), address, sizeof(T));
      if (page) {
        table_.memory.MarkPageWrite(ctx, address, sizeof(T), *page);
        std::memcpy(page->bytes + (address & 4095), &value, sizeof(T));
      } else { table_.memory.Write(ctx, address, &value, sizeof(T)); }
    }
  };
  static bool HasBorrow() { return threadBorrows_ != 0; }
  Memory();
  void Map(uint32_t address, uint32_t size);
  void Ensure(uint32_t address, uint32_t size);
  void Discard(uint32_t address, uint32_t size);
  struct PageRange {
    uint32_t base;
    uint64_t end;
    uint32_t mappingBase;
    bool mapped;
    bool committed;
  };
  PageRange QueryPages(uint32_t address) const;
  struct Usage {
    uint32_t physicalPages;
    uint32_t privateCommittedPages;
  };
  Usage QueryUsage() const;
  uint32_t CommittedPages(uint32_t address, uint32_t size) const;
  bool IsCommitted(uint32_t address) const;
  void DecommitMapped(uint32_t address, uint32_t size);
  void Commit(uint32_t address, uint32_t size);
  void Decommit(uint32_t address, uint32_t size);
  uint8_t* Span(const X86Context& ctx, uint32_t address, size_t size);
  uint32_t GuestAddress(const X86Context& ctx, const void* pointer, size_t size);
  void CheckRange(const X86Context& ctx, uint32_t address, size_t size);
  std::optional<uint64_t> PhysicalWriteVersion(const X86Context& ctx, uint32_t address, size_t size);
  [[gnu::noinline]] void Read(const X86Context& ctx, uint32_t address, void* output, size_t size);
  [[gnu::noinline]] void Write(const X86Context& ctx, uint32_t address, const void* input, size_t size);
  template <typename T> [[gnu::noinline]] T Load(const X86Context& ctx, uint32_t address) {
    T value;
    auto* page = HasAccess() ? PageAt(address, sizeof(T)) : nullptr;
    if (page) { std::memcpy(&value, page->bytes + (address & 4095), sizeof(T)); }
    else { Read(ctx, address, &value, sizeof(T)); }
    return value;
  }
  template <typename T> [[gnu::noinline]] void Store(const X86Context& ctx, uint32_t address, T value) {
    auto* page = HasAccess() ? PageAt(address, sizeof(T)) : nullptr;
    if (page) {
      MarkPageWrite(ctx, address, sizeof(T), *page);
      std::memcpy(page->bytes + (address & 4095), &value, sizeof(T));
    } else { Write(ctx, address, &value, sizeof(T)); }
  }
  void Copy(const X86Context& ctx, uint32_t destination, uint32_t source, size_t size);
  void MoveRepeated(X86Context& ctx, uint32_t width);
  void StoreRepeated(X86Context& ctx, uint32_t width);
  size_t BackingBytes() const;
  uint8_t* Base() { return reinterpret_cast<uint8_t*>(flat_->delta.data()); }
  static Memory& From(uint8_t* base) { return *reinterpret_cast<FlatTable*>(base - offsetof(FlatTable, delta))->owner; }
  static void FlatRepeat(X86Context& ctx, uint8_t* base, uint32_t width, bool fill);
};

inline uint8_t* FlatHost(uint8_t* base, uint32_t address) {
  return reinterpret_cast<uint8_t*>(reinterpret_cast<const uintptr_t*>(base)[address >> 12] + address);
}

inline uint8_t* FlatWrite(uint8_t* base, uint32_t address) {
  auto* host = FlatHost(base, address);
  base[Memory::kFlatWrittenOffset + ((reinterpret_cast<uintptr_t>(host) >> 12) & ((1u << 20) - 1))] = 1;
  return host;
}

template <typename T>
class FlatReference {
  uint8_t* base_;
  uint32_t address_;
public:
  FlatReference(uint8_t* base, uint32_t address) : base_(base), address_(address) {}
  operator T() const {
    T value;
    std::memcpy(&value, FlatHost(base_, address_), sizeof(T));
    return value;
  }
  FlatReference& operator=(T value) {
    std::memcpy(FlatWrite(base_, address_), &value, sizeof(T));
    return *this;
  }
  FlatReference& operator=(const FlatReference& value) { return *this = T(value); }
};

template <typename T>
inline T* FlatPointer(uint8_t* base, uint32_t address) { return reinterpret_cast<T*>(FlatWrite(base, address)); }

template <typename T, typename Backing = Memory>
class Reference {
  Backing& memory_;
  X86Context& context_;
  uint32_t address_;
public:
  Reference(Backing& memory, X86Context& context, uint32_t address)
    : memory_(memory), context_(context), address_(address) {}
  operator T() const {
    return memory_.template Load<T>(context_, address_);
  }
  Reference& operator=(T value) {
    memory_.template Store<T>(context_, address_, value);
    return *this;
  }
  Reference& operator=(const Reference& value) { return *this = T(value); }
};

template <typename T>
inline Reference<T> Access(X86Context& ctx, uint8_t* base, uint32_t address) {
  return {Memory::From(base), ctx, address};
}

template <typename T, typename Backing = Memory>
class ValuePointer {
  Reference<T, Backing> reference_;
public:
  ValuePointer(X86Context& ctx, uint8_t* base, uint32_t address) : reference_(Access<T>(ctx, base, address)) {}
  explicit ValuePointer(Reference<T, Backing> reference) : reference_(reference) {}
  Reference<T, Backing> operator*() const { return reference_; }
};

template <typename T>
inline Reference<T, Memory::BlockScope> BlockAccess(Memory::BlockScope& scope, X86Context& ctx, uint32_t address) {
  return {scope, ctx, address};
}

template <typename T>
inline auto BlockPointer(Memory::BlockScope& scope, X86Context& ctx, uint32_t address) {
  static_assert(std::is_same_v<T, X86Xmm>);
  return ValuePointer<T, Memory::BlockScope>{BlockAccess<T>(scope, ctx, address)};
}

inline void BlockPush32(Memory::BlockScope& scope, X86Context& ctx, uint32_t value) {
  ctx.esp -= 4;
  scope.Store(ctx, ctx.esp, value);
}

template <typename T>
inline auto Pointer(X86Context& ctx, uint8_t* base, uint32_t address) {
  if constexpr (std::is_same_v<T, X86Xmm>) {
    return ValuePointer<T>(ctx, base, address);
  } else {
    if (address % alignof(T)) { throw Fault(ctx, address, sizeof(T), "unsupported misaligned host or atomic pointer"); }
    return reinterpret_cast<T*>(Memory::From(base).Span(ctx, address, sizeof(T)));
  }
}

class Runtime {
public:
  enum class KernelTracePhase : uint32_t { Before, After, Fault };
  struct KernelTraceEvent {
    uint64_t sequence = 0;
    uint64_t callId = 0;
    uint64_t workerId = 0;
    uint32_t ordinal = 0;
    uint32_t fsBase = 0;
    uint32_t eax = 0;
    uint32_t esp = 0;
    uint32_t lastIcall = 0;
    KernelTracePhase phase = KernelTracePhase::Before;
  };
  struct KernelTraceSnapshot {
    std::array<KernelTraceEvent, 64> events{};
    size_t count = 0;
    uint64_t total = 0;
  };
private:
  mutable std::mutex kernelTraceLock_;
  std::array<KernelTraceEvent, 64> kernelTrace_{};
  uint64_t kernelTraceTotal_ = 0;
  uint64_t workerSequence_ = 0;
  static constinit thread_local uint64_t workerId_;
  uint64_t RecordKernelTrace(const X86Context& ctx, uint32_t ordinal, KernelTracePhase phase, uint64_t callId);
  std::vector<X86FuncMapping> mappings_;
  uint32_t directBase_ = 0;
  std::vector<X86Func*> direct_;
  std::vector<uint8_t> directContinuation_;
  std::vector<uint32_t> continuations_;
  std::vector<X86Func*> hosts_;
  std::vector<std::function<void(X86Context&, uint8_t*)>> kernels_;
  uint64_t kernelRevision_ = 0;
  std::mutex lock_;
  std::mutex waitLock_;
  std::condition_variable completion_;
  std::atomic<bool> cancelled_ = false;
  std::atomic<uint32_t> progress_ = 0;
  void RecordProgress(const X86Context& ctx);
  mutable std::atomic<size_t> stackPeak_ = 0;
  mutable std::atomic<size_t> stackBudget_ = size_t(-1);
  uint64_t sequence_ = 0;
  struct Worker {
    Runtime* runtime;
    uint64_t id;
    std::thread thread;
    std::atomic<bool> done = false;
    std::exception_ptr failure;
    Worker(Runtime* runtime, uint64_t id) : runtime(runtime), id(id) {}
    ~Worker();
    void Join(const char* phase);
    void Diagnose(const char* phase, const char* reason, int code = 0) const noexcept;
  };
  std::mutex workersLock_;
  std::mutex joiningLock_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::atomic<size_t> activeWorkers_ = 0;
  const size_t workerCapacity_;
  std::atomic<Memory*> imageMemory_ = nullptr;
  Memory* sdkMemory_ = nullptr;
  std::function<bool(X86Context&, uint32_t, uint64_t)> sdkObserver_;
  Memory* invocationMemory_ = nullptr;
  std::function<void(const X86Context&, uint8_t*, uint32_t, uint64_t)> invocationObserver_;
  std::vector<uint32_t> invocationReturns_;
  std::exception_ptr failure_;
  static constinit thread_local Runtime* worker_;
  static constinit thread_local X86Context* workerContext_;
  static constinit thread_local uint32_t workerKpcr_;
  static constinit thread_local Runtime* clock_;
  std::function<uint64_t()> timerSource_;
  std::mutex timerLock_;
  std::optional<uint64_t> timerEpoch_;
  uint64_t timerSample_ = 0;
  std::atomic<uint64_t> timerElapsed_ = 0;
  std::atomic<uint64_t> timerGeneration_ = 0;
  double timerReuseMilliseconds_ = 0;
  void StartTimer();
  std::function<void(X86Context&, uint8_t*)> expiryDriver_;
  static constinit thread_local bool polling_;
  static constinit thread_local uint32_t loopPollBudget_;
  void RequireImageSession(const X86Context& ctx, uint8_t* base) const;
  void ObserveInvocation(const X86Context& ctx, uint8_t* base, uint32_t target) noexcept;
public:
  static constinit thread_local uint32_t loopPolls;
  [[gnu::noinline]] static void PollLoopSlow(X86Context& ctx, uint8_t* base);
  void Charge(X86Context& ctx, uint8_t* base, uint32_t cost);
  enum class WaitResult { Changed, TimedOut };
  std::atomic<uint64_t> ticks = 0;
  size_t StackPeak() const { return stackPeak_.load(); }
  size_t StackBudget() const { const size_t budget = stackBudget_.load(); return budget == size_t(-1) ? 0 : budget; }
  void Checkpoint(const X86Context& ctx) const;
  bool IsCancelled() const { return cancelled_.load(std::memory_order_relaxed); }
  void Cancel();
  uint64_t Sequence();
  void Wake();
  WaitResult Wait(const X86Context& ctx, uint64_t sequence, std::chrono::milliseconds timeout);
  class ClockBinding {
    Runtime* previous_;
  public:
    explicit ClockBinding(Runtime& runtime) : previous_(clock_) { clock_ = &runtime; }
    ~ClockBinding() { clock_ = previous_; }
    ClockBinding(const ClockBinding&) = delete;
    ClockBinding& operator=(const ClockBinding&) = delete;
  };
  ClockBinding BindClock() { return ClockBinding(*this); }
  Runtime(std::span<const X86FuncMapping> mappings, std::span<const uint32_t> continuations,
    std::function<uint64_t()> timerSource = {}, size_t workerCapacity = 2);
  void InstallExpiryDriver(std::function<void(X86Context&, uint8_t*)> driver);
  void ReuseTimerSamples(double milliseconds);
  void InstallSdkObserver(Memory& memory, std::function<bool(X86Context&, uint32_t, uint64_t)> observer);
  void InstallInvocationObserver(Memory& memory, std::function<void(const X86Context&, uint8_t*, uint32_t, uint64_t)> observer,
    std::vector<uint32_t> returnAddresses = {});
  static bool ObserveSdkEntry(X86Context& ctx, uint8_t* base, uint32_t id);
  void Poll(X86Context& ctx, uint8_t* base);
  void PollLoop(X86Context& ctx, uint8_t* base);
  static uint64_t SystemTime();
  uint64_t TimerTime(const X86Context& ctx);
  uint64_t RelativeTimerDeadline(const X86Context& ctx, int64_t dueTime);
  ~Runtime();
  void Launch(X86Context ctx, uint8_t* base, std::function<void(X86Context&, uint8_t*)> entry);
  size_t ActiveWorkers() const { return activeWorkers_.load(); }
  size_t WorkerCapacity() const { return workerCapacity_; }
  bool HasPublishedImage() const { return imageMemory_.load() != nullptr; }
  const std::atomic<uint32_t>* ProgressAddress() const { return &progress_; }
  bool OwnsCurrentWorker() const { return worker_ == this; }
  bool OwnsCurrentContext(const X86Context& context) const {
    return worker_ == this && context.host == this && context.fs_base == workerKpcr_;
  }
  std::exception_ptr Join();
  std::exception_ptr Stop();
  static Runtime& From(X86Context& ctx);
  X86Func* Find(uint32_t target) const;
  void Invoke(X86Context& ctx, uint8_t* base, uint32_t target);
  void Resume(X86Context& ctx, uint8_t* base, uint32_t target, uint32_t esp);
  uint32_t Call(X86Context& ctx, uint8_t* base, uint32_t target, std::initializer_list<uint32_t> args);
  uint32_t HostThunk(X86Func* function);
  void RegisterKernel(uint32_t ordinal, std::function<void(X86Context&, uint8_t*)> function);
  uint64_t KernelRevision();
  void PublishImageMemory(Memory& memory, uint64_t revision, const std::function<void()>& publish);
  uint32_t KernelTarget(uint32_t ordinal);
  void Kernel(X86Context& ctx, uint8_t* base, uint32_t ordinal);
  KernelTraceSnapshot SnapshotKernelTrace() const;
  static uint64_t Clock();
};

}
