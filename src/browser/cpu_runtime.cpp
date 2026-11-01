#include "browser/cpu_runtime.h"
#include "cpu/x86_ops.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#ifdef __EMSCRIPTEN__
#include <emscripten/stack.h>
#endif

namespace browser {

namespace {
constexpr uint32_t pageSize = 4096;
constexpr size_t backingBudget = 112u << 20;
const auto Describe = [](const X86Context& ctx, uint32_t address, size_t size, const char* reason) {
  std::array<char, 256> message{};
  std::snprintf(message.data(), message.size(), "%s address=0x%08X size=%zu esp=0x%08X last_icall=0x%08X", reason, address, size, ctx.esp, ctx.last_icall);
  return std::string(message.data());
};
}

Fault::Fault(const X86Context& ctx, uint32_t address, size_t size, const char* reason)
  : std::runtime_error(Describe(ctx, address, size, reason)), context(std::make_shared<const X86Context>(ctx)), address(address), size(size) {}

thread_local size_t Memory::threadBorrows_ = 0;

Memory::Storage::Storage(uint32_t size) : bytes(size), committed(size / pageSize) {}

Memory::Memory() {
  Map(0x80000000, 64u << 20);
  std::fill(regions_[0].storage->committed.begin(), regions_[0].storage->committed.end(), 1);
  regions_.push_back({0xF0000000, regions_[0].storage});
  physicalStorage_ = regions_[0].storage;
}

void Memory::RequireMutable(uint32_t address, size_t size) {
  if (leases_) { throw Fault({}, address, size, "memory transition during host span lease"); }
}

void Memory::Map(uint32_t address, uint32_t size) {
  std::lock_guard lock(memoryLock_);
  RequireMutable(address, size);
  if (!size || address % pageSize || size % pageSize || uint64_t(address) + size > (1ull << 32)) {
    throw Fault({}, address, size, "invalid region");
  }
  for (const auto& region : regions_) {
    if (uint64_t(address) < uint64_t(region.address) + region.storage->bytes.size() && uint64_t(address) + size > region.address) {
      throw Fault({}, address, size, "overlapping region");
    }
  }
  if (BackingBytes() + size + size / pageSize > backingBudget) { throw Fault({}, address, size, "prototype backing budget exhausted"); }
  regions_.push_back({address, std::make_shared<Storage>(size)});
}

void Memory::Ensure(uint32_t address, uint32_t size) {
  std::lock_guard lock(memoryLock_);
  RequireMutable(address, size);
  if (!size || address % pageSize || size % pageSize || uint64_t(address) + size > (1ull << 32)) {
    throw Fault({}, address, size, "invalid backing range");
  }
  std::vector<std::pair<uint32_t, uint32_t>> missing;
  uint64_t cursor = address;
  const uint64_t end = cursor + size;
  size_t required = 0;
  while (cursor < end) {
    uint64_t next = end;
    bool mapped = false;
    for (const auto& region : regions_) {
      const uint64_t regionEnd = uint64_t(region.address) + region.storage->bytes.size();
      if (cursor >= region.address && cursor < regionEnd) {
        cursor = std::min(end, regionEnd);
        mapped = true;
        break;
      }
      if (region.address > cursor) { next = std::min(next, uint64_t(region.address)); }
    }
    if (!mapped) {
      const uint32_t count = uint32_t(next - cursor);
      required += size_t(count) + count / pageSize;
      missing.emplace_back(uint32_t(cursor), count);
      cursor = next;
    }
  }
  if (required > backingBudget - BackingBytes()) { throw Fault({}, address, size, "prototype backing budget exhausted"); }
  std::vector<Region> pending;
  for (const auto& [start, count] : missing) { pending.push_back({start, std::make_shared<Storage>(count)}); }
  regions_.reserve(regions_.size() + pending.size());
  regions_.insert(regions_.end(), pending.begin(), pending.end());
  Commit(address, size);
}

void Memory::Discard(uint32_t address, uint32_t size) {
  std::lock_guard lock(memoryLock_);
  RequireMutable(address, size);
  const uint64_t end = uint64_t(address) + size;
  if (end > (1ull << 32)) { throw Fault({}, address, size, "discard range wraparound"); }
  for (const auto& region : regions_) {
    const uint64_t regionEnd = uint64_t(region.address) + region.storage->bytes.size();
    if (region.address < end && regionEnd > address &&
        (region.address < address || regionEnd > end || region.storage.use_count() > 1)) {
      throw Fault({}, address, size, "discard crosses backing ownership");
    }
  }
  std::erase_if(regions_, [&](const Region& region) { return region.address >= address && uint64_t(region.address) < end; });
}

Memory::PageRange Memory::QueryPages(uint32_t address) const {
  std::lock_guard lock(memoryLock_);
  PageRange result{address & ~(pageSize - 1), 1ull << 32, 0, false, false};
  for (const auto& region : regions_) {
    if (address >= region.address && uint64_t(address) < uint64_t(region.address) + region.storage->bytes.size()) {
      const size_t page = (address - region.address) / pageSize;
      const bool committed = region.storage->committed[page] != 0;
      size_t n = page + 1;
      while (n < region.storage->committed.size() && (region.storage->committed[n] != 0) == committed) { ++n; }
      return {result.base, uint64_t(region.address) + n * pageSize, region.address, true, committed};
    }
    if (region.address > address) { result.end = std::min(result.end, uint64_t(region.address)); }
  }
  return result;
}

Memory::Usage Memory::QueryUsage() const {
  std::lock_guard lock(memoryLock_);
  Usage usage{uint32_t(physicalStorage_->committed.size()), 0};
  for (size_t n = 0; n < regions_.size(); ++n) {
    const auto& storage = regions_[n].storage;
    if (storage == physicalStorage_) { continue; }
    const bool alias = std::any_of(regions_.begin(), regions_.begin() + n,
      [&](const auto& previous) { return previous.storage == storage; });
    if (!alias) {
      usage.privateCommittedPages += uint32_t(std::count_if(storage->committed.begin(), storage->committed.end(),
        [](uint8_t committed) { return committed != 0; }));
    }
  }
  return usage;
}

uint32_t Memory::CommittedPages(uint32_t address, uint32_t size) const {
  std::lock_guard lock(memoryLock_);
  const uint64_t end = uint64_t(address) + size;
  if (end > (1ull << 32)) { throw Fault({}, address, size, "committed usage range wraparound"); }
  uint32_t pages = 0;
  for (const auto& region : regions_) {
    const uint64_t start = std::max(uint64_t(address), uint64_t(region.address));
    const uint64_t stop = std::min(end, uint64_t(region.address) + region.storage->bytes.size());
    if (start >= stop) { continue; }
    const size_t first = size_t(start - region.address) / pageSize;
    const size_t limit = size_t(stop - region.address + pageSize - 1) / pageSize;
    pages += uint32_t(std::count_if(region.storage->committed.begin() + first, region.storage->committed.begin() + limit,
      [](uint8_t committed) { return committed != 0; }));
  }
  return pages;
}

bool Memory::IsCommitted(uint32_t address) const {
  std::lock_guard lock(memoryLock_);
  for (const auto& region : regions_) {
    if (address >= region.address && uint64_t(address) < uint64_t(region.address) + region.storage->bytes.size()) {
      return region.storage->committed[(address - region.address) / pageSize] != 0;
    }
  }
  return false;
}

void Memory::DecommitMapped(uint32_t address, uint32_t size) {
  std::lock_guard lock(memoryLock_);
  RequireMutable(address, size);
  const uint64_t end = uint64_t(address) + size;
  if (end > (1ull << 32)) { throw Fault({}, address, size, "decommit range wraparound"); }
  for (const auto& region : regions_) {
    const uint64_t start = std::max(uint64_t(address), uint64_t(region.address));
    const uint64_t stop = std::min(end, uint64_t(region.address) + region.storage->bytes.size());
    if (start < stop) { Decommit(uint32_t(start), uint32_t(stop - start)); }
  }
}

Memory::Region& Memory::RegionAt(const X86Context& ctx, uint32_t address, size_t size) {
  for (auto& region : regions_) {
    if (address >= region.address && uint64_t(address) < uint64_t(region.address) + region.storage->bytes.size()) { return region; }
  }
  throw Fault(ctx, address, size, "unmapped guest memory");
}

void Memory::Validate(const X86Context& ctx, uint32_t address, size_t size) {
  if (size > (1ull << 32) - address) { throw Fault(ctx, address, size, "guest address wraparound"); }
  const uint32_t original = address;
  const size_t total = size;
  while (size) {
    auto& region = RegionAt(ctx, address, total);
    const size_t offset = address - region.address;
    const size_t count = std::min(size, region.storage->bytes.size() - offset);
    for (size_t n = offset / pageSize; n <= (offset + count - 1) / pageSize; ++n) {
      if (!region.storage->committed[n]) { throw Fault(ctx, original, total, "uncommitted guest memory"); }
    }
    address += uint32_t(count);
    size -= count;
  }
}

uint8_t* Memory::Span(const X86Context& ctx, uint32_t address, size_t size) {
  std::lock_guard lock(memoryLock_);
  if (!leases_) { throw Fault(ctx, address, size, "host span requires a backing lease"); }
  Validate(ctx, address, size);
  auto& region = RegionAt(ctx, address, size);
  const size_t offset = address - region.address;
  if (size > region.storage->bytes.size() - offset) { throw Fault(ctx, address, size, "noncontiguous host span"); }
  return region.storage->bytes.data() + offset;
}

uint32_t Memory::GuestAddress(const X86Context& ctx, const void* pointer, size_t size) {
  std::lock_guard lock(memoryLock_);
  const uintptr_t host = reinterpret_cast<uintptr_t>(pointer);
  for (const auto& region : regions_) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(region.storage->bytes.data());
    if (host >= start && host - start < region.storage->bytes.size()) {
      const uint32_t address = region.address + uint32_t(host - start);
      Validate(ctx, address, size);
      return address;
    }
  }
  throw Fault(ctx, 0, size, "host pointer has no guest backing");
}

