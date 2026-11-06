#include "xdk/hle.h"
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

X86_FUNC(__imp__sub_0036872C) { X87_ST(0) = NAN; ctx.esp += 4; }
X86_WEAK_FUNC(sub_0036872C, __imp__sub_0036872C)
X86_FUNC(__imp__sub_003687F8) { X87_ST(0) = NAN; ctx.esp += 4; }
X86_WEAK_FUNC(sub_003687F8, __imp__sub_003687F8)
X86_FUNC(__imp__sub_00368348) { X87_ST(0) = NAN; ctx.esp += 4; }
X86_WEAK_FUNC(sub_00368348, __imp__sub_00368348)

int main() {
  browser::Memory memory;
  browser::hooks::Bind(memory);
  memory.Ensure(0x10000, 0x10000);
  const std::array<X86FuncMapping, 3> mappings = {{{0x36872c, sub_0036872C}, {0x3687f8, sub_003687F8}, {0x368348, sub_00368348}}};
  browser::Runtime runtime(mappings, {});
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x1ff00;
  std::array<double, 3> results{};
  runtime.Launch(context, browser::hooks::Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto call = [&](uint32_t address, double st0) {
      x87_push(ctx, st0);
      ctx.esp -= 4;
      kernel::W32(ctx.esp, 0x12345678);
      runtime.Invoke(ctx, base, address);
      check(ctx.esp == 0x1ff00 - 20, "the return address was not popped");
      const double value = X87_ST(0);
      x87_pop(ctx);
      return value;
    };
    ctx.esp -= 20;
    results[0] = call(0x36872c, 0.25);
    results[1] = call(0x3687f8, -0.5);
    kernel::W32(ctx.esp, 0);
    kernel::W32(ctx.esp + 4, std::bit_cast<uint64_t>(2.0) >> 32);
    kernel::W32(ctx.esp + 8, 0);
    kernel::W32(ctx.esp + 12, std::bit_cast<uint64_t>(10.0) >> 32);
    results[2] = call(0x368348, 10.0);
    ctx.esp += 20;
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  std::printf("crt acos=%.17g asin=%.17g pow=%.17g\n", results[0], results[1], results[2]);
  check(results[0] == std::acos(0.25), "acos was not computed on the host");
  check(results[1] == std::asin(-0.5), "asin was not computed on the host");
  check(results[2] == 1024.0, "pow was not computed on the host");
  std::puts("crt-math-contracts-ok");
}
