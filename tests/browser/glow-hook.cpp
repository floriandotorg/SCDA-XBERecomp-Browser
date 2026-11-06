#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "browser/hook-graphics.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace kernel;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

std::array<float, 16> composited{};
uint32_t compositedCalls = 0;
X86_FUNC(__imp__sub_002EAF44) {
  std::memcpy(composited.data(), HostPtr(R32(ctx.esp + 8)), sizeof(composited));
  ++compositedCalls;
}
X86_WEAK_FUNC(sub_002EAF44, __imp__sub_002EAF44)

int main() {
  gpu::State state;
  state.viewport = {0, 0, 640, 480, 0, 1};
  std::recursive_mutex lock;
  browser::HookGraphics graphics(state, lock);
  browser::hooks::BindGraphics(graphics);
  browser::Memory memory;
  browser::hooks::Bind(memory);
  memory.Ensure(0x10000, 0x20000);
  const std::array<X86FuncMapping, 1> mappings = {{{0x2eaf44, sub_002EAF44}}};
  browser::Runtime runtime(mappings, {});
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x1ff00;
  constexpr uint32_t coords = 0x18000;
  const std::array<float, 16> given = {1, -1, 0.25f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 1, -1, 0.75f, 0.125f, 1, 1, 0, 0};
  for (uint32_t n = 0; n < given.size(); ++n) { W32(coords + n * 4, std::bit_cast<uint32_t>(given[n])); }
  runtime.Launch(context, browser::hooks::Base(), [&](X86Context& ctx, uint8_t* base) {
    ctx.esp -= 12;
    W32(ctx.esp, 0x12345678);
    W32(ctx.esp + 4, 0x17000);
    W32(ctx.esp + 8, coords);
    runtime.Invoke(ctx, base, 0x2eaf44);
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(compositedCalls == 1, "the glow composite did not run once");
  const float dx = 0.5f / 256 - 0.5f / 640, dy = 0.5f / 256 - 0.5f / 480;
  const std::array<float, 16> expected = {1, -1, 0.25f - dx, 0.5f - dy, 0.5f, 0.5f, 0.5f, 0.5f, 1, -1, 0.75f - dx, 0.125f - dy, 1, 1, 0, 0};
  for (uint32_t n = 0; n < 16; ++n) { check(std::abs(composited[n] - expected[n]) < 1e-6f, "a glow coordinate moved the wrong way or a screen copy moved"); }
  check(std::abs(dx - 0.001171875f) < 1e-9f && std::abs(dy - 0.000911458f) < 1e-8f, "native's 640x480 shift");
  browser::hooks::Unbind();
  std::puts("glow-hook-contracts-ok");
}
