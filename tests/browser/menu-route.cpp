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
  const std::array<X86FuncMapping, 7> mappings = {{{0x18697a, sub_0018697A}, {0x28fa86, isPage}, {0x1a6090, isElement}, {0x1a60e6, findList},
    {MR_ADDRESS(XNetStartup), MR_GUEST(XNetStartup)}, {MR_ADDRESS(WSAStartup), MR_GUEST(WSAStartup)}, {MR_ADDRESS(Direct3D_CreateDevice), MR_GUEST(Direct3D_CreateDevice)}}};
  browser::Runtime runtime(mappings, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Ensure(0x10000, 4096);
  image.memory->Ensure(0x20000, 0x4000);
  image.memory->Ensure(menu, 0x7000);
  image.memory->Ensure(0x541000, 4096);
  auto route = std::make_shared<browser::MenuRoute>(runtime, image);
  check(route->BelongsTo(runtime, image), "route identity");
  browser::PreparedImage foreign;
  check(!route->BelongsTo(runtime, foreign), "foreign image accepted");
  browser::InstallSdkProvider(runtime, image, browser::SdkPolicy::Observer, {}, {}, {}, route);
  image.image.kernel_thunk = 0x10000;
  const uint32_t zero = 0;
  image.memory->Write({}, 0x10000, &zero, 4);
  browser::BindImageImports(image, runtime, {});
  bool fault = false;
  X86Context outside{};
  try { route->Dispatch(outside, image.memory->Base(), 65); } catch (const browser::Fault&) { fault = true; }
  check(fault, "outside worker accepted");
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
    for (int16_t selection : {0, 3, 4, -1}) {
      prepare(selection);
      sub_0018697A(ctx, base);
      check(ctx.eax == 0x76543210 && ctx.esp == stack + 8, "original dispatch ABI");
    }
    check(originalCalls == 4, "Solo/Credits/other did not delegate");
    for (int16_t selection : {1, 2, 1}) {
      prepare(selection);
      sub_0018697A(ctx, base);
      auto handled = expected;
      handled.eax = 1;
      handled.esp += 8;
      check(std::memcmp(&ctx, &handled, sizeof(ctx)) == 0, "rejection did not preserve context and ret4 handled ABI");
      check(originalCalls == 4, "unsupported route reached original");
      check(uint32_t(X86_MEM32(ctx.esp - 8)) == 0x12345 && uint32_t(X86_MEM32(ctx.esp - 4)) == element, "rejection changed caller frame");
      const auto snapshot = route->Snapshot();
      check(snapshot.lastSelection == uint32_t(selection), "diagnostic selection lost");
    }
    check(route->Snapshot().rejectedAttempts == 3, "diagnostic attempt count");
    activePage = 0x100;
    const auto previousElements = elementCalls;
    prepare(2);
    sub_0018697A(ctx, base);
    check(elementCalls == previousElements && originalCalls == 5, "other page was classified");
    activePage = 1;
    activeElement = 0;
    const auto previousLists = listCalls;
    prepare(2);
    sub_0018697A(ctx, base);
    check(listCalls == previousLists && originalCalls == 6, "other element was classified");
    activeElement = 1;
    foundList = 0;
    prepare(2);
    sub_0018697A(ctx, base);
    check(originalCalls == 7, "missing list did not delegate");
    foundList = list;
    const auto expectFault = [&] {
      bool caught = false;
      try { sub_0018697A(ctx, base); } catch (const browser::Fault&) { caught = true; }
      check(caught && originalCalls == 7, "malformed route delegated");
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
    cancelHelper = true;
    bool cancelled = false;
    try { sub_0018697A(ctx, base); } catch (const browser::Cancelled&) { cancelled = true; }
    check(cancelled && originalCalls == 7 && route->Snapshot().rejectedAttempts == 3, "cancelled route dispatched or recorded rejection");
    check(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0, "cancelled helper leaked context changes");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(pageCalls == 13, "unexpected helper dispatch count");
  std::puts("menu-route-contracts-ok");
}
