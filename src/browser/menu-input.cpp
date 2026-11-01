#include "browser/menu-input.h"
#include "cpu/x86_ops.h"
#include "image_symbols.h"
#include <algorithm>

#define MI_CAT_(left, right) left##right
#define MI_CAT(left, right) MI_CAT_(left, right)
#define MI_ADDRESS(name) MI_CAT(0x, SYM_##name)
#define MI_GUEST(name) MI_CAT(sub_, SYM_##name)
#define MI_ORIGINAL(name) MI_CAT(__imp__sub_, SYM_##name)
#define MI_BOUNDARY(name, id) \
  X86_FUNC(MI_ORIGINAL(name)); \
  X86_FUNC(MI_GUEST(name)) { \
    if (browser::Runtime::ObserveSdkEntry(ctx, base, id)) { MI_ORIGINAL(name)(ctx, base); } \
  }

MI_BOUNDARY(XInitDevices, 40)
MI_BOUNDARY(XInputOpen, 41)
MI_BOUNDARY(XInputClose, 42)
MI_BOUNDARY(XInputGetState, 43)
MI_BOUNDARY(XInputSetState, 44)

namespace browser {
namespace {
constexpr uint32_t gamepadType = MI_ADDRESS(XPP_GamepadType);
constexpr uint32_t memoryUnitType = MI_ADDRESS(XPP_MemoryUnitType);
constexpr uint32_t validButtons = 2047;
static_assert(sizeof(std::atomic<uint32_t>) == 4 && std::atomic<uint32_t>::is_always_lock_free);
}

MenuInput::MenuInput(Runtime& runtime, PreparedImage& image)
  : runtime_(runtime), image_(image), memory_(image.memory.get()) {
  runtime.Checkpoint({});
  if (!memory_ || image.importsBound || image.boundRuntime || runtime.HasPublishedImage() || runtime.ActiveWorkers() ||
      Memory::HasBorrow() || (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) ||
      runtime.Find(MI_ADDRESS(XInitDevices)) != MI_GUEST(XInitDevices) ||
      runtime.Find(MI_ADDRESS(XInputOpen)) != MI_GUEST(XInputOpen) ||
      runtime.Find(MI_ADDRESS(XInputClose)) != MI_GUEST(XInputClose) ||
      runtime.Find(MI_ADDRESS(XInputGetState)) != MI_GUEST(XInputGetState) ||
      runtime.Find(MI_ADDRESS(XInputSetState)) != MI_GUEST(XInputSetState)) {
    throw Fault({}, 0, 0, "menu input requires its cold owned generated image");
  }
  memory_->CheckRange({}, gamepadType, 12);
  memory_->CheckRange({}, memoryUnitType, 12);
  image.diagnosticRuntime = &runtime;
}

bool MenuInput::BelongsTo(Runtime& runtime, PreparedImage& image) const {
  return &runtime == &runtime_ && &image == &image_ && image.memory.get() == memory_;
}
void MenuInput::Require(const X86Context& ctx, uint8_t* base) const {
  if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ ||
      base != memory_->Base() || !runtime_.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, ctx.esp, 0, "menu input requires its bound current image worker without a lease");
  }
  runtime_.Checkpoint(ctx);
}
bool MenuInput::Overlaps(const X86Context& ctx, uint32_t left, size_t leftBytes, uint32_t right, size_t rightBytes) const {
  const uintptr_t first = reinterpret_cast<uintptr_t>(memory_->Span(ctx, left, leftBytes));
  const uintptr_t second = reinterpret_cast<uintptr_t>(memory_->Span(ctx, right, rightBytes));
  return first < second + rightBytes && second < first + leftBytes;
}
void MenuInput::CheckOutput(const X86Context& ctx, uint32_t address, size_t bytes, size_t frameBytes) const {
  if (!address || Overlaps(ctx, address, bytes, ctx.esp, frameBytes) ||
      Overlaps(ctx, address, bytes, gamepadType, 12) || Overlaps(ctx, address, bytes, memoryUnitType, 12)) {
    throw Fault(ctx, address, bytes, "menu input output aliases a call frame or device record");
  }
}
bool MenuInput::Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) {
  Require(ctx, base);
  if (id < 40 || id > 44) { return true; }
  std::lock_guard lock(lock_);
  const uint32_t argumentCount = id == 41 ? 4 : id == 42 ? 1 : 2;
  const size_t frameBytes = (argumentCount + 1) * 4;
  if (uint64_t(ctx.esp) + frameBytes > UINT32_MAX) {
    throw Fault(ctx, ctx.esp, frameBytes, "menu input stdcall cleanup overflow");
  }
  std::array<uint32_t, 5> frame{};
  auto lease = memory_->Borrow();
  memory_->Read(ctx, ctx.esp, frame.data(), frameBytes);
  const auto finish = [&](uint32_t result) {
    ctx.eax = result;
    ctx.esp += uint32_t(frameBytes);
    return false;
  };
  if (id == 40) {
    if (Overlaps(ctx, ctx.esp, frameBytes, gamepadType, 12) ||
        Overlaps(ctx, ctx.esp, frameBytes, memoryUnitType, 12) ||
        Overlaps(ctx, gamepadType, 12, memoryUnitType, 12)) {
      throw Fault(ctx, ctx.esp, frameBytes, "menu input device initialization aliases its call frame");
    }
    const std::array<uint32_t, 3> connected{1, 1, 0};
    const std::array<uint32_t, 3> empty{};
    runtime_.Checkpoint(ctx);
    memory_->Write(ctx, gamepadType, connected.data(), sizeof(connected));
    memory_->Write(ctx, memoryUnitType, empty.data(), sizeof(empty));
    return finish(0);
  }
  if (id == 41) {
    if (frame[1] != gamepadType || frame[2] > 3) { return finish(0); }
    const auto handle = std::find_if(handles_.begin(), handles_.end(), [](const auto& entry) { return entry.token == 0; });
    if (handle == handles_.end() || nextToken_ > 0xbcfffffc) { return finish(0); }
    runtime_.Checkpoint(ctx);
    handle->token = nextToken_;
    handle->port = frame[2];
    nextToken_ += 4;
    return finish(handle->token);
  }
  const auto handle = std::find_if(handles_.begin(), handles_.end(), [&](const auto& entry) { return entry.token && entry.token == frame[1]; });
  if (id == 42) {
    runtime_.Checkpoint(ctx);
    if (handle != handles_.end()) { *handle = {}; }
    return finish(0);
  }
  if (handle == handles_.end()) { return finish(6); }
  if (handle->port != 0) { return finish(1167); }
  if (id == 44) {
    CheckOutput(ctx, frame[2], 70, frameBytes);
    std::array<uint8_t, 70> feedback;
    memory_->Read(ctx, frame[2], feedback.data(), feedback.size());
    uint32_t event;
    std::memcpy(&event, feedback.data() + 4, 4);
    if (event) { return finish(50); }
    const uint32_t complete = 0;
    runtime_.Checkpoint(ctx);
    memory_->Write(ctx, frame[2], &complete, 4);
    return finish(997);
  }
  CheckOutput(ctx, frame[2], 22, frameBytes);
  const uint32_t mask = shared_[0].load(std::memory_order_acquire);
  if (mask & ~validButtons) { throw Fault(ctx, frame[2], 22, "menu input shared controller mask is invalid"); }
  std::array<uint8_t, 18> sample{};
  const auto put16 = [&](size_t offset, int32_t value) {
    const uint16_t bits = uint16_t(value);
    sample[offset] = uint8_t(bits);
    sample[offset + 1] = uint8_t(bits >> 8);
  };
  put16(0, (mask & 15) | ((mask & Start) ? 0x10 : 0));
  sample[2] = (mask & Confirm) ? 255 : 0;
  sample[3] = (mask & Back) ? 255 : 0;
  put16(10, (int32_t(bool(mask & StrafeRight)) - int32_t(bool(mask & StrafeLeft))) * 32767);
  put16(12, (int32_t(bool(mask & Forward)) - int32_t(bool(mask & Backward))) * 32767);
  const uint32_t packet = handle->packet + uint32_t(!handle->sampled || sample != handle->last);
  std::array<uint8_t, 22> output{};
  for (uint32_t n = 0; n < 4; ++n) { output[n] = uint8_t(packet >> (n * 8)); }
  std::copy(sample.begin(), sample.end(), output.begin() + 4);
  runtime_.Checkpoint(ctx);
  memory_->Write(ctx, frame[2], output.data(), output.size());
  handle->packet = packet;
  handle->sampled = true;
  handle->last = sample;
  return finish(0);
}

}
