#include "xdk/hle.h"
#include <array>
#include <bit>
#include <cstdio>
#include <vector>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
std::vector<int32_t> ticks;
X86_FUNC(__imp__sub_002303AF) {
  const float seconds = std::bit_cast<float>(kernel::R32(ctx.esp + 4));
  ticks.push_back(int32_t(double(seconds) * 1000));
  ctx.esp += 8;
}
X86_WEAK_FUNC(sub_002303AF, __imp__sub_002303AF)

int main() {
  browser::Memory memory;
  browser::hooks::Bind(memory);
  memory.Ensure(0x10000, 0x10000);
  const std::array<X86FuncMapping, 1> mappings = {{{0x2303af, sub_002303AF}}};
  browser::Runtime runtime(mappings, {});
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x1ff00;
  runtime.Launch(context, browser::hooks::Base(), [&](X86Context& ctx, uint8_t* base) {
    for (uint32_t n = 0; n < 600; ++n) {
      ctx.esp -= 8;
      kernel::W32(ctx.esp + 4, std::bit_cast<uint32_t>(1.0f / 60));
      kernel::W32(ctx.esp, 0x12345678);
      ctx.ecx = 0x6a6638;
      runtime.Invoke(ctx, base, 0x2303af);
      check(ctx.esp == 0x1ff00, "ProcessTick frame was not popped");
    }
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  int32_t sum = 0;
  for (const auto tick : ticks) {
    check(tick == 16 || tick == 17, "a frame was handed other than 16 or 17 ms");
    sum += tick;
  }
  std::printf("magma ticks=%zu sum=%d\n", ticks.size(), sum);
  check(ticks.size() == 600 && sum == 10000, "ten seconds of 60 FPS frames did not reach Magma as 10000 ms");
  std::puts("magma-time-contracts-ok");
}
