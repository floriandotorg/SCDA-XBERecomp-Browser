#include "browser/menu-launch.h"
#include "cpu/x86_ops.h"
#include "image_symbols.h"
#include <algorithm>
#include <cstring>
#include <string_view>

#define ML_CAT_(left, right) left##right
#define ML_CAT(left, right) ML_CAT_(left, right)
#define ML_GUEST(name) ML_CAT(sub_, SYM_##name)
#define ML_ORIGINAL(name) ML_CAT(__imp__sub_, SYM_##name)
#define ML_ADDRESS(name) ML_CAT(0x, SYM_##name)

X86_FUNC(ML_ORIGINAL(XLaunchNewImageA));
X86_FUNC(ML_ORIGINAL(XMountUtilityDrive));
X86_FUNC(ML_GUEST(XMountUtilityDrive)) {
  if (browser::Runtime::ObserveSdkEntry(ctx, base, 136)) { ML_ORIGINAL(XMountUtilityDrive)(ctx, base); }
}
X86_FUNC(ML_GUEST(XLaunchNewImageA)) {
  if (browser::Runtime::ObserveSdkEntry(ctx, base, 64)) { ML_ORIGINAL(XLaunchNewImageA)(ctx, base); }
}

namespace browser {

std::array<uint8_t, 4096> DevelopmentIcelandLaunchPage() {
  std::array<uint8_t, 4096> page{};
  const auto word = [&](uint32_t offset, uint32_t value) { std::memcpy(page.data() + offset, &value, 4); };
  const auto ascii = [&](uint32_t offset, std::string_view value) { std::copy(value.begin(), value.end(), page.begin() + offset); };
  const auto wide = [&](uint32_t offset, std::u16string_view value) {
    for (uint32_t n = 0; n < value.size(); ++n) { page[offset + n * 2] = uint8_t(value[n]); page[offset + n * 2 + 1] = uint8_t(value[n] >> 8); }
  };
  word(4, 0x5553005e);
  ascii(8, "\\DEVICE\\CDROM0;default.xbe");
  word(532, 0xf0001);
  ascii(540, "\\DEVICE\\CDROM0");
  ascii(555, "default.xbe");
  ascii(1024, "omcs"); ascii(1032, "1dap"); ascii(1040, "m3cs");
  word(1044, 1); ascii(1048, "p3cs"); word(1052, 0x3f19999a); word(1056, 0x3f000000);
  wide(1060, u"SAMFISHER");
  wide(1104, u"MAPNAME=x01_Iceland_a?MENUPAGE=2?PLAYER1=SAMFISHER?MISSIONMODE=1?MISSIONDIFF=1?TRUSTVALUE=75 PLAYERNAME=SAMFISHER TRAVELDATA=FALSE  NEWGAME=TRUE LastScreen=D:\\Default\\DataXb\\Screens\\int\\x01_Iceland_a_loading.tga");
  return page;
}

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

void MenuLaunch::ConfigureDevelopmentStart() {
  if (image_.importsBound || runtime_.HasPublishedImage() || runtime_.ActiveWorkers() || Memory::HasBorrow()) {
    throw Fault({}, 0, 0, "development cache initialization requires its cold unpublished runtime");
  }
  initializeUtilityCache_ = true;
}

bool MenuLaunch::Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) {
  if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ ||
      base != memory_->Base() || !runtime_.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, ctx.esp, 0, "menu launch requires its bound current image worker without a lease");
  }
  if (id == 136) {
    std::lock_guard lock(developmentLock_);
    if (initializeUtilityCache_) {
      if (ctx.esp > UINT32_MAX - 8) { throw Fault(ctx, ctx.esp, 8, "development utility mount frame overflow"); }
      const uint32_t format = 1;
      memory_->CheckRange(ctx, ctx.esp, 8);
      memory_->Write(ctx, ctx.esp + 4, &format, 4);
      initializeUtilityCache_ = false;
    }
    return true;
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
    request.image = LaunchImage(request.path);
    if (!request.image) {
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

int LaunchImage(std::string_view path) {
  std::string lower(path);
  for (char& value : lower) {
    if (value >= 'A' && value <= 'Z') { value = char(value + ('a' - 'A')); }
  }
  for (const auto& [executable, image] : {std::pair<std::string_view, int>{"default.xbe", kStoryImage}, {"sc4_adv.xbe", kVersusImage}}) {
    if (lower.ends_with(executable) && (lower.size() == executable.size() || lower[lower.size() - executable.size() - 1] == '\\' ||
        lower[lower.size() - executable.size() - 1] == ';')) { return image; }
  }
  return 0;
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
      request.image = LaunchImage(request.path);
      if (!request.image) {
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