void Memory::CheckRange(const X86Context& ctx, uint32_t address, size_t size) {
  std::lock_guard lock(memoryLock_);
  Validate(ctx, address, size);
}

void Memory::Read(const X86Context& ctx, uint32_t address, void* output, size_t size) {
  std::lock_guard lock(memoryLock_);
  Validate(ctx, address, size);
  auto* destination = static_cast<uint8_t*>(output);
  while (size) {
    auto& region = RegionAt(ctx, address, size);
    const size_t offset = address - region.address;
    const size_t count = std::min(size, region.storage->bytes.size() - offset);
    std::memcpy(destination, region.storage->bytes.data() + offset, count);
    address += uint32_t(count);
    destination += count;
    size -= count;
  }
}

void Memory::Write(const X86Context& ctx, uint32_t address, const void* input, size_t size) {
  std::lock_guard lock(memoryLock_);
  Validate(ctx, address, size);
  const auto* source = static_cast<const uint8_t*>(input);
  while (size) {
    auto& region = RegionAt(ctx, address, size);
    const size_t offset = address - region.address;
    const size_t count = std::min(size, region.storage->bytes.size() - offset);
    std::memcpy(region.storage->bytes.data() + offset, source, count);
    address += uint32_t(count);
    source += count;
    size -= count;
  }
}

