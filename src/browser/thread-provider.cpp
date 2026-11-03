#include "browser/thread-provider.h"
#include <algorithm>
#include <array>
#include <cstdio>

namespace browser {
namespace {

struct ThreadExit { uint32_t status; };
constexpr uint32_t kThreadObjectType = 0xc0005000;
constexpr uint32_t kThreadReferenceBudget = 1024;

class ThreadProvider {
  Runtime& runtime_;
  PreparedImage& image_;
  std::function<bool(uint32_t)> closeFile_;
  std::shared_ptr<ThreadSessionCounters> counters_;
  std::mutex lock_;
  struct Thread {
    std::mutex publication;
    uint32_t handle = 0;
    uint32_t kpcr = 0;
    bool closed = false;
    uint32_t references = 0;
    uint32_t priority = 0;
    std::atomic<int> suspend = 0;
    uint32_t waitObject = 0;
    std::shared_ptr<ThreadWaitBridge> waitBridge;
  };
  std::array<std::shared_ptr<Thread>, kImageThreadCapacity - 1> threads_{};
  size_t threadCount_ = 0;
  Thread initial_;

  Thread* FindObject(uint32_t object) {
    if (initial_.kpcr && object == initial_.kpcr + 0x400) { return &initial_; }
    for (size_t n = 0; n < threadCount_; ++n) {
      if (object == threads_[n]->kpcr + 0x400) { return threads_[n].get(); }
    }
    return nullptr;
  }

  void InitializeInitial() {
    if (initial_.kpcr) { return; }
    std::lock_guard lock(image_.threadLock);
    if (image_.threads.empty()) { throw Fault({}, 246, 0, "initial thread storage is absent"); }
    initial_.kpcr = image_.threads.front().kpcr;
  }

  template<size_t N>
  std::array<uint32_t, N> ReadFrame(const X86Context& ctx) {
    std::array<uint32_t, N> frame;
    if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) {
      throw Fault(ctx, ctx.esp, sizeof(frame), "thread service frame overflow");
    }
    image_.memory->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    return frame;
  }

  void RequireOwner(const X86Context& ctx, uint8_t* base) {
    if (ctx.host != &runtime_ || !image_.importsBound || image_.boundRuntime != &runtime_ ||
        base != image_.memory->Base() || !runtime_.OwnsCurrentContext(ctx)) {
      throw Fault(ctx, ctx.esp, 0, "thread service requires its owned image worker");
    }
  }

  void ParkSuspended(const std::shared_ptr<Thread>& state, X86Context& child) {
    for (;;) {
      runtime_.Checkpoint(child);
      const uint64_t sequence = runtime_.Sequence();
      if (state->suspend.load() <= 0) { return; }
      runtime_.Wait(child, sequence, std::chrono::seconds(30));
    }
  }

public:
  ThreadProvider(Runtime& runtime, PreparedImage& image, std::function<bool(uint32_t)> closeFile)
    : runtime_(runtime), image_(image), closeFile_(std::move(closeFile)), counters_(image.threadSessionCounters) {}

