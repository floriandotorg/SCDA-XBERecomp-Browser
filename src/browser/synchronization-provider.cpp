#include "browser/synchronization-provider.h"
#include "browser/dispatcher-object-abi.h"
#include <algorithm>
#include <cstdio>

namespace browser {
namespace {

class SynchronizationProvider {
  Runtime& runtime_;
  PreparedImage& image_;
  std::shared_ptr<SynchronizationHandleRoutes> routes_;
  Dispatcher dispatcher_;
  DispatcherHandles handles_;
public:
  SynchronizationProvider(Runtime& runtime, PreparedImage& image, std::shared_ptr<SynchronizationHandleRoutes> routes)
    : runtime_(runtime), image_(image), routes_(std::move(routes)), dispatcher_(runtime, std::nullopt, true, false), handles_(dispatcher_) {}

  ~SynchronizationProvider() { dispatcher_.Disconnect(); }

  bool Close(uint32_t handle) {
    if (handle >= 0x70000000) { return false; }
    if (!handles_.Contains(handle)) { return false; }
    try { handles_.Close(handle); }
    catch (const DispatcherError& error) {
      if (error.status == 0xc0000008) { return false; }
      throw;
    }
    return true;
  }

  void Retire(uint32_t owner) { dispatcher_.AbandonOwner(owner); }
  uint32_t PublishThread(uint32_t handle) { return handles_.PublishThread(handle); }
  void RollbackThread(uint32_t handle, uint32_t token) { handles_.RollbackThread(handle, token); }
  void CompleteThread(uint32_t token) { dispatcher_.CompleteThread(token); }
  void CloseThread(uint32_t handle) { handles_.Close(handle); }
  std::function<void()> BeginFileIo(uint32_t handle) { return handles_.BeginFileIo(handle); }