void Memory::Copy(const X86Context& ctx, uint32_t destination, uint32_t source, size_t size) {
  std::lock_guard lock(memoryLock_);
  Validate(ctx, source, size);
  Validate(ctx, destination, size);
  if (size > (8u << 20)) { throw Fault(ctx, destination, size, "prototype copy scratch budget exhausted"); }
  std::vector<uint8_t> snapshot(size);
  Read(ctx, source, snapshot.data(), size);
  Write(ctx, destination, snapshot.data(), size);
}

void Memory::Pages(uint32_t address, uint32_t size, bool committed) {
  std::lock_guard lock(memoryLock_);
  RequireMutable(address, size);
  if (!size) { return; }
  if (uint64_t(address) + size > (1ull << 32)) { throw Fault({}, address, size, "page range wraparound"); }
  const uint64_t end = (uint64_t(address) + size + pageSize - 1) & ~uint64_t(pageSize - 1);
  uint64_t cursor = address & ~(pageSize - 1);
  for (uint64_t n = cursor; n < end; n += pageSize) { RegionAt({}, uint32_t(n), pageSize); }
  while (cursor < end) {
    auto& region = RegionAt({}, uint32_t(cursor), pageSize);
    if (region.address != 0x80000000 && region.address != 0xF0000000) {
      const size_t offset = size_t(cursor - region.address);
      auto& state = region.storage->committed[offset / pageSize];
      if (state != committed) {
        std::memset(region.storage->bytes.data() + offset, 0, pageSize);
        state = committed;
      }
    }
    cursor += pageSize;
  }
}

