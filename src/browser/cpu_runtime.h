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
  struct Storage {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> committed;
    explicit Storage(uint32_t size);
  };
  struct Region {
    uint32_t address;
    std::shared_ptr<Storage> storage;
  };
  mutable std::recursive_mutex memoryLock_;
  size_t leases_ = 0;
  static thread_local size_t threadBorrows_;
  static thread_local Memory* borrowedMemory_;
  static thread_local Memory* mutatedMemory_;
  bool HasAccess() const { return borrowedMemory_ == this || mutatedMemory_ == this; }
  std::vector<Region> regions_;
  std::array<size_t, 256> regionLookup_{};
  std::shared_ptr<Storage> physicalStorage_;
  std::vector<uint64_t> physicalVersions_;
  uint64_t physicalEpoch_ = 0;
  struct Page {
    uint8_t* bytes = nullptr;
    uint64_t* version = nullptr;
  };
  std::vector<Page> pages_;
  void InitializePages();
  Page* PageAt(uint32_t address, size_t size) {
    if (pages_.empty() || !size || size > 16 || size > 4096 - (address & 4095)) { return nullptr; }
    auto& page = pages_[address >> 12];
    return page.bytes ? &page : nullptr;
  }
  void MarkPageWrite(const X86Context& ctx, uint32_t address, size_t size, Page& page) {
    if (!page.version) { return; }
    if (physicalEpoch_ == UINT64_MAX) { throw Fault(ctx, address, size, "physical write version exhausted"); }
    *page.version = ++physicalEpoch_;
  }
  void MarkPhysicalWrite(const X86Context& ctx, const Region& region, size_t offset, size_t size);
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
    ~Lease() { borrowedMemory_ = previous_; --threadBorrows_; --memory_.leases_; }
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
  Mutation Mutate() { return Mutation(*this); }
  Lease Borrow() { return Lease(*this); }
  void BeginBlock();
  void EndBlock() noexcept;
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
  void Read(const X86Context& ctx, uint32_t address, void* output, size_t size);
  void Write(const X86Context& ctx, uint32_t address, const void* input, size_t size);
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
  size_t BackingBytes() const;
  uint8_t* Base() { return reinterpret_cast<uint8_t*>(this); }
  static Memory& From(uint8_t* base) { return *reinterpret_cast<Memory*>(base); }
};

template <typename T>
class Reference {
  Memory& memory_;
  X86Context& context_;
  uint32_t address_;
public:
  Reference(Memory& memory, X86Context& context, uint32_t address)
    : memory_(memory), context_(context), address_(address) {}
  operator T() const {
    return memory_.Load<T>(context_, address_);
  }
  Reference& operator=(T value) {
    memory_.Store<T>(context_, address_, value);
    return *this;
  }
  Reference& operator=(const Reference& value) { return *this = T(value); }
};

template <typename T>
inline Reference<T> Access(X86Context& ctx, uint8_t* base, uint32_t address) {
  return {Memory::From(base), ctx, address};
}

template <typename T>
class ValuePointer {
  Reference<T> reference_;
public:
  ValuePointer(X86Context& ctx, uint8_t* base, uint32_t address) : reference_(Access<T>(ctx, base, address)) {}
  Reference<T> operator*() const { return reference_; }
};

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
  static thread_local uint64_t workerId_;
  uint64_t RecordKernelTrace(const X86Context& ctx, uint32_t ordinal, KernelTracePhase phase, uint64_t callId);
  std::vector<X86FuncMapping> mappings_;
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
  std::exception_ptr failure_;
  static thread_local Runtime* worker_;
  static thread_local X86Context* workerContext_;
  static thread_local uint32_t workerKpcr_;
  static thread_local Runtime* clock_;
  std::function<uint64_t()> timerSource_;
  std::mutex timerLock_;
  std::optional<uint64_t> timerEpoch_;
  uint64_t timerSample_ = 0;
  void StartTimer();
  std::function<void(X86Context&, uint8_t*)> expiryDriver_;
  static thread_local bool polling_;
  static thread_local uint32_t loopPollBudget_;
  void RequireImageSession(const X86Context& ctx, uint8_t* base) const;
  void ObserveInvocation(const X86Context& ctx, uint8_t* base, uint32_t target) noexcept;
public:
  enum class WaitResult { Changed, TimedOut };
  std::atomic<uint64_t> ticks = 0;
  size_t StackPeak() const { return stackPeak_.load(); }
  size_t StackBudget() const { const size_t budget = stackBudget_.load(); return budget == size_t(-1) ? 0 : budget; }
  void Checkpoint(const X86Context& ctx) const;
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
  void InstallSdkObserver(Memory& memory, std::function<bool(X86Context&, uint32_t, uint64_t)> observer);
  void InstallInvocationObserver(Memory& memory, std::function<void(const X86Context&, uint8_t*, uint32_t, uint64_t)> observer);
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