  void Invoke(X86Context& ctx, uint8_t* base, uint32_t ordinal) {
    runtime_.Checkpoint(ctx);
    if (ctx.host != &runtime_ || !image_.importsBound || image_.boundRuntime != &runtime_ ||
        base != image_.memory->Base() || !runtime_.OwnsCurrentContext(ctx)) {
      throw Fault(ctx, ordinal, 0, "synchronization requires its owned image worker");
    }
    {
      std::lock_guard lock(image_.threadLock);
      if (std::none_of(image_.threads.begin(), image_.threads.end(), [&](const auto& thread) { return thread.kpcr == ctx.fs_base; })) {
        throw Fault(ctx, ctx.fs_base, 0, "synchronization requires its owned thread identity");
      }
    }
    const size_t arguments = ordinal == 186 ? 1 : ordinal == 192 || ordinal == 233 ? 3 : ordinal == 189 || ordinal == 234 ? 4 :
      ordinal == 235 ? 6 : 2;
    const uint64_t end = uint64_t(ctx.esp) + (arguments + 1) * 4;
    if (end > UINT32_MAX) { throw Fault(ctx, ctx.esp, (arguments + 1) * 4, "synchronization frame overflow"); }
    std::array<uint32_t, 7> frame{};
    std::array<uint32_t, 64> guarded{};
    {
      auto lease = image_.memory->Borrow();
      image_.memory->Read(ctx, ctx.esp, frame.data(), (arguments + 1) * 4);
      if (ordinal == 235) {
        if (frame[1] == 0 || frame[1] > guarded.size()) { throw Fault(ctx, frame[2], frame[1], "invalid guest handle array count"); }
        if (frame[2] == 0) { throw Fault(ctx, 0, frame[1] * 4, "null guest handle array"); }
        image_.memory->Read(ctx, frame[2], guarded.data(), frame[1] * 4);
      }
    }
    X86Context owned = ctx;
    owned.thread_handle = ctx.fs_base;
    try {
      if (ordinal == 233 || ordinal == 234) {
        if (routes_ && routes_->ownsFile && routes_->ownsFile(frame[1])) {
          throw Fault(ctx, ordinal, 0, "wait on owned file handle is not implemented");
        }
        const bool extended = ordinal == 234;
        if ((extended && (frame[2] & 0xFF) > 1) || (frame[extended ? 3 : 2] & 0xFF) > 1) {
          char reason[160];
          std::snprintf(reason, sizeof(reason),
            "unsupported wait processor mode or alertable flags ordinal=%u handle=0x%08x mode=0x%08x alertable=0x%08x timeout=0x%08x",
            ordinal, frame[1], extended ? frame[2] : 0, frame[extended ? 3 : 2], frame[extended ? 4 : 3]);
          throw Fault(ctx, ordinal, 0, reason);
        }
        owned.eax = handles_.WaitGuestStatus(*image_.memory, owned, std::span(&frame[1], 1), Dispatcher::WaitMode::Any,
          frame[extended ? 4 : 3], (frame[extended ? 3 : 2] & 0xFF) != 0, std::chrono::seconds(30));
        owned.esp = uint32_t(end);
      } else if (ordinal == 235) {
        if (frame[5] & 0xFF) { throw Fault(ctx, 0, 0, "alertable guest waits are not implemented"); }
        if ((frame[4] & 0xFF) > 1) { throw Fault(ctx, 0, 0, "invalid guest wait processor mode"); }
        if (frame[3] > 1) { throw Fault(ctx, 0, 0, "invalid guest wait type"); }
        for (size_t n = 0; n < frame[1]; ++n) {
          if (routes_ && routes_->ownsFile && routes_->ownsFile(guarded[n])) {
            throw Fault(ctx, ordinal, 0, "wait on owned file handle is not implemented");
          }
        }
        owned.eax = handles_.WaitGuestStatus(*image_.memory, owned, std::span(guarded.data(), frame[1]),
          frame[3] == 0 ? Dispatcher::WaitMode::All : Dispatcher::WaitMode::Any, frame[6], false, std::chrono::seconds(30));
        owned.esp = uint32_t(end);
      } else {
        InvokeDispatcherObject(handles_, *image_.memory, owned, ordinal);
      }
      ctx.eax = owned.eax;
      ctx.esp = owned.esp;
    } catch (const DispatcherError& error) {
      ctx.eax = error.status;
      ctx.esp = uint32_t(end);
    }
  }
};

}

std::function<bool(uint32_t)> InstallSynchronizationProvider(Runtime& runtime, PreparedImage& image,
  std::shared_ptr<SynchronizationHandleRoutes> routes) {
  runtime.Checkpoint({});
  if (!image.memory || image.importsBound || image.boundRuntime || runtime.ActiveWorkers() || runtime.HasPublishedImage() ||
      Memory::HasBorrow() || image.synchronizationProviderRuntime || image.threadRetirement ||
      (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) ||
      (image.threadProviderRuntime && image.threadProviderRuntime != &runtime)) {
    throw Fault({}, 192, 0, "synchronization provider requires its unpublished stopped image");
  }
  for (const uint32_t ordinal : {186u, 189u, 192u, 221u, 225u, 233u, 234u, 235u}) {
    if (runtime.KernelTarget(ordinal)) { throw Fault({}, ordinal, 0, "synchronization export already installed"); }
  }
  auto provider = std::make_shared<SynchronizationProvider>(runtime, image, std::move(routes));
  for (const uint32_t ordinal : {186u, 189u, 192u, 221u, 225u, 233u, 234u, 235u}) {
    runtime.RegisterKernel(ordinal, [weak = std::weak_ptr(provider), ordinal](X86Context& ctx, uint8_t* base) {
      const auto owner = weak.lock();
      if (!owner) { throw Fault(ctx, ordinal, 0, "synchronization image owner retired"); }
      owner->Invoke(ctx, base, ordinal);
    });
  }
  image.threadRetirement = [provider](uint32_t owner) { provider->Retire(owner); };
  auto bridge = std::make_shared<ThreadWaitBridge>();
  bridge->publish = [weak = std::weak_ptr(provider)](uint32_t handle) {
    const auto owner = weak.lock();
    if (!owner) { throw Fault({}, handle, 0, "thread wait publication owner retired"); }
    return owner->PublishThread(handle);
  };
  bridge->rollback = [weak = std::weak_ptr(provider)](uint32_t handle, uint32_t token) {
    if (const auto owner = weak.lock()) { owner->RollbackThread(handle, token); }
  };
  bridge->complete = [weak = std::weak_ptr(provider)](uint32_t token) {
    if (const auto owner = weak.lock()) { owner->CompleteThread(token); }
  };
  bridge->close = [weak = std::weak_ptr(provider)](uint32_t handle) {
    if (const auto owner = weak.lock()) { owner->CloseThread(handle); }
  };
  image.threadWaitBridge = std::move(bridge);
  image.beginFileIo = [weak = std::weak_ptr(provider)](uint32_t handle) -> FileIoCompletion {
    const auto owner = weak.lock();
    if (!owner) { throw Fault({}, handle, 0, "file read event owner retired"); }
    try {
      return {0, [owner, complete = owner->BeginFileIo(handle)] { complete(); }};
    } catch (const DispatcherError& error) {
      return {error.status, {}};
    }
  };
  image.synchronizationProviderRuntime = &runtime;
  return [weak = std::weak_ptr(provider)](uint32_t handle) {
    const auto owner = weak.lock();
    return owner && owner->Close(handle);
  };
}

}
