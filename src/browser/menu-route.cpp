#include "browser/menu-route.h"
#include "browser/hooks.h"
#include "cpu/x86_ops.h"
#include <algorithm>

// exit-menu.cpp's Exit item on the game mode list, before the route checks Coop and Versus; a build without it has
// no such item
namespace browser::exit_menu {
__attribute__((weak)) bool Press(X86Context&) { return false; }
}

// story addresses: the versus module links the classes only, it never installs them
#ifndef BROWSER_VERSUS
BROWSER_GAME_CHAIN(0018697A)
X86_FUNC(sub_0018697A) {
  if (browser::exit_menu::Press(ctx)) { return; }
  if (browser::Runtime::ObserveSdkEntry(ctx, base, 65)) { game_sub_0018697A(ctx, base); }
}
#else
X86_FUNC(sub_0018697A) __attribute__((weak));
#endif

namespace browser {
namespace {
constexpr uint32_t pressHandler = 0x18697a;
constexpr uint32_t pressBase = 0x2758;
constexpr uint32_t gameModePage = 0x2788;
constexpr uint32_t isPage = 0x28fa86;
constexpr uint32_t isElement = 0x1a6090;
constexpr uint32_t findList = 0x1a60e6;
constexpr uint32_t pageName = 0x541a98;
constexpr uint32_t listName = 0x541c28;
constexpr uint32_t selectionOffset = 0x9c;
}

MenuMissionObservation::MenuMissionObservation(Runtime& runtime, PreparedImage& image)
  : runtime_(runtime), image_(image), memory_(image.memory.get()) {
  runtime.Checkpoint({});
  if (!memory_ || image.importsBound || image.boundRuntime || runtime.HasPublishedImage() || runtime.ActiveWorkers() ||
      Memory::HasBorrow() || (image.diagnosticRuntime && image.diagnosticRuntime != &runtime)) {
    throw Fault({}, 0, 0, "mission observation requires its cold owned image");
  }
  image.diagnosticRuntime = &runtime;
}

bool MenuMissionObservation::BelongsTo(Runtime& runtime, PreparedImage& image) const {
  return &runtime == &runtime_ && &image == &image_ && image.memory.get() == memory_;
}

MenuMissionObservation::SnapshotData MenuMissionObservation::Snapshot() const {
  std::lock_guard lock(observationLock_);
  return evidence_;
}

MenuMissionObservation::Command MenuMissionObservation::Classify(std::u16string_view command) {
  Command result;
  if (command.empty() || command.size() > MaxCommandUnits) { return result; }
  for (size_t n = 0; n < command.size(); ++n) {
    const auto unit = command[n];
    if (unit < 0x20 || unit == 0x7f) { return result; }
    if (unit >= 0xd800 && unit <= 0xdbff) {
      if (++n == command.size() || command[n] < 0xdc00 || command[n] > 0xdfff) { return result; }
    } else if (unit >= 0xdc00 && unit <= 0xdfff) { return result; }
  }
  const auto consume = [&](std::u16string_view prefix) {
    if (!command.starts_with(prefix)) { return false; }
    command.remove_prefix(prefix.size());
    return true;
  };
  const auto field = [&](std::u16string_view delimiter) {
    const auto end = command.find(delimiter);
    if (end == std::u16string_view::npos) { return std::u16string_view{}; }
    const auto value = command.substr(0, end);
    command.remove_prefix(end + delimiter.size());
    return value;
  };
  const auto number = [](std::u16string_view value, uint32_t limit, uint32_t& output) {
    if (value.empty() || value.size() > 10 || (value.size() > 1 && value.front() == u'0')) { return false; }
    uint64_t parsed = 0;
    for (const auto unit : value) {
      if (unit < u'0' || unit > u'9') { return false; }
      parsed = parsed * 10 + uint32_t(unit - u'0');
      if (parsed > limit) { return false; }
    }
    output = uint32_t(parsed);
    return true;
  };
  if (!consume(u"TRAVEL MAPNAME=")) { return result; }
  const auto map = field(u"?MENUPAGE=");
  const auto page = field(u"?PLAYER1=");
  const auto profile = field(u"?MISSIONMODE=");
  const auto mode = field(u"?MISSIONDIFF=");
  auto identifier = map;
  if (identifier.ends_with(u".unr")) { identifier.remove_suffix(4); }
  if (identifier.empty() || map.size() > MaxFieldUnits ||
      profile.empty() || profile.size() > MaxFieldUnits || profile.find_first_not_of(u' ') == std::u16string_view::npos ||
      profile.find_first_of(u"?=") != std::u16string_view::npos ||
      !number(page, 255, result.menuPage) || !number(mode, 1, result.mode) || result.mode != 1 ||
      !number(command, INT32_MAX, result.difficulty)) { return {}; }
  for (size_t n = 0; n < identifier.size(); ++n) {
    const auto unit = identifier[n];
    if (!((unit >= u'a' && unit <= u'z') || (unit >= u'A' && unit <= u'Z') ||
        (unit >= u'0' && unit <= u'9') || unit == u'_' || unit == u'-')) { return {}; }
  }
  result.map = map;
  result.profile = profile;
  result.valid = true;
  return result;
}

void MenuMissionObservation::Inspect(const X86Context& ctx, Event& event) {
  const auto read = [&]<typename T>(uint32_t address, uint32_t offset = 0) {
    if (!address || uint64_t(address) + offset + sizeof(T) > uint64_t(UINT32_MAX) + 1) {
      throw Fault(ctx, address, sizeof(T), "mission observation address unavailable");
    }
    T value{};
    memory_->Read(ctx, address + offset, &value, sizeof(value));
    return value;
  };
  const auto word = [&](uint32_t address, uint32_t offset = 0) { return read.template operator()<uint32_t>(address, offset); };
  const auto text = [&](uint32_t address, size_t limit) {
    if (!address || address % 2) { throw Fault(ctx, address, 2, "mission UTF16 pointer unavailable"); }
    std::u16string value;
    for (size_t n = 0; n <= limit; ++n) {
      const auto unit = read.template operator()<char16_t>(address, uint32_t(n * 2));
      if (!unit) { return value; }
      if (n == limit) { break; }
      value.push_back(unit);
    }
    throw Fault(ctx, address, (limit + 1) * 2, "mission UTF16 terminator unavailable");
  };
  const bool engineBranch = event.returnAddress == 0x18890a;
  event.commandAddress = word(ctx.esp, engineBranch ? 8 : 4);
  event.command = text(event.commandAddress, MaxCommandUnits);
  event.request = Classify(event.command);
  if (!event.request.valid) { event.state = State::Malformed; return; }
  const auto mismatch = [&] { event.state = State::ContextMismatch; };
  const auto engine = word(0x6500fc);
  const auto table = word(ctx.ecx);
  if (!runtime_.Find(event.target) || word(table, engineBranch ? 0x14 : 0) != event.target ||
      (engineBranch && (word(ctx.esp, 4) != 6 || engine != ctx.ecx || !word(engine, 0xf0) || ctx.eax != table)) ||
      (!engineBranch && (word(engine, 0xf0) || ctx.eax != ctx.ecx || ctx.edx != table || ctx.ecx < 0x30 ||
        word(ctx.esp, 8) != word(0x5b57e8)))) { mismatch(); return; }
  if (ctx.ebp < 0x1c) { throw Fault(ctx, ctx.ebp, 0x1c, "mission frame unavailable"); }
  event.difficultyOwner = ctx.esi;
  event.menuOwner = word(ctx.esi, 0x20);
  const auto list = word(ctx.esi, 0x38);
  const auto selection = read.template operator()<int16_t>(list, 0x9c);
  const auto records = word(list, 0x60);
  if (selection < 0 || uint64_t(records) + uint32_t(selection) * 0x38 + 0x20 > uint64_t(UINT32_MAX) + 1) {
    mismatch(); return;
  }
  const auto selectedDifficulty = word(records, uint32_t(selection) * 0x38 + 0x1c);
  const auto pageOwner = word(event.menuOwner, 4);
  const auto actualPage = read.template operator()<uint8_t>(pageOwner, 0x38);
  const auto profilePointer = word(event.menuOwner, 0x2704);
  const auto profileCount = word(event.menuOwner, 0x2708);
  const auto mapPointer = word(ctx.ebp - 0x1c);
  const auto mapCount = word(ctx.ebp - 0x18);
  if (word(ctx.ebp, 4) != 0x18aa96 || word(ctx.esi, 0x70) != 1 || word(event.menuOwner, 0x18) != 1 || word(0x5b5ac8) != 1 ||
      word(ctx.ebp - 4) != event.request.difficulty || selectedDifficulty != event.request.difficulty ||
      actualPage != event.request.menuPage || word(ctx.ebp - 0x10) != event.commandAddress ||
      word(ctx.ebp - 0xc) != event.command.size() + 1 || profileCount != event.request.profile.size() + 1 ||
      mapCount != event.request.map.size() + 1 ||
      text(profilePointer, MaxFieldUnits) != event.request.profile || text(mapPointer, MaxFieldUnits) != event.request.map) {
    mismatch(); return;
  }
  event.state = State::Request;
}

void MenuMissionObservation::Observe(const X86Context& ctx, uint8_t* base, uint32_t target, uint64_t workerId) noexcept {
  try {
    if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ ||
        base != memory_->Base() || !runtime_.OwnsCurrentContext(ctx) || !workerId || Memory::HasBorrow()) { return; }
    Event event;
    event.workerId = workerId;
    event.target = target;
    event.thisPointer = ctx.ecx;
    event.fsBase = ctx.fs_base;
    auto lease = memory_->Borrow();
    memory_->Read(ctx, ctx.esp, &event.returnAddress, 4);
    const auto returns = ReturnAddresses();
    if (std::find(returns.begin(), returns.end(), event.returnAddress) == returns.end()) { return; }
    try { Inspect(ctx, event); } catch (...) { event.state = State::Unavailable; }
    std::lock_guard lock(observationLock_);
    event.sequence = evidence_.total + 1;
    evidence_.events[evidence_.total % evidence_.events.size()] = std::move(event);
    ++evidence_.total;
    evidence_.count = size_t(std::min<uint64_t>(evidence_.total, evidence_.events.size()));
  } catch (...) {}
}