void Memory::Commit(uint32_t address, uint32_t size) { Pages(address, size, true); }
void Memory::Decommit(uint32_t address, uint32_t size) { Pages(address, size, false); }

size_t Memory::BackingBytes() const {
  std::lock_guard lock(memoryLock_);
  size_t bytes = 0;
  for (size_t n = 0; n < regions_.size(); ++n) {
    bool alias = false;
    for (size_t previous = 0; previous < n; ++previous) { alias |= regions_[previous].storage == regions_[n].storage; }
    if (!alias) { bytes += regions_[n].storage->bytes.size() + regions_[n].storage->committed.size(); }
  }
  return bytes;
}

thread_local Runtime* Runtime::worker_ = nullptr;
thread_local X86Context* Runtime::workerContext_ = nullptr;
thread_local uint32_t Runtime::workerKpcr_ = 0;
thread_local uint64_t Runtime::workerId_ = 0;
thread_local Runtime* Runtime::clock_ = nullptr;
thread_local bool Runtime::polling_ = false;

Runtime::Runtime(std::span<const X86FuncMapping> mappings, std::span<const uint32_t> continuations,
    std::function<uint64_t()> timerSource)
  : mappings_(mappings.begin(), mappings.end()), continuations_(continuations.begin(), continuations.end()), kernels_(400),
    timerSource_(timerSource ? std::move(timerSource) : [origin = std::chrono::steady_clock::now()] {
      return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - origin).count()) / 100;
    }) {
  std::sort(mappings_.begin(), mappings_.end(), [](const auto& left, const auto& right) { return left.guest < right.guest; });
  for (size_t n = 0; n < mappings_.size(); ++n) {
    if (!mappings_[n].host || (n && mappings_[n - 1].guest == mappings_[n].guest) || mappings_[n].guest >= 0xFD000000) {
      throw Fault({}, mappings_[n].guest, 0, "invalid dispatch metadata");
    }
  }
  for (uint32_t target : continuations_) {
    if (!Find(target)) { throw Fault({}, target, 0, "continuation has no body"); }
  }
}

Runtime::~Runtime() { Stop(); }

void Runtime::Launch(X86Context ctx, uint8_t* base, std::function<void(X86Context&, uint8_t*)> entry) {
  Checkpoint(ctx);
  if (!entry || !base) { throw Fault(ctx, ctx.last_icall, 0, "invalid guest worker entry"); }
  std::lock_guard lock(workersLock_);
  Checkpoint(ctx);
  if (auto* memory = imageMemory_.load(); memory && (ctx.host != this || base != memory->Base())) {
    throw Fault(ctx, ctx.last_icall, 0, "guest worker belongs to a different image session");
  }
  std::erase_if(workers_, [&](const auto& worker) {
    if (!worker->done.load()) { return false; }
    worker->thread.join();
    if (!failure_) { failure_ = worker->failure; }
    return true;
  });
  if (activeWorkers_.load() >= 2) { throw Fault(ctx, ctx.last_icall, 0, "guest worker capacity exhausted"); }
  workers_.reserve(workers_.size() + 1);
  auto worker = std::make_unique<Worker>();
  auto* state = worker.get();
  ++activeWorkers_;
  try {
    worker->thread = std::thread([this, state, ctx, base, entry, workerId = ++workerSequence_]() mutable {
      Runtime* previous = worker_;
      auto* previousContext = workerContext_;
      const uint32_t previousKpcr = workerKpcr_;
      const uint64_t previousId = workerId_;
      worker_ = this;
      workerContext_ = &ctx;
      workerKpcr_ = ctx.fs_base;
      workerId_ = workerId;
      try {
        auto clock = BindClock();
        ctx.host = this;
        Checkpoint(ctx);
        StartTimer();
        entry(ctx, base);
      } catch (const Cancelled&) {
      } catch (...) {
        state->failure = std::current_exception();
        Cancel();
      }
      worker_ = previous;
      workerContext_ = previousContext;
      workerKpcr_ = previousKpcr;
      workerId_ = previousId;
      --activeWorkers_;
      state->done.store(true);
    });
  } catch (...) {
    --activeWorkers_;
    throw;
  }
  workers_.push_back(std::move(worker));
}

