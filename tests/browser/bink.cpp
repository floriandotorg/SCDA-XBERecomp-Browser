#include "xdk/hle.h"
#include <array>
#include <cstdio>

using kernel::R32;
using kernel::W32;

namespace {
const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
uint32_t waits = 0, decodes = 0, advances = 0;
bool summaryOverflowed = false;
}

// BinkWait(bink): nonzero while the next frame is not due; a 30 FPS movie is due every second 60 Hz game frame
X86_FUNC(__imp__sub_004BA920) {
  ctx.eax = waits++ % 2 ? 0 : 1;
  ctx.esp += 8;
}
X86_FUNC(__imp__sub_004BB1B0) {
  ++decodes;
  ctx.eax = 0;
  ctx.esp += 8;
}
X86_WEAK_FUNC(sub_004BB1B0, __imp__sub_004BB1B0)
X86_FUNC(__imp__sub_004BB790) {
  ++advances;
  ctx.esp += 8;
}
X86_WEAK_FUNC(sub_004BB790, __imp__sub_004BB790)
// BinkGetSummary(bink, summary): bytes read * 1000 / (read ms + 1) in a 32 bit div
X86_FUNC(__imp__sub_004BABE0) {
  const uint32_t bink = R32(ctx.esp + 4);
  summaryOverflowed = uint64_t(R32(bink + 0x134)) * 1000 / (uint64_t(R32(bink + 0x13c)) + 1) > UINT32_MAX;
  ctx.esp += 12;
}
X86_WEAK_FUNC(sub_004BABE0, __imp__sub_004BABE0)

int main() {
  browser::Memory memory;
  browser::hooks::Bind(memory);
  memory.Ensure(0x10000, 0x20000);
  const std::array<X86FuncMapping, 4> mappings = {{{0x4ba920, __imp__sub_004BA920}, {0x4babe0, sub_004BABE0}, {0x4bb1b0, sub_004BB1B0}, {0x4bb790, sub_004BB790}}};
  browser::Runtime runtime(mappings, {});
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x1ff00;
  int failures = 0;
  runtime.Launch(context, browser::hooks::Base(), [&](X86Context& ctx, uint8_t* base) {
    constexpr uint32_t bink = 0x20000;
    const auto call = [&](uint32_t target, std::initializer_list<uint32_t> args) {
      const uint32_t before = ctx.esp;
      for (auto argument = std::rbegin(args); argument != std::rend(args); ++argument) {
        ctx.esp -= 4;
        W32(ctx.esp, *argument);
      }
      ctx.esp -= 4;
      W32(ctx.esp, 0x12345678);
      runtime.Invoke(ctx, base, target);
      check(ctx.esp == before, "the callee did not clean its frame");
    };
    const auto run = [&](const char* name, auto body) {
      try {
        body();
        std::printf("case %s ok\n", name);
      } catch (const std::exception& error) {
        ++failures;
        std::printf("case %s failed: %s\n", name, error.what());
      }
    };
    run("movie-pace", [&] {
      for (int frame = 0; frame < 60; ++frame) {
        call(0x4bb1b0, {bink});
        call(0x4bb790, {bink});
      }
      check(decodes == 30 && advances == 30, "one second of 60 Hz game frames did not play 30 movie frames");
    });
    run("summary", [&] {
      W32(bink + 0x134, 0x10000000);
      W32(bink + 0x13c, 0);
      call(0x4babe0, {bink, 0x21000});
      check(!summaryOverflowed, "the summary of a fast large read overflowed its 32 bit division");
    });
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  if (failures) {
    std::printf("bink failures=%d\n", failures);
    return 1;
  }
  std::puts("bink-contracts-ok");
}
