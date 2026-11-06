#include "xdk/hle.h"
#include "browser/host-settings.h"
#include <array>
#include <cstdio>
#include <string_view>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
uint32_t videos = 0, screens = 0;
X86_FUNC(__imp__sub_00039AD4) {
  ++videos;
  ctx.eax = 1;
  ctx.esp += 0x18;
}
X86_FUNC(__imp__sub_00037274) {
  ++screens;
  ctx.eax = 1;
  ctx.esp += 0x14;
}
X86_WEAK_FUNC(game_sub_00039AD4, __imp__sub_00039AD4)
X86_WEAK_FUNC(sub_00037274, __imp__sub_00037274)

void SkipIntros(bool skip) {
  namespace hs = browser::host_settings;
  auto& words = hs::Shared().words;
  const uint32_t revision = words[hs::kRevisionWord].load();
  words[hs::kRevisionWord] = revision + 1;
  words[hs::kSchemaWord] = hs::kSchema;
  words[hs::kFlagsWord] = skip ? 1u << hs::kSkipIntrosBit : 0;
  words[hs::kRevisionWord] = revision + 2;
}

void WideString(uint32_t va, std::string_view text) {
  for (const char c : text) { kernel::W16(va, uint16_t(c)), va += 2; }
  kernel::W16(va, 0);
}

int main() {
  browser::Memory memory;
  browser::hooks::Bind(memory);
  memory.Ensure(0x10000, 0x10000);
  memory.Ensure(0x30000, 0x1000);
  memory.Ensure(0x651000, 0x1000);
  constexpr uint32_t logos = 0x30000, other = 0x30100, rating = 0x30200, splash = 0x30300;
  WideString(logos, "LogosVideo");
  WideString(other, "IntroVideo");
  WideString(rating, "Textures\\ESRB.TGA");
  WideString(splash, "SplinterCellLogo.tga");
  kernel::W32(0x651344, logos);
  const std::array<X86FuncMapping, 2> mappings = {{{0x37274, sub_00037274}, {0x39ad4, game_sub_00039AD4}}};
  browser::Runtime runtime(mappings, {});
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x1ff00;
  runtime.Launch(context, browser::hooks::Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto call = [&](uint32_t target, uint32_t name, uint32_t arguments) {
      for (uint32_t n = arguments; n > 1; --n) { ctx.esp -= 4, kernel::W32(ctx.esp, n); }
      ctx.esp -= 8;
      kernel::W32(ctx.esp + 4, name);
      kernel::W32(ctx.esp, 0x12345678);
      ctx.ecx = 0x651340;
      ctx.eax = 0xffffffff;
      runtime.Invoke(ctx, base, target);
      check(ctx.esp == 0x1ff00, "the call frame was not popped");
      return ctx.eax;
    };
    call(0x39ad4, logos, 5);
    call(0x37274, rating, 4);
    check(videos == 1 && screens == 1, "with skip_intros off the logos video or rating screen was dropped");
    SkipIntros(true);
    check(call(0x39ad4, logos, 5) == 0 && videos == 1, "the logos video still played");
    check(call(0x39ad4, other, 5) == 1 && videos == 2, "another video did not reach the original");
    check(call(0x37274, rating, 4) == 0 && screens == 1, "the rating screen was still shown");
    check(call(0x37274, splash, 4) == 1 && screens == 2, "the splash screen did not reach the original");
    SkipIntros(false);
    check(call(0x39ad4, logos, 5) == 1 && videos == 3, "turning skip_intros off did not restore the logos video");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  std::puts("intro-contracts-ok");
}