std::exception_ptr Runtime::Join() {
  if (Memory::HasBorrow()) { throw Fault({}, 0, 0, "joining guest workers during host backing lease"); }
  if (worker_ == this) { throw Fault({}, 0, 0, "guest worker cannot join its own runtime"); }
  std::lock_guard joining(joiningLock_);
  for (;;) {
    std::vector<std::unique_ptr<Worker>> workers;
    {
      std::lock_guard lock(workersLock_);
      if (workers_.empty()) { return failure_; }
      workers.swap(workers_);
    }
    for (auto& worker : workers) { worker->thread.join(); }
    std::lock_guard lock(workersLock_);
    for (auto& worker : workers) {
      if (!failure_) { failure_ = worker->failure; }
    }
  }
}

std::exception_ptr Runtime::Stop() {
  Cancel();
  return Join();
}

void Runtime::Checkpoint(const X86Context& ctx) const {
#ifdef __EMSCRIPTEN__
  const size_t base = emscripten_stack_get_base();
  const size_t current = emscripten_stack_get_current();
  const size_t end = emscripten_stack_get_end();
  if (current < end || current > base) { throw Fault(ctx, ctx.last_icall, 0, "host worker stack bounds exceeded"); }
  const size_t used = base - current;
  const size_t capacity = base - end;
  size_t peak = stackPeak_.load();
  while (used > peak && !stackPeak_.compare_exchange_weak(peak, used)) {}
  size_t budget = stackBudget_.load();
  while (capacity < budget && !stackBudget_.compare_exchange_weak(budget, capacity)) {}
#endif
  if (cancelled_.load()) { throw Cancelled(ctx); }
}

void Runtime::Cancel() {
  {
    std::lock_guard lock(waitLock_);
    cancelled_.store(true);
  }
  completion_.notify_all();
}

uint64_t Runtime::Sequence() {
  std::lock_guard lock(waitLock_);
  return sequence_;
}

void Runtime::Wake() {
  {
    std::lock_guard lock(waitLock_);
    ++sequence_;
  }
  completion_.notify_all();
}

Runtime::WaitResult Runtime::Wait(const X86Context& ctx, uint64_t sequence, std::chrono::milliseconds timeout) {
  if (Memory::HasBorrow()) { throw Fault(ctx, ctx.last_icall, 0, "guest wait during host backing lease"); }
  if (timeout < std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(30)) {
    throw Fault(ctx, ctx.last_icall, 0, "invalid guest wait budget");
  }
  std::unique_lock lock(waitLock_);
  Checkpoint(ctx);
  const bool changed = completion_.wait_for(lock, timeout, [&] { return cancelled_.load() || sequence_ != sequence; });
  Checkpoint(ctx);
  return changed ? WaitResult::Changed : WaitResult::TimedOut;
}

Runtime& Runtime::From(X86Context& ctx) {
  if (!ctx.host) { throw Fault(ctx, ctx.last_icall, 0, "missing CPU runtime"); }
  return *static_cast<Runtime*>(ctx.host);
}

X86Func* Runtime::Find(uint32_t target) const {
  const auto entry = std::lower_bound(mappings_.begin(), mappings_.end(), target,
    [](const auto& mapping, uint32_t address) { return mapping.guest < address; });
  return entry != mappings_.end() && entry->guest == target ? entry->host : nullptr;
}

void Runtime::RequireImageSession(const X86Context& ctx, uint8_t* base) const {
  if (auto* memory = imageMemory_.load(); memory && (ctx.host != this || base != memory->Base())) {
    throw Fault(ctx, ctx.last_icall, 0, "guest invocation belongs to a different image session");
  }
}