MenuRoute::MenuRoute(Runtime& runtime, PreparedImage& image)
  : runtime_(runtime), image_(image), memory_(image.memory.get()) {
  runtime.Checkpoint({});
  if (!memory_ || image.importsBound || image.boundRuntime || runtime.HasPublishedImage() || runtime.ActiveWorkers() ||
      Memory::HasBorrow() || (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) ||
      runtime.Find(pressHandler) != sub_0018697A || !runtime.Find(isPage) || !runtime.Find(isElement) || !runtime.Find(findList)) {
    throw Fault({}, 0, 0, "menu route requires its cold owned generated image and native helpers");
  }
  memory_->CheckRange({}, pageName, sizeof("P_OnlineOffline"));
  memory_->CheckRange({}, listName, sizeof("Lst_OfflineOnline"));
  image.diagnosticRuntime = &runtime;
}

bool MenuRoute::BelongsTo(Runtime& runtime, PreparedImage& image) const {
  return &runtime == &runtime_ && &image == &image_ && image.memory.get() == memory_;
}

MenuRoute::SnapshotData MenuRoute::Snapshot() const {
  std::lock_guard lock(lock_);
  return snapshot_;
}

uint32_t MenuRoute::Call(X86Context& ctx, uint8_t* base, uint32_t target, uint32_t self, std::initializer_list<uint32_t> arguments) {
  struct Restore {
    X86Context& context;
    X86Context saved;
    ~Restore() { context = saved; }
  } restore{ctx, ctx};
  ctx.ecx = self;
  const auto result = runtime_.Call(ctx, base, target, arguments);
  runtime_.Checkpoint(ctx);
  return result;
}

