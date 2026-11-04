#include "browser/mission-input.h"
#include "cpu/x86_ops.h"
#include <bit>
#include <cstdio>
#include <cmath>

X86_FUNC(sub_001D2A4E);
X86_FUNC(sub_001CDA8A);
std::vector<float> deltas;
uint32_t draws = 0;
bool failTick = false;
X86_FUNC(__imp__sub_0004B525) {}
X86_FUNC(__imp__sub_001CDA8A) { ++draws; ctx.esp += 24; }
X86_FUNC(__imp__sub_001D2A4E) {
  auto& memory = browser::Memory::From(base);
  deltas.push_back(memory.Load<float>(ctx, ctx.esp + 4));
  if (failTick) { throw browser::Fault(ctx, 0x1234, 0, "test tick fault"); }
  browser::Runtime::From(ctx).Call(ctx, base, 0x1cda8a, {0x20000, 0, 0, 0, 0});
  ctx.esp += 8;
}
int main() {
  const auto check = [](bool valid) { if (!valid) { throw std::runtime_error("mission stepping ABI changed"); } };
  const std::array<X86FuncMapping, 2> mappings = {{{0x1cda8a, sub_001CDA8A}, {0x1d2a4e, sub_001D2A4E}}};
  uint64_t clock = 1000000;
  browser::Runtime runtime(mappings, {}, [&] { return clock; });
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  auto& memory = *image.memory;
  memory.Ensure(0x10000, 4096);
  memory.Ensure(0x20000, 0x4000);
  memory.Ensure(0x4f6000, 4096);
  memory.Ensure(0x6a6000, 4096);
  auto storage = std::make_shared<browser::MissionInput::SharedStorage>();
  browser::MissionInput input(runtime, image, {}, storage);
  runtime.InstallSdkObserver(memory, [&](X86Context& ctx, uint32_t id, uint64_t) { return input.Dispatch(ctx, memory.Base(), id); });
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [&] { image.importsBound = true; image.boundRuntime = &runtime; });
  X86Context context{};
  context.host = &runtime;
  context.ecx = 0x21000;
  context.esp = 0x10f00;
  runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto put = [&](uint32_t address, uint32_t value) { memory.Store(ctx, address, value); };
    put(0x21000, 0x22000);
    put(0x2205c, 0x1d2a4e);
    put(0x2207c, 0x1cda8a);
    put(0x21048, 0x20800);
    put(0x20830, 0x20900);
    put(0x20900, 0x20000);
    put(0x20034, 0x23600);
    put(0x236f4, 0x23000);
    put(0x20070, 0x23700);
    put(0x2373c, 0x23800);
    put(0x2380c, 1);
    put(0x6a6638, 0x23900);
    put(0x2393c, 0x23100);
    put(0x23940, 1);
    put(0x23100, 0x23200);
    put(0x23218, 0x23300);
    put(0x2321c, 1);
    put(0x23304, 0x23400);
    put(0x23308, 0x23408);
    put(0x23400, 0x23500);
    put(0x2351c, 100);
    put(0x23524, 1);
    memory.Store(ctx, 0x4f64c8, 0.06666599959135056f);
    const auto tick = [&] {
      ctx.ecx = 0x21000;
      const auto before = ctx;
      runtime.Call(ctx, base, 0x1d2a4e, {std::bit_cast<uint32_t>(0.02f)});
      check(ctx.esp == before.esp && ctx.ebx == before.ebx && ctx.esi == before.esi && ctx.edi == before.edi && ctx.ebp == before.ebp);
    };
    tick();
    check(deltas.size() == 1 && draws == 1);
    clock += 1500000;
    tick();
    check(deltas.size() == 4 && draws == 2);
    check(std::abs(double(deltas[1] + deltas[2] + deltas[3]) - 0.15) < 0.000001);
    put(0x2351c, 1);
    clock += 50000000;
    tick();
    check(deltas.size() == 5 && draws == 3 && deltas.back() == 0.02f);
    put(0x2351c, 100);
    tick();
    check(deltas.size() == 6 && draws == 4);
    memory.Store(ctx, 0x233bc, 10.0f);
    clock += 100000;
    tick();
    memory.Store(ctx, 0x233bc, 3.0f);
    clock += 1500000;
    const auto beforeReset = deltas.size();
    tick();
    check(deltas.size() == beforeReset + 1 && deltas.back() == 0.02f);
    const uint32_t beforeFailureDraws = draws;
    clock += 1500000;
    failTick = true;
    const auto saved = ctx;
    bool failed = false;
    try { tick(); } catch (const browser::Fault&) { failed = true; }
    check(failed);
    ctx = saved;
    failTick = false;
    runtime.Call(ctx, base, 0x1cda8a, {0x20000, 0, 0, 0, 0});
    check(draws == beforeFailureDraws + 1);
    const auto stats = input.SnapshotSteps();
    check(stats.skippedDraws == 2 && stats.steps >= 6);
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  std::puts("mission-stepping-contracts-ok");
}
