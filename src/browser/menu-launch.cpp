#include "browser/menu-launch.h"
#include "cpu/x86_ops.h"
#include "image_symbols.h"

#define ML_CAT_(left, right) left##right
#define ML_CAT(left, right) ML_CAT_(left, right)
#define ML_GUEST(name) ML_CAT(sub_, SYM_##name)
#define ML_ORIGINAL(name) ML_CAT(__imp__sub_, SYM_##name)
#define ML_ADDRESS(name) ML_CAT(0x, SYM_##name)

X86_FUNC(ML_ORIGINAL(XLaunchNewImageA));
X86_FUNC(ML_GUEST(XLaunchNewImageA)) {
  if (browser::Runtime::ObserveSdkEntry(ctx, base, 64)) { ML_ORIGINAL(XLaunchNewImageA)(ctx, base); }
}

namespace browser {

MenuLaunch::MenuLaunch(Runtime& runtime, PreparedImage& image, bool originalLaunch)
  : runtime_(runtime), image_(image), memory_(image.memory.get()), originalLaunch_(originalLaunch) {
  runtime.Checkpoint({});
  if (!memory_ || image.importsBound || image.boundRuntime || runtime.HasPublishedImage() || runtime.ActiveWorkers() ||
      Memory::HasBorrow() || (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) ||
      runtime.Find(ML_ADDRESS(XLaunchNewImageA)) != ML_GUEST(XLaunchNewImageA)) {
    throw Fault({}, 0, 0, "menu launch requires its cold owned generated image");
  }
  image.diagnosticRuntime = &runtime;
}

bool MenuLaunch::BelongsTo(Runtime& runtime, PreparedImage& image) const {
  return &runtime == &runtime_ && &image == &image_ && image.memory.get() == memory_;
}

bool MenuLaunch::Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) {
  if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ ||
      base != memory_->Base() || !runtime_.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, ctx.esp, 0, "menu launch requires its bound current image worker without a lease");
  }
  if (id != 64) { return true; }
  if (uint64_t(ctx.esp) + 12 > UINT32_MAX) { throw Fault(ctx, ctx.esp, 12, "menu launch frame overflow"); }
  runtime_.Checkpoint(ctx);
  std::array<uint32_t, 3> frame{};
  MenuLaunchRequested request;
  {
    auto lease = memory_->Borrow();
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    if (frame[1]) {
      for (uint32_t n = 0; n <= 260; ++n) {
        if (uint64_t(frame[1]) + n > UINT32_MAX) { throw Fault(ctx, frame[1], n, "menu launch path overflow"); }
        char value = 0;
        memory_->Read(ctx, frame[1] + n, &value, 1);
        if (!value) { break; }
        if (n == 260 || uint8_t(value) < 32 || uint8_t(value) > 126) {
          throw Fault(ctx, frame[1], n + 1, "menu launch path exceeds the owned host protocol");
        }
        request.path.push_back(value);
      }
    }
    std::string lower = request.path;
    for (char& value : lower) {
      if (value >= 'A' && value <= 'Z') { value = char(value + ('a' - 'A')); }
    }
    constexpr std::string_view executable = "default.xbe";
    const size_t offset = lower.size() >= executable.size() ? lower.size() - executable.size() : std::string::npos;
    const bool supported = offset != std::string::npos && lower.compare(offset, executable.size(), executable) == 0 &&
      (offset == 0 || lower[offset - 1] == '\\' || lower[offset - 1] == ';');
    if (!supported) {
      ctx.eax = 2;
      ctx.esp += 12;
      return false;
    }
    if (frame[2]) {
      memory_->Read(ctx, frame[2], request.data.data(), request.data.size());
      request.hasData = true;
    }
  }
  runtime_.Checkpoint(ctx);
  if (originalLaunch_) { return true; }
  throw request;
}

void MenuLaunch::InstallFirmware(uint32_t launchCell) {
  runtime_.Checkpoint({});
  if (!originalLaunch_ || !BelongsTo(runtime_, image_) || image_.importsBound || runtime_.HasPublishedImage() ||
      runtime_.ActiveWorkers() || runtime_.KernelTarget(49) || Memory::HasBorrow() || !launchCell || launchCell % 4) {
    throw Fault({}, launchCell, 4, "firmware restart requires its cold owned story runtime and launch cell");
  }
  memory_->CheckRange({}, launchCell, 4);
  runtime_.RegisterKernel(49, [this, launchCell](X86Context& ctx, uint8_t* base) {
    if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ ||
        base != memory_->Base() || !runtime_.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
      throw Fault(ctx, 49, 0, "firmware restart requires its bound current image worker");
    }
    runtime_.Checkpoint(ctx);
    MenuLaunchRequested request;
    {
      auto lease = memory_->Borrow();
      std::array<uint32_t, 2> frame{};
      memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      if (frame[1] != 2) { throw Fault(ctx, 49, frame[1], "unsupported firmware restart mode"); }
      uint32_t address = 0;
      memory_->Read(ctx, launchCell, &address, 4);
      if (!address || address % 4096) { throw Fault(ctx, address, 4096, "missing or unaligned firmware launch page"); }
      request.page.emplace();
      memory_->Read(ctx, address, request.page->data(), request.page->size());
      for (size_t n = 8; n < 528; ++n) {
        const uint8_t value = (*request.page)[n];
        if (!value) { break; }
        if (value < 32 || value > 126 || n == 527) { throw Fault(ctx, address + 8, 520, "invalid firmware image path"); }
        request.path.push_back(char(value));
      }
      auto lower = request.path;
      for (char& value : lower) {
        if (value >= 'A' && value <= 'Z') { value = char(value + ('a' - 'A')); }
      }
      constexpr std::string_view executable = "default.xbe";
      if (!lower.ends_with(executable) || (lower.size() > executable.size() &&
          lower[lower.size() - executable.size() - 1] != '\\' && lower[lower.size() - executable.size() - 1] != ';')) {
        throw Fault(ctx, address + 8, 520, "unsupported firmware executable; return to the single player shell");
      }
      request.hasData = true;
      std::copy(request.page->begin() + 1024, request.page->end(), request.data.begin());
    }
    runtime_.Checkpoint(ctx);
    throw request;
  });
}

}