void Runtime::Invoke(X86Context& ctx, uint8_t* base, uint32_t target) {
  Checkpoint(ctx);
  RequireImageSession(ctx, base);
  Poll(ctx, base);
  ctx.last_icall = target;
  if (std::find(continuations_.begin(), continuations_.end(), target) != continuations_.end()) { throw X86Resume{target, ctx.esp}; }
  if (auto* function = Find(target)) { function(ctx, base); return; }
  if (target >= 0xFE000000 && target < 0xFE000640 && !(target & 3)) { Kernel(ctx, base, (target - 0xFE000000) / 4); return; }
  X86Func* function = nullptr;
  if (target >= 0xFD000000 && target < 0xFE000000 && !(target & 3)) {
    std::lock_guard lock(lock_);
    const size_t index = (target - 0xFD000000) / 4;
    if (index < hosts_.size()) { function = hosts_[index]; }
  }
  if (!function) { throw Fault(ctx, target, 0, "unknown indirect target"); }
  function(ctx, base);
}

void Runtime::Resume(X86Context& ctx, uint8_t* base, uint32_t target, uint32_t esp) {
  Checkpoint(ctx);
  RequireImageSession(ctx, base);
  auto* function = Find(target);
  if (!function || std::find(continuations_.begin(), continuations_.end(), target) == continuations_.end()) {
    throw Fault(ctx, target, 0, "unknown continuation target");
  }
  ctx.esp = esp;
  Poll(ctx, base);
  function(ctx, base);
}

uint32_t Runtime::Call(X86Context& ctx, uint8_t* base, uint32_t target, std::initializer_list<uint32_t> args) {
  Checkpoint(ctx);
  RequireImageSession(ctx, base);
  const std::array<uint32_t, 8> saved{ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esp, ctx.ebp, ctx.esi, ctx.edi};
  for (auto argument = args.end(); argument != args.begin();) { X86_PUSH32(*--argument); }
  X86_PUSH32(0xFFFFFFF0);
  Invoke(ctx, base, target);
  const uint32_t result = ctx.eax;
  ctx.eax = saved[0]; ctx.ecx = saved[1]; ctx.edx = saved[2]; ctx.ebx = saved[3];
  ctx.esp = saved[4]; ctx.ebp = saved[5]; ctx.esi = saved[6]; ctx.edi = saved[7];
  return result;
}

uint32_t Runtime::HostThunk(X86Func* function) {
  std::lock_guard lock(lock_);
  if (!function) { throw Fault({}, 0, 0, "null host thunk"); }
  auto entry = std::find(hosts_.begin(), hosts_.end(), function);
  if (entry != hosts_.end()) { return 0xFD000000 + uint32_t(entry - hosts_.begin()) * 4; }
  if (hosts_.size() >= 0x400000) { throw Fault({}, 0, 0, "host thunk budget exhausted"); }
  hosts_.push_back(function);
  return 0xFD000000 + uint32_t(hosts_.size() - 1) * 4;
}

void Runtime::RegisterKernel(uint32_t ordinal, std::function<void(X86Context&, uint8_t*)> function) {
  std::scoped_lock lock(workersLock_, lock_);
  Checkpoint({});
  if (!ordinal || ordinal >= kernels_.size() || !function) { throw Fault({}, ordinal, 0, "invalid kernel registration"); }
  if (imageMemory_.load() || activeWorkers_.load() || kernels_[ordinal]) {
    throw Fault({}, ordinal, 0, "kernel registration requires an unpublished stopped runtime and an unused ordinal");
  }
  kernels_[ordinal] = std::move(function);
  ++kernelRevision_;
}

uint64_t Runtime::KernelRevision() {
  std::lock_guard lock(lock_);
  return kernelRevision_;
}

void Runtime::PublishImageMemory(Memory& memory, uint64_t revision, const std::function<void()>& publish) {
  std::scoped_lock lock(workersLock_, lock_);
  Checkpoint({});
  if (!publish || imageMemory_.load() || activeWorkers_.load() || kernelRevision_ != revision || (sdkMemory_ && sdkMemory_ != &memory)) {
    throw Fault({}, 0, 0, "image publication requires an unchanged unpublished stopped runtime");
  }
  publish();
  imageMemory_.store(&memory);
}

