#include "browser/sdk-provider.h"
#include "browser/audio-entries.h"
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
#if defined(__APPLE__)
X86_FUNC(__imp__sub_000534B8) __attribute__((weak_import));
X86_FUNC(__imp__sub_0005351A) __attribute__((weak_import));
#else
X86_FUNC(__imp__sub_000534B8) __attribute__((weak));
X86_FUNC(__imp__sub_0005351A) __attribute__((weak));
#endif

namespace {

constexpr uint32_t kSignatureBeginAddress = 0x000534B8;
constexpr uint32_t kSignatureEndAddress = 0x0005351A;

struct SignatureObservation;
thread_local SignatureObservation* signatureObservation = nullptr;

struct SignatureObservation {
  X86Context& context;
  uint8_t* base;
  uint32_t id;
  std::function<void(uint32_t)> returned;
  SignatureObservation* previous = signatureObservation;
  SignatureObservation(X86Context& context, uint8_t* base, uint32_t id) : context(context), base(base), id(id) {
    signatureObservation = this;
  }
  ~SignatureObservation() { signatureObservation = previous; }
};

}

X86_FUNC(sub_000534B8) {
  SignatureObservation observation(ctx, base, 69);
  browser::Runtime::ObserveSdkEntry(ctx, base, observation.id);
  __imp__sub_000534B8(ctx, base);
  observation.returned(ctx.eax);
}

X86_FUNC(sub_0005351A) {
  SignatureObservation observation(ctx, base, 71);
  browser::Runtime::ObserveSdkEntry(ctx, base, observation.id);
  __imp__sub_0005351A(ctx, base);
  observation.returned(ctx.eax);
}

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
  std::shared_ptr<SdkGraphics> graphics, std::shared_ptr<SdkGraphics> input, std::shared_ptr<SdkGraphics> launch,
  std::shared_ptr<SdkGraphics> route, std::shared_ptr<SdkGraphics> audio) {
  const auto signatureBegin = runtime.Find(kSignatureBeginAddress);
  const auto signatureEnd = runtime.Find(kSignatureEndAddress);
  const bool signatures = signatureBegin || signatureEnd;
  if ((signatures && (signatureBegin != sub_000534B8 || signatureEnd != sub_0005351A ||
        !__imp__sub_000534B8 || !__imp__sub_0005351A ||
        image.image.entry == kSignatureBeginAddress || image.image.entry == kSignatureEndAddress)) ||
      (route && !route->BelongsTo(runtime, image)) || (launch && !launch->BelongsTo(runtime, image)) || (input && !input->BelongsTo(runtime, image)) || (graphics && !graphics->BelongsTo(runtime, image)) ||
      (audio && !audio->BelongsTo(runtime, image)) || !image.memory || image.importsBound || image.boundRuntime || Memory::HasBorrow() ||
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
  auto sharedAudio = std::move(audio);
  runtime.InstallSdkObserver(*memory, [trace, memory, &image, &runtime, policy, graphics, input, launch, route, signatures, sharedAudio](X86Context& ctx, uint32_t id, uint64_t workerId) {
    if (image.memory.get() != memory || !image.importsBound || image.boundRuntime != &runtime || id == 0 || id == 70 ||
        id > (sharedAudio ? BROWSER_AUDIO_LAST_ID : 71)) {
      throw Fault(ctx, ctx.esp, 0, "SDK observer belongs to a stale or unpublished image");
    }
    if (id >= BROWSER_AUDIO_FIRST_ID) {
      if (!sharedAudio) {
        throw Fault(ctx, ctx.esp, 0, "audio entry observed without the installed audio provider");
      }
      return sharedAudio->Dispatch(ctx, memory->Base(), id);
    }
    if (id == 69 || id == 71) {
      auto* observation = signatureObservation;
      if (!signatures || !observation || &observation->context != &ctx || observation->base != memory->Base() || observation->id != id || observation->returned) {
        throw Fault(ctx, ctx.esp, 0, "signature observation requires its exact retained entry");
      }
      std::array<uint32_t, 2> frame{};
      {
        auto lease = memory->Borrow();
        memory->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      }
      runtime.Checkpoint(ctx);
      SdkTrace::Event event;
      event.workerId = workerId;
      event.id = id;
      event.fsBase = ctx.fs_base;
      event.threadHandle = ctx.thread_handle;
      event.esp = ctx.esp;
      event.returnAddress = frame[0];
      event.argumentCount = 1;
      event.arguments[0] = frame[1];
      observation->returned = [trace, event](uint32_t result) mutable {
        ++event.id;
        event.argumentCount = 2;
        event.arguments[1] = result;
        trace->Record(event);
      };
      trace->Record(event);
      return true;
    }
    if (id == 66 || id == 67) { return graphics ? graphics->Dispatch(ctx, memory->Base(), id) : true; }
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
