#include "browser/menu-route.h"
#include "cpu/x86_ops.h"
#include "image_symbols.h"
#include <array>
#include <cstdio>
#include <cstring>

#define MR_CAT_(left, right) left##right
#define MR_CAT(left, right) MR_CAT_(left, right)
#define MR_GUEST(name) MR_CAT(sub_, SYM_##name)
#define MR_ORIGINAL(name) MR_CAT(__imp__sub_, SYM_##name)
#define MR_ADDRESS(name) MR_CAT(0x, SYM_##name)

const auto check = [](bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } };
constexpr uint32_t menu = 0x30000, page = 0x34000, list = 0x35000, element = 0x36000;
constexpr uint32_t stack = 0x22f00;
X86Context expected{};
uint32_t originalCalls = 0, pageCalls = 0, elementCalls = 0, listCalls = 0;
uint32_t activePage = 1, activeElement = 1, foundList = list;
bool cancelHelper = false;
X86Context expectedMission{};
uint32_t missionDispatches = 0;
bool missionDispatchThrows = false;

X86_FUNC(missionDispatch) {
  check(std::memcmp(&ctx, &expectedMission, sizeof(ctx)) == 0, "observation changed original mission context");
  ++missionDispatches;
  if (missionDispatchThrows) { throw std::runtime_error("original mission dispatch fault"); }
  ctx.eax = 0x87654321;
  ctx.esp += 12;
}

X86_FUNC(__imp__sub_000534B8) { throw browser::Fault(ctx, 0x534b8, 0, "signature Begin is absent from route fixture"); }
X86_FUNC(__imp__sub_0005351A) { throw browser::Fault(ctx, 0x5351a, 0, "signature End is absent from route fixture"); }
X86_FUNC(MR_ORIGINAL(XNetStartup)) {}
X86_FUNC(MR_ORIGINAL(WSAStartup)) {}
X86_FUNC(MR_ORIGINAL(Direct3D_CreateDevice)) {}
X86_FUNC(MR_GUEST(XNetStartup));
X86_FUNC(MR_GUEST(WSAStartup));
X86_FUNC(MR_GUEST(Direct3D_CreateDevice));
X86_FUNC(sub_0018697A);
X86_FUNC(__imp__sub_0018697A) {
  check(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0, "delegation changed original context");
  check(uint32_t(X86_MEM32(ctx.esp)) == 0x12345 && uint32_t(X86_MEM32(ctx.esp + 4)) == element, "delegation changed original frame");
  ++originalCalls;
  ctx.eax = 0x76543210;
  ctx.esp += 8;
}
const auto clobber = [](X86Context& ctx, uint32_t result, uint32_t cleanup) {
  ctx.eax = result;
  ctx.ecx = 0xdead;
  ctx.edx = 0xbeef;
  ctx.cf = 1;
  ctx.df = 1;
  ctx.xmm[0].u[0] = 0x12345678;
  ctx.st[0] = 12.5;
  ctx.esp += cleanup;
};
X86_FUNC(isPage) {
  check(ctx.ecx == menu && uint32_t(X86_MEM32(ctx.esp + 4)) == 0x541a98, "page helper thiscall ABI");
  ++pageCalls;
  if (cancelHelper) { browser::Runtime::From(ctx).Cancel(); }
  clobber(ctx, activePage, 8);
}
X86_FUNC(isElement) {
  check(uint32_t(X86_MEM32(ctx.esp + 4)) == element && uint32_t(X86_MEM32(ctx.esp + 8)) == 0x541c28, "element helper cdecl ABI");
  ++elementCalls;
  clobber(ctx, activeElement, 4);
}
X86_FUNC(findList) {
  check(uint32_t(X86_MEM32(ctx.esp + 4)) == page && uint32_t(X86_MEM32(ctx.esp + 8)) == 0x541c28, "list helper cdecl ABI");
  ++listCalls;
  clobber(ctx, foundList, 4);
}