uint32_t Runtime::KernelTarget(uint32_t ordinal) {
  std::lock_guard lock(lock_);
  return ordinal && ordinal < kernels_.size() && kernels_[ordinal] ? 0xFE000000 + ordinal * 4 : 0;
}

void Runtime::Kernel(X86Context& ctx, uint8_t* base, uint32_t ordinal) {
  Checkpoint(ctx);
  RequireImageSession(ctx, base);
  const std::function<void(X86Context&, uint8_t*)>* function = nullptr;
  {
    std::lock_guard lock(lock_);
    if (ordinal < kernels_.size() && kernels_[ordinal]) { function = &kernels_[ordinal]; }
  }
  if (!function) { throw Fault(ctx, ordinal, 0, "unavailable kernel ordinal"); }
  RecordProgress(ctx);
  const uint64_t callId = RecordKernelTrace(ctx, ordinal, KernelTracePhase::Before, 0);
  try {
    (*function)(ctx, base);
  } catch (...) {
    RecordKernelTrace(ctx, ordinal, KernelTracePhase::Fault, callId);
    throw;
  }
  RecordKernelTrace(ctx, ordinal, KernelTracePhase::After, callId);
  RecordProgress(ctx);
}

uint64_t Runtime::RecordKernelTrace(const X86Context& ctx, uint32_t ordinal, KernelTracePhase phase, uint64_t callId) {
  std::lock_guard lock(kernelTraceLock_);
  const uint64_t sequence = ++kernelTraceTotal_;
  kernelTrace_[(sequence - 1) % kernelTrace_.size()] = {
    sequence, callId ? callId : sequence, worker_ == this ? workerId_ : 0,
    ordinal, ctx.fs_base, ctx.eax, ctx.esp, ctx.last_icall, phase
  };
  return sequence;
}

Runtime::KernelTraceSnapshot Runtime::SnapshotKernelTrace() const {
  std::lock_guard lock(kernelTraceLock_);
  KernelTraceSnapshot snapshot;
  snapshot.total = kernelTraceTotal_;
  snapshot.count = size_t(std::min<uint64_t>(kernelTraceTotal_, kernelTrace_.size()));
  const uint64_t start = kernelTraceTotal_ - snapshot.count;
  for (size_t n = 0; n < snapshot.count; ++n) {
    snapshot.events[n] = kernelTrace_[(start + n) % kernelTrace_.size()];
  }
  return snapshot;
}

void Runtime::InstallExpiryDriver(std::function<void(X86Context&, uint8_t*)> driver) {
  std::scoped_lock lock(workersLock_, lock_);
  Checkpoint({});
  if (!driver || expiryDriver_ || imageMemory_.load() || activeWorkers_.load()) {
    throw Fault({}, 149, 0, "expiry driver requires an unpublished stopped runtime");
  }
  expiryDriver_ = std::move(driver);
}

void Runtime::InstallSdkObserver(Memory& memory, std::function<bool(X86Context&, uint32_t, uint64_t)> observer) {
  std::scoped_lock lock(workersLock_, lock_);
  Checkpoint({});
  if (!observer || sdkObserver_ || imageMemory_.load() || workerSequence_ || Memory::HasBorrow()) {
    throw Fault({}, 0, 0, "SDK observer requires an unpublished cold runtime");
  }
  sdkMemory_ = &memory;
  sdkObserver_ = std::move(observer);
}

bool Runtime::ObserveSdkEntry(X86Context& ctx, uint8_t* base, uint32_t id) {
  auto* runtime = worker_;
  if (!runtime || workerContext_ != &ctx || !runtime->OwnsCurrentContext(ctx) || Memory::HasBorrow() || !runtime->sdkObserver_ ||
      !runtime->sdkMemory_ || runtime->imageMemory_.load() != runtime->sdkMemory_ || base != runtime->sdkMemory_->Base()) {
    throw Fault(ctx, ctx.esp, 0, "SDK observation requires its bound current image worker without a backing lease");
  }
  runtime->Checkpoint(ctx);
  return runtime->sdkObserver_(ctx, id, workerId_);
}

