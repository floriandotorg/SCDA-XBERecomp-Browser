#include "browser/sdk-provider.h"
#include "cpu/x86_ops.h"
#include "image_symbols.h"
#include <algorithm>

#define SDK_CAT_(left, right) left##right
#define SDK_CAT(left, right) SDK_CAT_(left, right)
#define SDK_GUEST(name) SDK_CAT(sub_, SYM_##name)
#define SDK_ORIGINAL(name) SDK_CAT(__imp__sub_, SYM_##name)
#define SDK_ADDRESS(name) SDK_CAT(0x, SYM_##name)

X86_FUNC(SDK_ORIGINAL(XNetStartup));
X86_FUNC(SDK_ORIGINAL(WSAStartup));
X86_FUNC(SDK_ORIGINAL(Direct3D_CreateDevice));

X86_FUNC(SDK_GUEST(XNetStartup)) {
  if (browser::Runtime::ObserveSdkEntry(ctx, base, 1)) {
    SDK_ORIGINAL(XNetStartup)(ctx, base);
    return;
  }
  browser::Runtime::From(ctx).Checkpoint(ctx);
  ctx.eax = 0x276b;
  ctx.esp += 8;
}

X86_FUNC(SDK_GUEST(WSAStartup)) {
  browser::Runtime::ObserveSdkEntry(ctx, base, 2);
  SDK_ORIGINAL(WSAStartup)(ctx, base);
}

X86_FUNC(SDK_GUEST(Direct3D_CreateDevice)) {
  if (browser::Runtime::ObserveSdkEntry(ctx, base, 3)) {
    SDK_ORIGINAL(Direct3D_CreateDevice)(ctx, base);
  }
}

namespace browser {

void SdkTrace::Record(Event event) {
  std::lock_guard lock(lock_);
  event.sequence = ++total_;
  events_[(event.sequence - 1) % events_.size()] = event;
}

SdkTrace::SnapshotData SdkTrace::Snapshot() const {
  std::lock_guard lock(lock_);
  SnapshotData snapshot;
  snapshot.total = total_;
  snapshot.count = size_t(std::min<uint64_t>(total_, events_.size()));
  for (size_t n = 0; n < snapshot.count; ++n) {
    snapshot.events[n] = events_[(total_ - snapshot.count + n) % events_.size()];
  }
  return snapshot;
}

std::shared_ptr<const SdkTrace> InstallSdkProvider(Runtime& runtime, PreparedImage& image, SdkPolicy policy,
  std::shared_ptr<SdkGraphics> graphics, std::shared_ptr<SdkGraphics> input, std::shared_ptr<SdkGraphics> launch, std::shared_ptr<SdkGraphics> route) {
  if ((route && !route->BelongsTo(runtime, image)) || (launch && !launch->BelongsTo(runtime, image)) || (input && !input->BelongsTo(runtime, image)) || (graphics && !graphics->BelongsTo(runtime, image)) || !image.memory || image.importsBound || image.boundRuntime || Memory::HasBorrow() ||
      (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) ||
      image.image.entry == SDK_ADDRESS(XNetStartup) || image.image.entry == SDK_ADDRESS(WSAStartup) ||
      image.image.entry == SDK_ADDRESS(Direct3D_CreateDevice) ||
      runtime.Find(SDK_ADDRESS(XNetStartup)) != SDK_GUEST(XNetStartup) ||
      runtime.Find(SDK_ADDRESS(WSAStartup)) != SDK_GUEST(WSAStartup) ||
      runtime.Find(SDK_ADDRESS(Direct3D_CreateDevice)) != SDK_GUEST(Direct3D_CreateDevice)) {
    throw Fault({}, 0, 0, "SDK observer requires its unpublished generated image");
  }
  auto trace = std::make_shared<SdkTrace>();
  auto* memory = image.memory.get();
  runtime.InstallSdkObserver(*memory, [trace, memory, &image, &runtime, policy, graphics, input, launch, route](X86Context& ctx, uint32_t id, uint64_t workerId) {
    if (image.memory.get() != memory || !image.importsBound || image.boundRuntime != &runtime || id == 0 || id > 65) {
      throw Fault(ctx, ctx.esp, 0, "SDK observer belongs to a stale or unpublished image");
    }
    if (id == 65) { return route ? route->Dispatch(ctx, memory->Base(), id) : true; }
    if (id == 64) { return launch ? launch->Dispatch(ctx, memory->Base(), id) : true; }
    if (id >= 40 && id <= 44) {
      return input ? input->Dispatch(ctx, memory->Base(), id) : true;
    }
    if (id > 3) {
      if (!graphics) { return true; }
      return graphics->Dispatch(ctx, memory->Base(), id);
    }
    const uint32_t argumentCount = id == 3 ? 6 : id;
    std::array<uint32_t, 7> frame{};
    std::array<uint32_t, 13> presentation{};
    {
      auto lease = memory->Borrow();
      memory->Read(ctx, ctx.esp, frame.data(), (argumentCount + 1) * sizeof(uint32_t));
      if (id == 3) { memory->Read(ctx, frame[5], presentation.data(), sizeof(presentation)); }
    }
    const bool delegate = id != 1 || policy == SdkPolicy::Observer;
    if (!delegate && ctx.esp > UINT32_MAX - 8) {
      throw Fault(ctx, ctx.esp, 0, "SDK stack cleanup overflows");
    }
    runtime.Checkpoint(ctx);
    SdkTrace::Event event;
    event.workerId = workerId;
    event.id = id;
    event.fsBase = ctx.fs_base;
    event.threadHandle = ctx.thread_handle;
    event.esp = ctx.esp;
    event.returnAddress = frame[0];
    event.argumentCount = argumentCount;
    std::copy_n(frame.begin() + 1, argumentCount, event.arguments.begin());
    event.presentation = presentation;
    event.behavior = id == 3 && graphics ? SdkBehavior::Diagnostic : delegate ? SdkBehavior::Original : SdkBehavior::Unavailable;
    trace->Record(event);
    if (id == 3 && graphics) {
      graphics->Dispatch(ctx, memory->Base(), id);
      return false;
    }
    return delegate;
  });
  image.diagnosticRuntime = &runtime;
  return trace;
}

}