int main() {
  const std::array<X86FuncMapping, 8> mappings = {{{0x190000, missionDispatch}, {0x18697a, sub_0018697A}, {0x28fa86, isPage}, {0x1a6090, isElement}, {0x1a60e6, findList},
    {MR_ADDRESS(XNetStartup), MR_GUEST(XNetStartup)}, {MR_ADDRESS(WSAStartup), MR_GUEST(WSAStartup)}, {MR_ADDRESS(Direct3D_CreateDevice), MR_GUEST(Direct3D_CreateDevice)}}};
  browser::Runtime runtime(mappings, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 4096);
  image.memory->Ensure(0x20000, 0x4000);
  image.memory->Ensure(menu, 0x7000);
  image.memory->Ensure(0x541000, 4096);
  const auto classify = browser::MenuMissionObservation::Classify;
  const std::u16string command = u"TRAVEL MAPNAME=synthetic_map.unr?MENUPAGE=1?PLAYER1=Test Agent?MISSIONMODE=1?MISSIONDIFF=2";
  const auto parsed = classify(command);
  check(parsed.valid && parsed.map == u"synthetic_map.unr" && parsed.profile == u"Test Agent" &&
    parsed.menuPage == 1 && parsed.mode == 1 && parsed.difficulty == 2, "actual travel command classification");
  const auto bare = classify(u"TRAVEL MAPNAME=synthetic_map?MENUPAGE=2?PLAYER1=Test Agent?MISSIONMODE=1?MISSIONDIFF=1");
  check(bare.valid && bare.map == u"synthetic_map" && bare.menuPage == 2 && bare.difficulty == 1,
    "observed extensionless map identifier rejected");
  for (const auto& map : {u".unr", u"x.bin", u"x.y.unr", u"../x", u"x/y", u"x\\y"}) {
    check(!classify(std::u16string(u"TRAVEL MAPNAME=") + map + u"?MENUPAGE=2?PLAYER1=Test Agent?MISSIONMODE=1?MISSIONDIFF=1").valid,
      "map classifier admitted a path or unverified extension");
  }
  for (const auto& invalid : {u"TRAVEL MAPNAME=x.unr", u"XLaunchNewImageA default.xbe",
      u"TRAVEL MAPNAME=x.unr?MENUPAGE=1?PLAYER1=?MISSIONMODE=1?MISSIONDIFF=2",
      u"TRAVEL MAPNAME=../x.unr?MENUPAGE=1?PLAYER1=Sam?MISSIONMODE=1?MISSIONDIFF=2",
      u"TRAVEL MAPNAME=x.unr?MENUPAGE=256?PLAYER1=Sam?MISSIONMODE=1?MISSIONDIFF=2",
      u"TRAVEL MAPNAME=x.unr?MENUPAGE=1?PLAYER1=Sam?MISSIONMODE=2?MISSIONDIFF=2",
      u"TRAVEL MAPNAME=x.unr?MENUPAGE=1?PLAYER1=Sam?MISSIONMODE=1?MISSIONDIFF=-1",
      u"TRAVEL MAPNAME=x.unr?MENUPAGE=1?PLAYER1=Sam?MISSIONMODE=1?MISSIONDIFF=2147483648",
      u"TRAVEL MAPNAME=x.unr?MENUPAGE=1?PLAYER1=Sam?MISSIONMODE=1?MISSIONDIFF=02",
      u"TRAVEL MAPNAME=x.unr?MENUPAGE=1?PLAYER1=Sam?MISSIONMODE=1?MISSIONDIFF=2?EXTRA=1",
      u"TRAVEL MAPNAME=x.unr?MENUPAGE=1?PLAYER1=Sam\n?MISSIONMODE=1?MISSIONDIFF=2"}) {
    check(!classify(invalid).valid, "malformed/non-Solo command classified");
  }
  check(classify(u"TRAVEL MAPNAME=x.unr?MENUPAGE=0?PLAYER1=\u00c4\U0001f600?MISSIONMODE=1?MISSIONDIFF=0").valid, "valid UTF16 profile rejected");
  auto malformedUtf16 = command;
  malformedUtf16[60] = 0xd800;
  check(!classify(malformedUtf16).valid, "unpaired surrogate classified");
  check(!classify(std::u16string(browser::MenuMissionObservation::MaxCommandUnits + 1, u'x')).valid, "oversized command classified");
  const auto withProfile = [](const std::u16string& profile) {
    return u"TRAVEL MAPNAME=x.unr?MENUPAGE=1?PLAYER1=" + profile + u"?MISSIONMODE=1?MISSIONDIFF=2";
  };
  check(classify(withProfile(std::u16string(browser::MenuMissionObservation::MaxFieldUnits, u'A'))).valid, "bounded profile rejected");
  check(!classify(withProfile(std::u16string(browser::MenuMissionObservation::MaxFieldUnits + 1, u'A'))).valid, "oversized profile classified");
  check(!classify(withProfile(std::u16string(1, char16_t(0)))).valid, "embedded UTF16 terminator classified");
  auto observation = std::make_shared<browser::MenuMissionObservation>(runtime, image);
  check(observation->BelongsTo(runtime, image), "mission image identity");
  browser::PreparedImage unrelated;
  check(!observation->BelongsTo(runtime, unrelated), "mission foreign image accepted");
  browser::Runtime unrelatedRuntime(mappings, {});
  check(!observation->BelongsTo(unrelatedRuntime, image), "mission foreign runtime accepted");
  bool foreignColdRejected = false;
  try { browser::MenuMissionObservation foreignObservation(unrelatedRuntime, image); } catch (const browser::Fault&) { foreignColdRejected = true; }
  check(foreignColdRejected, "mission foreign cold ownership accepted");
  auto route = std::make_shared<browser::MenuRoute>(runtime, image);
  check(route->BelongsTo(runtime, image), "route identity");
  browser::PreparedImage foreign;
  check(!route->BelongsTo(runtime, foreign), "foreign image accepted");
  runtime.InstallInvocationObserver(*image.memory, [observation](const X86Context& ctx, uint8_t* base, uint32_t target, uint64_t workerId) {
    observation->Observe(ctx, base, target, workerId);
  });
  browser::InstallSdkProvider(runtime, image, browser::SdkPolicy::Observer, {}, {}, {}, route);
  image.image.kernel_thunk = 0x10000;
  const uint32_t zero = 0;
  image.memory->Write({}, 0x10000, &zero, 4);
  browser::BindImageImports(image, runtime, {});
  bool fault = false;
  X86Context outside{};
  try { route->Dispatch(outside, image.memory->Base(), 65); } catch (const browser::Fault&) { fault = true; }
  check(fault, "outside worker accepted");
  observation->Observe(outside, image.memory->Base(), 0x18697a, 1);
  check(observation->Snapshot().total == 0, "outside worker observation accepted");
  X86Context worker{};
  worker.host = &runtime;
  runtime.Launch(worker, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto prepare = [&](int16_t selection) {
      const auto host = ctx.host;
      const auto fs = ctx.fs_base;
      ctx = {};
      ctx.host = host;
      ctx.fs_base = fs;
      ctx.ecx = menu + 0x2758;
      ctx.esp = stack;
      ctx.eax = 0xabcdef01;
      ctx.ebx = 19;
      ctx.edx = 20;
      const std::array<uint32_t, 2> frame{0x12345, element};
      image.memory->Write(ctx, stack, frame.data(), sizeof(frame));
      image.memory->Write(ctx, menu + 0x2788, &page, 4);
      image.memory->Write(ctx, list + 0x9c, &selection, 2);
      expected = ctx;
    };
    for (int16_t selection : {0, 2, 3, 4, -1}) {
      prepare(selection);
      sub_0018697A(ctx, base);
      check(ctx.eax == 0x76543210 && ctx.esp == stack + 8, "original dispatch ABI");
    }
    check(originalCalls == 5, "Solo/Versus/Credits/other did not delegate");
    for (int16_t selection : {1, 1, 1}) {
      prepare(selection);
      sub_0018697A(ctx, base);
      auto handled = expected;
      handled.eax = 1;
      handled.esp += 8;
      check(std::memcmp(&ctx, &handled, sizeof(ctx)) == 0, "rejection did not preserve context and ret4 handled ABI");
      check(originalCalls == 5, "unsupported route reached original");
      check(uint32_t(X86_MEM32(ctx.esp - 8)) == 0x12345 && uint32_t(X86_MEM32(ctx.esp - 4)) == element, "rejection changed caller frame");
      const auto snapshot = route->Snapshot();
      check(snapshot.lastSelection == uint32_t(selection), "diagnostic selection lost");
    }
    check(route->Snapshot().rejectedAttempts == 3, "diagnostic attempt count");
    activePage = 0x100;
    const auto previousElements = elementCalls;
    prepare(2);
    sub_0018697A(ctx, base);
    check(elementCalls == previousElements && originalCalls == 6, "other page was classified");
    activePage = 1;
    activeElement = 0;
    const auto previousLists = listCalls;
    prepare(2);
    sub_0018697A(ctx, base);
    check(listCalls == previousLists && originalCalls == 7, "other element was classified");
    activeElement = 1;
    foundList = 0;
    prepare(2);
    sub_0018697A(ctx, base);
    check(originalCalls == 8, "missing list did not delegate");
    foundList = list;
    const auto expectFault = [&] {
      bool caught = false;
      try { sub_0018697A(ctx, base); } catch (const browser::Fault&) { caught = true; }
      check(caught && originalCalls == 8, "malformed route delegated");
    };
    prepare(2); ctx.esp = 0xfffffffc; expectFault();
    prepare(2); ctx.esp = 4; expectFault();
    prepare(2); ctx.esp = 0x25000; expectFault();
    prepare(2); ctx.ecx = 0x100; expectFault();
    prepare(2); ctx.ecx = 0xfffffffc; expectFault();
    prepare(2); foundList = 0xfffffff0; expectFault();
    prepare(2); foundList = 0x37000; expectFault();
    prepare(2); foundList = list;
    {
      auto lease = image.memory->Borrow();
      expectFault();
    }
    prepare(2);
    auto unchanged = ctx;
    check(route->Dispatch(ctx, base, 64) && std::memcmp(&unchanged, &ctx, sizeof(ctx)) == 0, "foreign service mutated context");
    const auto write32 = [&](uint32_t address, uint32_t value) { image.memory->Write(ctx, address, &value, 4); };
    const auto writeText = [&](uint32_t address, const std::u16string& text) {
      image.memory->Write(ctx, address, text.c_str(), (text.size() + 1) * 2);
    };
    constexpr uint32_t difficultyOwner = 0x30000, menuOwner = 0x31000, engine = 0x35000, viewport = 0x35200;
    constexpr uint32_t vtable = 0x35400, commandAddress = 0x20000, mapAddress = 0x21000, profileAddress = 0x21200;
    image.memory->Ensure(0x650000, 4096);
    image.memory->Ensure(0x5b5000, 4096);
    const auto prepareMission = [&](bool engineBranch) {
      ctx.esp = stack;
      ctx.ebp = stack + 0x80;
      ctx.esi = difficultyOwner;
      ctx.ecx = engineBranch ? engine : viewport;
      ctx.eax = engineBranch ? vtable : viewport;
      ctx.edx = vtable;
      write32(stack, engineBranch ? 0x18890a : 0x18892e);
      write32(stack + 4, engineBranch ? 6 : commandAddress);
      write32(stack + 8, engineBranch ? commandAddress : 0x123);
      write32(0x5b57e8, 0x123);
      write32(0x5b5ac8, 1);
      write32(0x6500fc, engine);
      write32(engine + 0xf0, engineBranch ? 1 : 0);
      write32(ctx.ecx, vtable);
      write32(vtable + (engineBranch ? 0x14 : 0), 0x18697a);
      write32(ctx.ebp + 4, 0x18aa96);
      write32(difficultyOwner + 0x70, 1);
      write32(difficultyOwner + 0x20, menuOwner);
      write32(difficultyOwner + 0x38, list);
      const int16_t selected = 0;
      image.memory->Write(ctx, list + 0x9c, &selected, 2);
      write32(list + 0x60, 0x35600);
      write32(0x3561c, 2);
      write32(ctx.ebp - 4, 2);
      write32(menuOwner + 0x18, 1);
      write32(menuOwner + 4, 0x35800);
      const uint8_t pageByte = 1;
      image.memory->Write(ctx, 0x35838, &pageByte, 1);
      write32(menuOwner + 0x2704, profileAddress);
      write32(menuOwner + 0x2708, 11);
      write32(ctx.ebp - 0x1c, mapAddress);
      write32(ctx.ebp - 0x18, 18);
      write32(ctx.ebp - 0x10, commandAddress);
      write32(ctx.ebp - 0xc, uint32_t(command.size() + 1));
      writeText(commandAddress, command);
      writeText(mapAddress, u"synthetic_map.unr");
      write32(ctx.ebp - 0x18, 18);
      writeText(profileAddress, u"Test Agent");
    };
    const auto observe = [&] {
      const auto before = ctx;
      const auto capture = [&] {
        std::vector<uint8_t> bytes(0xd000);
        image.memory->Read(ctx, 0x20000, bytes.data(), 0x4000);
        image.memory->Read(ctx, 0x30000, bytes.data() + 0x4000, 0x7000);
        image.memory->Read(ctx, 0x650000, bytes.data() + 0xb000, 0x1000);
        image.memory->Read(ctx, 0x5b5000, bytes.data() + 0xc000, 0x1000);
        return bytes;
      };
      const auto saved = capture();
      observation->Observe(ctx, base, 0x18697a, 7);
      check(std::memcmp(&before, &ctx, sizeof(ctx)) == 0 && saved == capture(), "mission observer mutated guest");
      const auto evidence = observation->Snapshot();
      return evidence.events[(evidence.total - 1) % evidence.events.size()];
    };
    for (const bool engineBranch : {true, false}) {
      prepareMission(engineBranch);
      const auto evidence = observe();
      check(evidence.state == browser::MenuMissionObservation::State::Request && evidence.command == command &&
        evidence.workerId == 7 && evidence.target == 0x18697a && evidence.thisPointer == ctx.ecx &&
        evidence.returnAddress == (engineBranch ? 0x18890a : 0x18892e) && evidence.request.profile == u"Test Agent",
        "genuine branch ABI request not captured");
      writeText(commandAddress, u"changed");
      check(evidence.command == command, "mission evidence borrowed guest memory");
    }
    prepareMission(true);
    const std::u16string bareCommand = u"TRAVEL MAPNAME=synthetic_map?MENUPAGE=1?PLAYER1=Test Agent?MISSIONMODE=1?MISSIONDIFF=2";
    writeText(commandAddress, bareCommand);
    write32(ctx.ebp - 0xc, uint32_t(bareCommand.size() + 1));
    writeText(mapAddress, u"synthetic_map");
    write32(ctx.ebp - 0x18, 14);
    check(observe().state == browser::MenuMissionObservation::State::Request,
      "original bare-map dispatch lost owned context verification");
    for (const bool engineBranch : {true, false}) {
      prepareMission(engineBranch);
      write32(vtable + (engineBranch ? 0x14 : 0), 0x190000);
      expectedMission = ctx;
      expectedMission.last_icall = 0x190000;
      runtime.Invoke(ctx, base, 0x190000);
      const auto dispatched = observation->Snapshot();
      const auto& event = dispatched.events[(dispatched.total - 1) % dispatched.events.size()];
      check(ctx.eax == 0x87654321 && ctx.esp == stack + 12 &&
        event.state == browser::MenuMissionObservation::State::Request && event.workerId != 0 && event.command == command,
        "generic hook changed original dispatch result or missed ABI request");
    }
    check(missionDispatches == 2, "generic observer replaced original dispatch");
    prepareMission(true);
    write32(vtable + 0x14, 0x190000);
    expectedMission = ctx;
    expectedMission.last_icall = 0x190000;
    missionDispatchThrows = true;
    bool originalException = false;
    try { runtime.Invoke(ctx, base, 0x190000); } catch (const std::runtime_error& error) {
      originalException = std::string_view(error.what()) == "original mission dispatch fault";
    }
    check(originalException && missionDispatches == 3, "generic observer hid original dispatch exception");
    const auto faultedDispatch = observation->Snapshot();
    check(faultedDispatch.events[(faultedDispatch.total - 1) % faultedDispatch.events.size()].state == browser::MenuMissionObservation::State::Request,
      "original dispatch exception rewrote pre-dispatch evidence");
    missionDispatchThrows = false;
    prepareMission(true);
    write32(vtable + 0x14, 0x190000);
    write32(stack + 8, 0x24000);
    expectedMission = ctx;
    expectedMission.last_icall = 0x190000;
    runtime.Invoke(ctx, base, 0x190000);
    const auto unavailableDispatch = observation->Snapshot();
    check(ctx.eax == 0x87654321 && ctx.esp == stack + 12 && missionDispatches == 4 &&
      unavailableDispatch.events[(unavailableDispatch.total - 1) % unavailableDispatch.events.size()].state == browser::MenuMissionObservation::State::Unavailable,
      "unavailable evidence changed original dispatch result");
    prepareMission(true);
    const auto total = observation->Snapshot().total;
    write32(stack, 0x188907); observation->Observe(ctx, base, 0x18697a, 7);
    check(observation->Snapshot().total == total, "call instruction mistaken for return address");
    prepareMission(true);
    observation->Observe(ctx, nullptr, 0x18697a, 7);
    observation->Observe(ctx, base, 0x18697a, 0);
    {
      auto lease = image.memory->Borrow();
      observation->Observe(ctx, base, 0x18697a, 7);
    }
    check(observation->Snapshot().total == total, "invalid ownership/borrow accepted");
    write32(stack + 4, 5);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "wrong engine event accepted");
    prepareMission(false); write32(stack + 8, 0x124);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "wrong direct ABI context accepted");
    prepareMission(true); write32(menuOwner + 0x18, 2);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "Co-op owner accepted");
    prepareMission(true); write32(0x5b5ac8, 2);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "Co-op global mode accepted as Solo");
    prepareMission(true); write32(ctx.ebp + 4, 0x18aa91);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "New Game entry mistaken for true caller return");
    prepareMission(true); write32(difficultyOwner + 0x70, 0);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "non-New Game difficulty page accepted");
    prepareMission(true); write32(vtable + 0x14, 0x18697b);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "foreign resolved target accepted");
    prepareMission(true);
    const int16_t noSelection = -1;
    image.memory->Write(ctx, list + 0x9c, &noSelection, 2);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "missing difficulty selection accepted");
    prepareMission(true); write32(0x3561c, 1);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "actual difficulty record mismatch accepted");
    prepareMission(true); write32(ctx.ebp - 4, 0);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "selected difficulty mismatch accepted");
    prepareMission(true);
    const uint8_t otherPage = 2;
    image.memory->Write(ctx, 0x35838, &otherPage, 1);
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "menu page mismatch accepted");
    prepareMission(true); write32(ctx.ebp - 0xc, uint32_t(command.size()));
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "unterminated FString count accepted");
    prepareMission(true); writeText(profileAddress, u"Not Sam");
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "profile mismatch accepted");
    prepareMission(true); writeText(mapAddress, u"other.unr");
    check(observe().state == browser::MenuMissionObservation::State::ContextMismatch, "map mismatch accepted");
    prepareMission(true); write32(stack + 8, 0xfffffff0);
    check(observe().state == browser::MenuMissionObservation::State::Unavailable, "overflow span accepted");
    prepareMission(true); write32(stack + 8, 0x24000);
    check(observe().state == browser::MenuMissionObservation::State::Unavailable, "unmapped span accepted");
    prepareMission(true); write32(stack + 8, commandAddress + 1);
    check(observe().state == browser::MenuMissionObservation::State::Unavailable, "misaligned UTF16 accepted");
    prepareMission(true); write32(stack + 8, 0x23ffe);
    const char16_t nonzero = u'Q';
    image.memory->Write(ctx, 0x23ffe, &nonzero, 2);
    check(observe().state == browser::MenuMissionObservation::State::Unavailable, "UTF16 crossed unowned span");
    prepareMission(true); writeText(commandAddress, std::u16string(browser::MenuMissionObservation::MaxCommandUnits, u'Q'));
    const auto atBound = observe();
    check(atBound.state == browser::MenuMissionObservation::State::Malformed && atBound.command.size() == browser::MenuMissionObservation::MaxCommandUnits,
      "terminated exact bound command not owned completely");
    prepareMission(true); writeText(commandAddress, std::u16string(browser::MenuMissionObservation::MaxCommandUnits + 1, u'Q'));
    auto missing = observe();
    check(missing.state == browser::MenuMissionObservation::State::Unavailable && missing.command.empty(), "unterminated command captured as prefix");
    prepareMission(true); writeText(commandAddress, u"TRAVEL MAPNAME=x.unr");
    check(observe().state == browser::MenuMissionObservation::State::Malformed, "incomplete travel classified");
    for (uint32_t n = 0; n < 20; ++n) { prepareMission(true); observe(); }
    const auto bounded = observation->Snapshot();
    check(bounded.count == bounded.events.size() && bounded.total > bounded.count, "mission evidence was not bounded");
    bool coldRejected = false;
    try { browser::MenuMissionObservation late(runtime, image); } catch (const browser::Fault&) { coldRejected = true; }
    check(coldRejected, "published observer installation accepted");
    prepare(2);
    cancelHelper = true;
    bool cancelled = false;
    try { sub_0018697A(ctx, base); } catch (const browser::Cancelled&) { cancelled = true; }
    check(cancelled && originalCalls == 8 && route->Snapshot().rejectedAttempts == 3, "cancelled route dispatched or recorded rejection");
    check(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0, "cancelled helper leaked context changes");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(pageCalls == 14, "unexpected helper dispatch count");
  std::puts("menu-route-contracts-ok");
}