  void Create(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "thread creation during backing lease"); }
    std::array<uint32_t, 11> frame;
    auto& memory = *image_.memory;
    if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "thread frame overflow"); }
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    const uint32_t output = frame[1], extension = frame[2], stack = frame[3], tls = frame[4], idOutput = frame[5];
    const uint32_t start = frame[6], argument = frame[7], debugger = frame[9], system = frame[10];
    const uint32_t suspended = frame[8] & 0xFF;
    const uint32_t expectedTls = image_.image.tls_addr ? ((uint32_t(image_.tlsTemplate.size()) + 15) & ~15u) + 4 : 0;
    const auto reject = [&](const char* restriction) {
      std::array<char, 512> detail{};
      std::snprintf(detail.data(), detail.size(),
        "unsupported system thread creation arguments: %s "
        "return_address=0x%08X output=0x%08X extension=0x%08X stack=0x%08X tls=0x%08X expected_tls=0x%08X id_output=0x%08X "
        "start=0x%08X argument=0x%08X suspended=0x%X debugger=0x%X system=0x%08X",
        restriction, frame[0], output, extension, stack, tls, expectedTls, idOutput, start, argument, frame[8], debugger, system);
      throw Fault(ctx, 255, 0, detail.data());
    };
    if (!output) { reject("null thread handle output"); }
    if (extension) { reject("thread extension storage is not supported"); }
    if (debugger) { reject("debugger thread creation is not supported"); }
    if (tls != expectedTls) { reject("thread TLS data size mismatch"); }
    if (!runtime_.Find(start)) { reject("thread start routine has no generated body"); }
    if (system && !runtime_.Find(system)) { reject("thread system routine has no generated body"); }
    if (!image_.tlsCallbacks.empty()) { reject("image TLS callbacks are not integrated"); }
    if (idOutput && uint64_t(output) < uint64_t(idOutput) + 4 && uint64_t(idOutput) < uint64_t(output) + 4) {
      throw Fault(ctx, output, 4, "thread outputs overlap");
    }
    memory.CheckRange(ctx, output, 4);
    if (idOutput) { memory.CheckRange(ctx, idOutput, 4); }
    std::lock_guard lock(lock_);
    if (threadCount_ == threads_.size()) { throw Fault(ctx, 255, 0, "session thread storage capacity exhausted"); }
    auto state = std::make_shared<Thread>();
    state->handle = 0x70000004 + uint32_t(threadCount_) * 4;
    state->suspend = suspended ? 1 : 0;
    const uint32_t id = 0x14 + uint32_t(threadCount_) * 4;
    std::unique_lock publication(state->publication);
    state->waitBridge = image_.threadWaitBridge;
    if (state->waitBridge) { state->waitObject = state->waitBridge->publish(state->handle); }
    try {
      LaunchImageThread(image_, runtime_, ctx, id, stack, [&](const ImageThread& thread) {
        auto lease = memory.Borrow();
        auto* handleBytes = memory.Span(ctx, output, 4);
        auto* idBytes = idOutput ? memory.Span(ctx, idOutput, 4) : nullptr;
        state->kpcr = thread.kpcr;
        X86Context child = thread.context;
        child.thread_handle = state->handle;
        runtime_.Launch(child, base, [this, state, start, argument, system, suspended](X86Context& child, uint8_t* backing) {
          ImageThreadRetirement retirement{image_, child};
          struct SuspendedGauge {
            std::shared_ptr<ThreadSessionCounters> counters;
            ~SuspendedGauge() { if (counters) { --counters->childrenSuspended; } }
          } gauge{};
          { std::lock_guard ready(state->publication); }
          runtime_.Checkpoint(child);
          if (suspended) { ++counters_->childrenSuspended; gauge.counters = counters_; }
          ParkSuspended(state, child);
          if (gauge.counters) { --gauge.counters->childrenSuspended; gauge.counters.reset(); }
          try {
            const uint32_t status = system ? runtime_.Call(child, backing, system, {start, argument})
                                           : runtime_.Call(child, backing, start, {argument});
            runtime_.Checkpoint(child);
            CompleteImageThread(image_, runtime_, child, status);
            if (state->waitObject) { state->waitBridge->complete(state->waitObject); }
            ++counters_->childrenCompleted;
          } catch (const ThreadExit& exit) {
            runtime_.Checkpoint(child);
            CompleteImageThread(image_, runtime_, child, exit.status);
            if (state->waitObject) { state->waitBridge->complete(state->waitObject); }
            ++counters_->childrenCompleted;
          }
        });
        std::memcpy(handleBytes, &state->handle, 4);
        if (idBytes) { std::memcpy(idBytes, &thread.id, 4); }
        threads_[threadCount_] = state;
        ++threadCount_;
        ++counters_->childrenCreated;
        counters_->requestedStackBytes += stack;
        counters_->committedBytes += 0x1000 + (thread.stackTop - thread.stackLimit);
        ctx.eax = 0;
        ctx.esp += sizeof(frame);
      });
    } catch (...) {
      if (state->waitObject) { state->waitBridge->rollback(state->handle, state->waitObject); }
      throw;
    }
  }

  void Reference(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    const auto frame = ReadFrame<4>(ctx);
    std::lock_guard lock(lock_);
    InitializeInitial();
    Thread* state = nullptr;
    if (frame[1] == 0xfffffffe) {
      state = FindObject(ctx.fs_base + 0x400);
    } else {
      for (size_t n = 0; n < threadCount_; ++n) {
        if (threads_[n]->handle == frame[1] && !threads_[n]->closed) { state = threads_[n].get(); break; }
      }
    }
    if (!state) {
      ctx.eax = 0xc0000008;
    } else if (frame[2] && frame[2] != kThreadObjectType) {
      ctx.eax = 0xc0000024;
    } else {
      if (state->references == kThreadReferenceBudget) { throw Fault(ctx, 246, 0, "thread reference budget exhausted"); }
      if (!frame[3]) { throw Fault(ctx, 0, 4, "null thread object output"); }
      const uint32_t object = state->kpcr + 0x400;
      auto lease = image_.memory->Borrow();
      image_.memory->Write(ctx, frame[3], &object, 4);
      ++state->references;
      ++counters_->objectReferences;
      ctx.eax = 0;
    }
    ctx.esp += sizeof(frame);
  }

  void Priority(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    const auto frame = ReadFrame<3>(ctx);
    std::lock_guard lock(lock_);
    InitializeInitial();
    auto* state = FindObject(frame[1]);
    if (!state || (!state->references && state->closed)) {
      throw Fault(ctx, frame[1], 0, "priority requires an owned thread object");
    }
    ctx.eax = state->priority;
    state->priority = frame[2];
    ctx.esp += sizeof(frame);
  }

  void Dereference(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    const auto frame = ReadFrame<1>(ctx);
    std::lock_guard lock(lock_);
    InitializeInitial();
    auto* state = FindObject(ctx.ecx);
    if (!state || !state->references) { throw Fault(ctx, ctx.ecx, 0, "release requires an owned thread reference"); }
    --state->references;
    --counters_->objectReferences;
    ctx.eax = 0;
    ctx.esp += sizeof(frame);
  }

  bool Contains(uint32_t handle) {
    std::lock_guard lock(lock_);
    return std::any_of(threads_.begin(), threads_.begin() + threadCount_, [&](const auto& state) {
      return state->handle == handle && !state->closed;
    });
  }
  void Close(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    std::array<uint32_t, 2> frame;
    if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "close frame overflow"); }
    image_.memory->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    std::lock_guard lock(lock_);
    const auto thread = std::find_if(threads_.begin(), threads_.begin() + threadCount_,
      [&](const auto& state) { return state->handle == frame[1]; });
    const bool managed = thread != threads_.begin() + threadCount_;
    const bool live = managed && !(*thread)->closed;
    if (live) {
      if ((*thread)->waitObject) { (*thread)->waitBridge->close((*thread)->handle); }
      (*thread)->closed = true;
      ++counters_->handlesClosed;
    }
    ctx.eax = live || (!managed && closeFile_ && closeFile_(frame[1])) ? 0 : 0xc0000008;
    ctx.esp += sizeof(frame);
  }

  void Resume(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    const auto frame = ReadFrame<3>(ctx);
    auto& memory = *image_.memory;
    if (frame[2]) { memory.CheckRange(ctx, frame[2], 4); }
    bool resumed = false;
    {
      std::lock_guard lock(lock_);
      const auto thread = std::find_if(threads_.begin(), threads_.begin() + threadCount_,
        [&](const auto& state) { return state->handle == frame[1] && !state->closed; });
      if (thread == threads_.begin() + threadCount_) {
        ctx.eax = 0xc0000008;
      } else {
        const int previous = (*thread)->suspend--;
        if (frame[2]) {
          auto lease = memory.Borrow();
          memory.Write(ctx, frame[2], &previous, 4);
        }
        resumed = true;
        ctx.eax = 0;
      }
    }
    if (resumed) { runtime_.Wake(); }
    ctx.esp += sizeof(frame);
  }

  void Terminate(X86Context& ctx, uint8_t* base) {
    RequireOwner(ctx, base);
    std::array<uint32_t, 2> frame;
    if (uint64_t(ctx.esp) + sizeof(frame) > UINT32_MAX) { throw Fault(ctx, ctx.esp, sizeof(frame), "termination frame overflow"); }
    image_.memory->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    std::lock_guard lock(lock_);
    const auto thread = std::find_if(threads_.begin(), threads_.begin() + threadCount_,
      [&](const auto& state) { return state->kpcr == ctx.fs_base && state->handle == ctx.thread_handle; });
    if (thread == threads_.begin() + threadCount_) { throw Fault(ctx, 258, 0, "termination requires a managed child thread"); }
    throw ThreadExit{frame[1]};
  }
};

}

