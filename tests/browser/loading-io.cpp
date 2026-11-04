#include "browser/cpu_runtime.h"
#include "cpu/x86_ops.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
uint32_t originalCalls = 0;
X86_FUNC(__imp__sub_004B8EC0) {
  ++originalCalls;
  ctx.eax = 7;
  ctx.esp += 12;
}
X86_WEAK_FUNC(sub_004B8EC0, __imp__sub_004B8EC0)

int main() {
  const std::array<X86FuncMapping, 1> mappings = {{{0x4b8ec0, sub_004B8EC0}}};
  browser::Runtime runtime(mappings, {});
  browser::Memory memory;
  memory.Ensure(0x10000, 4096);
  memory.Ensure(0x20000, 4096);
  memory.Ensure(0x5e3000, 8192);
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x10f00;
  runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto put = [&](uint32_t address, uint32_t value) { memory.Write(ctx, address, &value, 4); };
    const auto invoke = [&](uint32_t held, uint32_t ended, uint32_t reader) {
      put(0x5e3e24, held);
      put(0x20120, reader);
      put(0x2012c, ended);
      ctx.esp = 0x10f00;
      const std::array<uint32_t, 3> frame{0x123456, 0x2020c, 0};
      memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
      runtime.Invoke(ctx, base, 0x4b8ec0);
      check(ctx.esp == 0x10f0c, "Bink work query changed stdcall cleanup");
    };
    for (uint32_t n = 0; n < 3; ++n) {
      invoke(n != 1, n != 0, 0x35aa1);
      check(ctx.eax == 0 && originalCalls == 0, "held or completed game reads reported work and could busy-spin");
    }
    invoke(0, 0, 0x35aa1);
    check(ctx.eax == 7 && originalCalls == 1, "normal game reading did not delegate to the original worker");
    invoke(1, 1, 0x123456);
    check(ctx.eax == 7 && originalCalls == 2, "unrelated Bink reader was suppressed");
    ctx.esp = 0x10f00;
    const std::array<uint32_t, 3> invalidFrame{0x123456, 0x100, 0};
    memory.Write(ctx, ctx.esp, invalidFrame.data(), sizeof(invalidFrame));
    bool rejected = false;
    try { runtime.Invoke(ctx, base, 0x4b8ec0); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected && ctx.esp == 0x10f00 && originalCalls == 2, "invalid loading pointer reached the original worker");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  std::puts("loading-io-contracts-ok");
}