void Runtime::RecordProgress(const X86Context& ctx) {
  if (OwnsCurrentContext(ctx)) { progress_.fetch_add(1, std::memory_order_relaxed); }
}

void Runtime::Poll(X86Context& ctx, uint8_t* base) {
  Checkpoint(ctx);
  RequireImageSession(ctx, base);
  RecordProgress(ctx);
  if (!expiryDriver_ || polling_ || Memory::HasBorrow()) { return; }
  RequireImageSession(ctx, base);
  if (!OwnsCurrentContext(ctx)) { throw Fault(ctx, 149, 0, "expiry polling requires its current worker"); }
  struct Polling {
    bool& active;
    explicit Polling(bool& flag) : active(flag) { active = true; }
    ~Polling() { active = false; }
  } polling(polling_);
  expiryDriver_(ctx, base);
  Checkpoint(ctx);
}

uint64_t Runtime::SystemTime() {
  const auto elapsed = std::chrono::system_clock::now().time_since_epoch();
  const auto units = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
  if (units < 0 || uint64_t(units) > UINT64_MAX - 116444736000000000ull) {
    throw Fault({}, 149, 0, "host system time outside NT epoch range");
  }
  return uint64_t(units) + 116444736000000000ull;
}

void Runtime::StartTimer() {
  std::lock_guard lock(timerLock_);
  if (!timerEpoch_) { timerEpoch_ = timerSample_ = timerSource_(); }
}

uint64_t Runtime::TimerTime(const X86Context& ctx) {
  if (ctx.host != this || !OwnsCurrentWorker()) { throw Fault(ctx, 149, 0, "timer clock requires its owned runtime worker"); }
  Checkpoint(ctx);
  std::lock_guard lock(timerLock_);
  const uint64_t sample = timerSource_();
  if (!timerEpoch_ || sample < timerSample_) { throw Fault(ctx, 149, 0, "timer clock source moved backwards or has no epoch"); }
  timerSample_ = sample;
  return sample - *timerEpoch_;
}

uint64_t Runtime::RelativeTimerDeadline(const X86Context& ctx, int64_t dueTime) {
  if (dueTime >= 0) { throw Fault(ctx, 149, 0, "absolute timer deadlines require a system time epoch"); }
  const uint64_t now = TimerTime(ctx);
  const uint64_t delay = uint64_t(-(dueTime + 1)) + 1;
  if (delay > UINT64_MAX - now) { throw Fault(ctx, 149, 0, "relative timer deadline overflow"); }
  return now + delay;
}

uint64_t Runtime::Clock() {
  if (!clock_) { throw Fault({}, 0, 0, "guest clock not configured"); }
  return clock_->ticks.load();
}

}

void x86_icall(X86Context& ctx, uint8_t* base, uint32_t target) { browser::Runtime::From(ctx).Invoke(ctx, base, target); }
void x86_resume(X86Context& ctx, uint8_t* base, const X86Resume& resume) { browser::Runtime::From(ctx).Resume(ctx, base, resume.target, resume.esp); }
void x86_kernel(X86Context& ctx, uint8_t* base, uint32_t ordinal) { browser::Runtime::From(ctx).Kernel(ctx, base, ordinal); }
void x86_trap(X86Context& ctx, uint32_t address, const char* reason) { throw browser::Fault(ctx, address, 0, reason); }
void x86_priv(X86Context& ctx, uint32_t address, const char* reason) { throw browser::Fault(ctx, address, 0, reason); }
uint64_t x86_rdtsc() { return browser::Runtime::Clock(); }
void x86_cpuid(X86Context& ctx) {
  switch (ctx.eax) {
    case 0: ctx.eax = 2; ctx.ebx = 0x756E6547; ctx.edx = 0x49656E69; ctx.ecx = 0x6C65746E; break;
    case 1: ctx.eax = 0x0000068A; ctx.ebx = 0; ctx.ecx = 0; ctx.edx = 0x0383F9FF; break;
    default: ctx.eax = 0; ctx.ebx = 0; ctx.ecx = 0; ctx.edx = 0; break;
  }
}