bool MenuRoute::Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) {
  if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ ||
      base != memory_->Base() || !runtime_.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, ctx.esp, 0, "menu route requires its bound current image worker without a lease");
  }
  runtime_.Checkpoint(ctx);
  if (id != 65) { return true; }
  if (uint64_t(ctx.esp) + 8 > UINT32_MAX || ctx.esp < 12 || ctx.ecx < pressBase ||
      uint64_t(ctx.ecx - pressBase) + gameModePage + 4 > UINT32_MAX) {
    throw Fault(ctx, ctx.esp, 8, "menu route frame or menu address overflow");
  }
  const uint32_t menu = ctx.ecx - pressBase;
  std::array<uint32_t, 2> frame{};
  {
    auto lease = memory_->Borrow();
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    memory_->CheckRange(ctx, ctx.esp - 12, 12);
  }
  if (!(Call(ctx, base, isPage, menu, {pageName}) & 0xff)) { return true; }
  if (!Call(ctx, base, isElement, ctx.ecx, {frame[1], listName})) { return true; }
  uint32_t page = 0;
  {
    auto lease = memory_->Borrow();
    memory_->Read(ctx, menu + gameModePage, &page, sizeof(page));
  }
  if (!page) { return true; }
  const uint32_t list = Call(ctx, base, findList, ctx.ecx, {page, listName});
  if (!list) { return true; }
  if (uint64_t(list) + selectionOffset + 2 > UINT32_MAX) {
    throw Fault(ctx, list, 2, "menu route selection address overflow");
  }
  int16_t selection = 0;
  {
    auto lease = memory_->Borrow();
    memory_->Read(ctx, list + selectionOffset, &selection, sizeof(selection));
  }
  // Versus (2) goes through to its launch of sc4_adv.xbe, which the page answers by switching modules; Coop (1) waits
  // for the network
  if (selection != 1) { return true; }
  runtime_.Checkpoint(ctx);
  {
    std::lock_guard lock(lock_);
    ++snapshot_.rejectedAttempts;
    snapshot_.lastSelection = uint32_t(selection);
  }
  ctx.eax = 1;
  ctx.esp += 8;
  return false;
}

}
