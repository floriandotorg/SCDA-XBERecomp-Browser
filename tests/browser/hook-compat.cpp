#include "xdk/hle.h"
#include <array>
#include <bit>
#include <cstdio>

using namespace kernel;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
template <typename Action> bool Faults(const Action& action) {
  try { action(); } catch (const browser::Fault&) { return true; }
  return false;
}

X86_FUNC(guest_multiply) {
  const Args args{ctx};
  const uint32_t product = args[0] * args[1];
  ctx.ecx = ctx.edx = ctx.ebx = ctx.esi = ctx.edi = ctx.ebp = 0xdeadbeef;
  Return(ctx, product, 2);
}

HLE_STDCALL(TestAdd, 2) { return a[0] + a[1]; }
HLE_STDCALL(TestScale, 2) { return uint32_t(ArgF(a, 0) * ArgF(a, 1) + GF(0x30020)); }

int main() {
  browser::Memory memory;
  check(Faults([] { R32(0x30000); }), "an unbound helper read memory");
  const uint64_t generation = browser::hooks::Generation();
  browser::hooks::Bind(memory);
  check(browser::hooks::Generation() == generation + 1, "binding did not advance the hook generation");
  memory.Ensure(0x10000, 0x10000);
  memory.Ensure(0x30000, 0x2000);

  W32(0x30000, 0x11223344);
  W16(0x30004, 0x5566);
  W64(0x30008, 0x0102030405060708ull);
  check(R32(0x30000) == 0x11223344 && R16(0x30004) == 0x5566 && R64(0x30008) == 0x0102030405060708ull, "word round trip");
  check(R8(0x30000) == 0x44 && R8(0x30003) == 0x11, "byte read is not little endian");

#ifdef X86_FLAT_MEMORY
  check(G<uint32_t>(0x30000) == 0x11223344, "flat memory hands out host references without a lease, as generated code reads it");
#else
  check(Faults([] { G<uint32_t>(0x30000); }), "a host reference was taken without a backing lease");
#endif
  {
    auto lease = memory.Borrow();
    G<float>(0x30010) = 2.5f;
    G<float>(0x30010) *= 2;
    check(R32(0x30010) == std::bit_cast<uint32_t>(5.0f), "float lvalue write did not reach guest memory");
    std::array<uint8_t, 40> pattern;
    for (uint32_t n = 0; n < pattern.size(); ++n) { pattern[n] = uint8_t(n * 7 + 1); }
    std::memcpy(HostPtr(0x30fec), pattern.data(), pattern.size());
    std::array<uint8_t, 40> back{};
    memory.Read({}, 0x30fec, back.data(), back.size());
    check(back == pattern, "host pointer copy across a page of one allocation");
    check(Faults([] { HostPtr(0x7000000); }), "an unmapped host pointer was handed out");
  }
  check(Faults([] { R32(0x7000000); }) && Faults([] { W32(0x7000000, 1); }), "unmapped access did not fault");

  constexpr uint32_t physical = 0x80001000;
  const auto version = [&] { return memory.PhysicalWriteVersion({}, physical, 4).value_or(0); };
  const uint64_t first = version();
  W32(physical, 1);
  const uint64_t second = version();
  check(second > first, "W32 on a physical page did not bump its write version");
  {
    auto lease = memory.Borrow();
    G<uint32_t>(physical) = 2;
  }
  check(version() > second && R32(physical) == 2, "G on a physical page did not bump its write version");

  W32(0x30020, std::bit_cast<uint32_t>(0.25f));
  {
    auto lease = memory.Borrow();
    check(GF(0x30020) == 0.25f, "GF did not read a guest float");
  }

  const std::array<X86FuncMapping, 3> mappings = {{{0x20000, sub_00020000}, {0x20010, sub_00020010}, {0x20100, guest_multiply}}};
  browser::Runtime runtime(mappings, {});
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x1ff00;
  runtime.Launch(context, browser::hooks::Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto push = [&](uint32_t value) { ctx.esp -= 4; W32(ctx.esp, value); };
    push(4);
    push(3);
    push(0x12345678);
    runtime.Invoke(ctx, base, 0x20000);
    check(ctx.eax == 7 && ctx.esp == 0x1ff00, "stdcall host function did not pop its frame");
    push(std::bit_cast<uint32_t>(4.0f));
    push(std::bit_cast<uint32_t>(1.5f));
    push(0x12345678);
    runtime.Invoke(ctx, base, 0x20010);
    check(ctx.eax == 6 && ctx.esp == 0x1ff00, "ArgF did not read stack floats");
    ctx.eax = 1, ctx.ecx = 2, ctx.edx = 3, ctx.ebx = 4, ctx.ebp = 5, ctx.esi = 6, ctx.edi = 7;
    const uint32_t product = CallGuest(ctx, 0x20100, {6, 7});
    check(product == 42, "CallGuest lost the guest result");
    check(ctx.eax == 1 && ctx.ecx == 2 && ctx.edx == 3 && ctx.ebx == 4 && ctx.ebp == 5 && ctx.esi == 6 && ctx.edi == 7 && ctx.esp == 0x1ff00,
      "CallGuest did not restore the caller's registers");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }

  W32(0x30100, 0x6c6c6568);
  W32(0x30104, 0x0000006f);
  check(ReadCString(0x30100) == "hello" && ReadCString(0x30100, 3) == "hel", "C string read");
  browser::hooks::Unbind();
  check(Faults([] { R32(0x30000); }), "an unbound helper read memory after teardown");
  std::puts("hook-compat-contracts-ok");
}