ThreadSessionSnapshot SnapshotThreadSession(const PreparedImage& image) {
  ThreadSessionSnapshot snapshot;
  const auto& counters = image.threadSessionCounters;
  if (!counters) { return snapshot; }
  snapshot.childrenCreated = counters->childrenCreated.load();
  snapshot.childrenCompleted = counters->childrenCompleted.load();
  snapshot.handlesClosed = counters->handlesClosed.load();
  snapshot.childrenSuspended = counters->childrenSuspended.load();
  snapshot.objectReferences = counters->objectReferences.load();
  snapshot.requestedStackBytes = counters->requestedStackBytes.load();
  snapshot.committedBytes = counters->committedBytes.load();
  return snapshot;
}

KernelDataExport InstallThreadProvider(Runtime& runtime, PreparedImage& image, std::function<bool(uint32_t)> closeFile,
  std::function<bool(uint32_t)>* ownsHandle) {
  runtime.Checkpoint({});
  if (!image.memory || image.importsBound || image.boundRuntime || runtime.ActiveWorkers() ||
      runtime.HasPublishedImage() || Memory::HasBorrow() || image.threadProviderRuntime ||
      (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) ||
      (image.synchronizationProviderRuntime && image.synchronizationProviderRuntime != &runtime)) {
    throw Fault({}, 259, 32, "thread provider requires its unpublished stopped image");
  }
  for (uint32_t ordinal : {143u, 187u, 224u, 246u, 250u, 255u, 258u, 259u}) {
    if (runtime.KernelTarget(ordinal)) { throw Fault({}, ordinal, 0, "thread provider export already installed"); }
  }
  image.threadSessionCounters = std::make_shared<ThreadSessionCounters>();
  auto provider = std::make_shared<ThreadProvider>(runtime, image, std::move(closeFile));
  if (ownsHandle) {
    *ownsHandle = [weak = std::weak_ptr(provider)](uint32_t handle) {
      const auto owner = weak.lock();
      return owner && owner->Contains(handle);
    };
  }
  auto mutation = image.memory->Mutate();
  if (image.memory->QueryPages(kThreadObjectType).mapped) {
    throw Fault({}, kThreadObjectType, 32, "thread object type requires fresh owned storage");
  }
  image.memory->Map(kThreadObjectType, 4096);
  struct PendingMapping {
    Memory& memory;
    bool installed = false;
    ~PendingMapping() { if (!installed) { memory.Discard(kThreadObjectType, 4096); } }
  } pending{*image.memory};
  image.memory->Commit(kThreadObjectType, 4096);
  constexpr std::array<uint8_t, 32> metadata{};
  image.memory->Write({}, kThreadObjectType, metadata.data(), metadata.size());
  runtime.RegisterKernel(246, [provider](X86Context& ctx, uint8_t* base) { provider->Reference(ctx, base); });
  runtime.RegisterKernel(143, [provider](X86Context& ctx, uint8_t* base) { provider->Priority(ctx, base); });
  runtime.RegisterKernel(250, [provider](X86Context& ctx, uint8_t* base) { provider->Dereference(ctx, base); });
  runtime.RegisterKernel(255, [provider](X86Context& ctx, uint8_t* base) { provider->Create(ctx, base); });
  runtime.RegisterKernel(187, [provider](X86Context& ctx, uint8_t* base) { provider->Close(ctx, base); });
  runtime.RegisterKernel(224, [provider](X86Context& ctx, uint8_t* base) { provider->Resume(ctx, base); });
  runtime.RegisterKernel(258, [provider](X86Context& ctx, uint8_t* base) { provider->Terminate(ctx, base); });
  image.threadProviderRuntime = &runtime;
  pending.installed = true;
  return {259, kThreadObjectType, uint32_t(metadata.size())};
}

}
