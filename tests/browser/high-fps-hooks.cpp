#include "xdk/hle.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

using kernel::R32;
using kernel::W32;

namespace {

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
float ReadFloat(uint32_t address) { return std::bit_cast<float>(R32(address)); }
void WriteFloat(uint32_t address, float value) { W32(address, std::bit_cast<uint32_t>(value)); }

constexpr uint32_t kCameraEffects = 0x288, kLevel = 0xf4, kTimeSeconds = 0x3bc, kDeltaSeconds = 0x3f4;
enum Effect { HitRoll, HitFadeOut, TiltPitch, TiltTarget, TiltSpeed, TiltFadeOut, ShakeRoll, ShakeTarget, ShakeSpeed, ShakeFadeOut, kEffectCount };
using Effects = std::array<int32_t, kEffectCount>;

Effects ReadEffects(uint32_t camera) {
  Effects effects;
  for (int n = 0; n < kEffectCount; ++n) { effects[n] = int32_t(R32(camera + kCameraEffects + 4 * n)); }
  return effects;
}
void WriteEffects(uint32_t camera, const Effects& effects) {
  for (int n = 0; n < kEffectCount; ++n) { W32(camera + kCameraEffects + 4 * n, uint32_t(effects[n])); }
}

int32_t Approach(int32_t current, int32_t target, int32_t speed, float seconds) {
  const float step = float(speed) * seconds, distance = float(target) - float(current);
  return int32_t(std::trunc(double(current) + std::clamp(distance, -step, step)));
}
void Swing(int32_t& roll, int32_t& target, int32_t speed, int32_t fadeOut, float seconds) {
  if (!roll && !target) { return; }
  if (roll == target) { target = -Approach(target, 0, fadeOut, seconds); }
  roll = Approach(roll, target, speed, seconds);
}
void StepEffects(Effects& effects, float seconds) {
  if (effects[HitRoll]) { effects[HitRoll] = Approach(effects[HitRoll], 0, effects[HitFadeOut], seconds); }
  Swing(effects[ShakeRoll], effects[ShakeTarget], effects[ShakeSpeed], effects[ShakeFadeOut], seconds);
  Swing(effects[TiltPitch], effects[TiltTarget], effects[TiltSpeed], effects[TiltFadeOut], seconds);
}
Effects Console(Effects effects, int steps) {
  for (int n = 0; n < steps; ++n) { StepEffects(effects, 1.0f / 30); }
  return effects;
}

struct CameraDraw {
  Effects seen;
};
std::vector<CameraDraw> cameraDraws;

uint32_t textureTicks = 0, glitchRolls = 0, attachmentCalls = 0, boundsCalls = 0;
uint32_t clothSteps = 0, clothWrongStep = 0;
bool interferenceScrolls = true;
uint32_t nextRandom = 1000;
double nextFrand = 0.125;

constexpr uint32_t kRootMotionInstance = 0x23000, kRootMotionOut = 0x24000;
int32_t rootMotionError = -5;
bool rootMotionMirrored = false;

}

// EPlayerCam effect update, thiscall(pitch, yaw, roll, relative): applies what it finds, then steps by the frame time
X86_FUNC(__imp__sub_000FCB5D) {
  const uint32_t camera = ctx.ecx;
  Effects effects = ReadEffects(camera);
  cameraDraws.push_back({effects});
  StepEffects(effects, ReadFloat(R32(camera + kLevel) + kDeltaSeconds));
  WriteEffects(camera, effects);
  ctx.esp += 20;
}
X86_WEAK_FUNC(sub_000FCB5D, __imp__sub_000FCB5D)

// mesh instance root motion: writes the world yaw a few units short of the exact mesh yaw
X86_FUNC(__imp__sub_00216CFD) {
  const uint32_t out = R32(ctx.esp + 8);
  const int32_t exact = int32_t(R32(ctx.ecx + 0x3a8 + 4));
  W32(out + 4, uint32_t((rootMotionMirrored ? -exact : exact) + (rootMotionMirrored ? -rootMotionError : rootMotionError)));
  ctx.eax = 1;
  ctx.esp += 24;
}
X86_WEAK_FUNC(sub_00216CFD, __imp__sub_00216CFD)

// UTexture::Tick, thiscall(DeltaSeconds)
X86_FUNC(__imp__sub_001DDA92) {
  ++textureTicks;
  ctx.esp += 8;
}
X86_WEAK_FUNC(sub_001DDA92, __imp__sub_001DDA92)

// JBA night vision glitch, thiscall(LevelInfo)
X86_FUNC(__imp__sub_001F8F9C) {
  ++glitchRolls;
  ctx.esp += 8;
}
X86_WEAK_FUNC(sub_001F8F9C, __imp__sub_001F8F9C)

// JBA night vision pass: the interference scrolls by a fixed amount a frame
X86_FUNC(__imp__sub_002F157B) {
  if (interferenceScrolls) { WriteFloat(0x6a94ec, ReadFloat(0x6a94ec) - 0.005f); }
  ctx.esp += 4;
}
X86_WEAK_FUNC(sub_002F157B, __imp__sub_002F157B)

// UESoftBody::Update: one solver step moves every vertex by +1
X86_FUNC(__imp__sub_0025E82D) {
  const uint32_t body = ctx.ecx;
  const uint32_t level = R32(R32(body + 0x5c) + kLevel);
  if (ReadFloat(level + kDeltaSeconds) != 1.0f / 30 || ReadFloat(body + 0x17c) != 1.0f / 30) { ++clothWrongStep; }
  const uint32_t vertices = R32(body + 0x94), count = R32(body + 0x98);
  for (uint32_t n = 0; n < count * 3; ++n) {
    const uint32_t address = vertices + n / 3 * 0x48 + 4 + n % 3 * 4;
    WriteFloat(address, ReadFloat(address) + 1);
  }
  ++clothSteps;
  ctx.esp += 4;
}
X86_WEAK_FUNC(sub_0025E82D, __imp__sub_0025E82D)
X86_FUNC(cloth_attachments) {
  ++attachmentCalls;
  ctx.esp += 4;
}
X86_FUNC(cloth_bounds) {
  ++boundsCalls;
  ctx.esp += 4;
}

// appFrand: a fresh value on the x87 stack each call
X86_FUNC(__imp__sub_00067760) {
  x87_push(ctx, nextFrand);
  nextFrand += 0.0625;
  ctx.esp += 4;
}
X86_WEAK_FUNC(sub_00067760, __imp__sub_00067760)

// rand: a fresh value in eax each call
X86_FUNC(__imp__sub_00067732) {
  ctx.eax = nextRandom++;
  ctx.esp += 4;
}
X86_WEAK_FUNC(sub_00067732, __imp__sub_00067732)

int main() {
  browser::Memory memory;
  browser::hooks::Bind(memory);
  memory.Ensure(0x10000, 0x20000);
  memory.Ensure(0x6a9000, 0x1000);
  std::array<X86FuncMapping, 10> mappings = {{{0x67732, sub_00067732}, {0x67760, sub_00067760}, {0xfcb5d, sub_000FCB5D},
    {0x1dda92, sub_001DDA92}, {0x1f8f9c, sub_001F8F9C}, {0x216cfd, sub_00216CFD}, {0x25e82d, sub_0025E82D},
    {0x2f157b, sub_002F157B}, {0x500000, cloth_attachments}, {0x500010, cloth_bounds}}};
  browser::Runtime runtime(mappings, {});
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x1ff00;
  int failures = 0;
  runtime.Launch(context, browser::hooks::Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto call = [&](uint32_t target, uint32_t self, uint32_t ret, std::initializer_list<uint32_t> args) {
      const uint32_t before = ctx.esp;
      for (auto argument = std::rbegin(args); argument != std::rend(args); ++argument) {
        ctx.esp -= 4;
        W32(ctx.esp, *argument);
      }
      ctx.esp -= 4;
      W32(ctx.esp, ret);
      ctx.ecx = self;
      runtime.Invoke(ctx, base, target);
      check(ctx.esp == before, "the callee did not clean its frame");
    };
    const auto run = [&](const char* name, const std::function<void()>& body) {
      try {
        body();
        std::printf("case %s ok\n", name);
      } catch (const std::exception& error) {
        ++failures;
        std::printf("case %s failed: %s\n", name, error.what());
      }
    };

    run("camera-shake", [&] {
      constexpr uint32_t level = 0x22000;
      const Effects shake = {3000, 9000, 0, 1000, 8000, 2000, 0, 2000, 30000, 6000};
      uint32_t camera = 0x21000;
      const auto update = [&](float seconds, float delta) {
        WriteFloat(level + kTimeSeconds, seconds);
        WriteFloat(level + kDeltaSeconds, delta);
        call(0xfcb5d, camera, 0x12345678, {0, 0, 0, 0});
      };
      const auto play = [&](uint32_t address, int rate) {
        camera = address;
        W32(camera + kLevel, level);
        WriteEffects(camera, shake);
        update(10, 0);
        update(10 + 1.0f / 120, 1.0f / 120);
        for (int n = 1; n <= rate; ++n) { update(10 + 1.0f / 120 + float(n) / float(rate), 1.0f / float(rate)); }
        return ReadEffects(camera);
      };
      const Effects console = play(0x21000, 30);
      cameraDraws.clear();
      const Effects display = play(0x21400, 60);
      check(console == Console(shake, 30), "one second at 30 Hz did not take 30 console steps");
      check(display == console, "one second at 60 Hz did not leave the fields of one second at 30 Hz");
      bool between = false;
      for (const auto& draw : cameraDraws) {
        for (const Effect speed : {HitFadeOut, TiltSpeed, TiltFadeOut, ShakeSpeed, ShakeFadeOut}) {
          check(!draw.seen[speed], "the stock update saw a nonzero effect speed");
        }
      }
      check(cameraDraws.size() == 62, "the 60 Hz run did not draw every update");
      Effects steps = shake;
      for (size_t step = 1; step < 30; ++step) {
        const Effects next = Console(steps, 1);
        for (const Effect effect : {HitRoll, TiltPitch, ShakeRoll}) {
          const int32_t low = std::min(steps[effect], next[effect]), high = std::max(steps[effect], next[effect]);
          for (const size_t frame : {2 * step + 1, 2 * step + 2}) {
            const int32_t drawn = cameraDraws[frame].seen[effect];
            check(drawn >= low && drawn <= high, "a frame between two steps was not drawn between them");
            between |= drawn != low && drawn != high;
          }
        }
        steps = next;
      }
      check(between, "no frame was drawn strictly between two steps");

      camera = 0x21800;
      W32(camera + kLevel, level);
      WriteEffects(camera, {});
      update(5, 0);
      update(5 + 1.0f / 120, 1.0f / 120);
      WriteEffects(camera, shake);
      for (int n = 1; n <= 12; ++n) { update(5 + 1.0f / 120 + float(n) / 60, 1.0f / 60); }
      check(ReadEffects(camera) == Console(shake, 6), "a shake written between updates was not adopted and stepped at 30 Hz");

      const Effects moved = ReadEffects(camera);
      update(2, 1.0f / 60);
      check(ReadEffects(camera) == moved, "time going back did not restart from the fields");
      update(4, 1.0f / 60);
      check(ReadEffects(camera) == moved, "a gap above one second did not restart from the fields");
      update(4 + 0.5f, 0.5f);
      check(ReadEffects(camera) == Console(moved, 4), "a half second hitch did not run exactly four steps");
      update(4 + 0.5f + 0.025f, 0.025f);
      check(ReadEffects(camera) == Console(moved, 4), "a half second hitch kept the dropped time");
    });

    run("root-motion", [&] {
      constexpr uint32_t actor = 0x23800;
      const auto turn = [&](uint32_t instance, uint32_t ret, uint32_t meshPitch, int32_t exact, uint32_t actorRoll) {
        W32(instance + 0x3a8, meshPitch);
        W32(instance + 0x3a8 + 4, uint32_t(exact));
        W32(instance + 0x3a8 + 8, 0);
        W32(actor + 0x1f4, 0);
        W32(actor + 0x1f4 + 4, 0x4000);
        W32(actor + 0x1f4 + 8, actorRoll);
        ctx.ebx = actor;
        call(0x216cfd, instance, ret, {0x24100, kRootMotionOut, 0, 0x24200, std::bit_cast<uint32_t>(1.0f / 60)});
        return int32_t(R32(kRootMotionOut + 4));
      };
      constexpr uint32_t motion = 0x280e65;
      rootMotionError = -5;
      check(turn(kRootMotionInstance, motion, 0, 1000, 0) == 1000, "an upright root motion turn did not get its exact yaw");
      check(turn(kRootMotionInstance, 0x280e70, 0, 1000, 0) == 995, "another caller's turn was changed");
      check(turn(kRootMotionInstance, motion, 0x100, 1000, 0) == 995, "a turn with mesh pitch was changed");
      check(turn(kRootMotionInstance, motion, 0, 1000, 0x200) == 995, "a turn of a rolled actor was changed");
      rootMotionError = -20;
      check(turn(kRootMotionInstance, motion, 0, 1000, 0) == 980, "an error above 8 units was changed");
      rootMotionMirrored = true;
      rootMotionError = -5;
      check(turn(kRootMotionInstance + 0x400, motion, 0, 1000, 0) == -1000, "a mirrored turn did not get its exact yaw");
      check(turn(kRootMotionInstance + 0x400, motion, 0, 4, 0) == -4, "a small mirrored turn lost its learned sign");
      rootMotionMirrored = false;
    });

    run("texture-tick", [&] {
      constexpr uint32_t texture = 0x25000;
      const auto tick = [&] { call(0x1dda92, texture, 0x12345678, {std::bit_cast<uint32_t>(1.0f / 60)}); };
      WriteFloat(texture + 0xac, 0);
      WriteFloat(texture + 0xb0, 0);
      for (uint32_t n = 1; n <= 10; ++n) {
        tick();
        check(textureTicks == n / 2, "a texture with no MaxFrameRate did not tick every second 60 Hz frame");
      }
      textureTicks = 0;
      WriteFloat(texture + 0xac, 30);
      for (uint32_t n = 1; n <= 4; ++n) { tick(); }
      check(textureTicks == 4, "a texture with a MaxFrameRate did not tick every frame");
    });

    run("jba-glitch", [&] {
      constexpr uint32_t level = 0x26000;
      WriteFloat(level + kDeltaSeconds, 1.0f / 60);
      for (uint32_t n = 1; n <= 10; ++n) {
        call(0x1f8f9c, 0x26800, 0x12345678, {level});
        check(glitchRolls == n / 2, "the glitch did not roll every second 60 Hz frame");
      }
    });

    run("interference", [&] {
      constexpr uint32_t pass = 0x27000, viewport = 0x27400, viewActor = 0x27800, level = 0x27c00;
      W32(pass + 0x1d0, viewport);
      W32(viewport + 0x34, viewActor);
      W32(viewActor + 0xf4, level);
      const auto second = [&](int rate) {
        WriteFloat(0x6a94ec, 0);
        WriteFloat(level + kDeltaSeconds, 1.0f / float(rate));
        for (int n = 0; n < rate; ++n) { call(0x2f157b, pass, 0x12345678, {}); }
        return ReadFloat(0x6a94ec);
      };
      const float console = second(30), display = second(60);
      check(std::fabs(console + 0.15f) < 1e-4f, "one second at 30 Hz did not scroll 0.15");
      check(std::fabs(display - console) < 1e-4f, "one second at 60 Hz did not scroll as far as at 30 Hz");
      interferenceScrolls = false;
      WriteFloat(0x6a94ec, 0.25f);
      call(0x2f157b, pass, 0x12345678, {});
      check(ReadFloat(0x6a94ec) == 0.25f, "the scroll was not restored when the pass did not scroll");
      interferenceScrolls = true;
    });

    run("grain", [&] {
      const auto frand = [&](uint32_t ret) {
        call(0x67760, 0, ret, {});
        const double value = X87_ST(0);
        x87_pop(ctx);
        return value;
      };
      const auto noise = [&] { return std::array<double, 3>{frand(0x2f02e8), frand(0x2f0343), frand(0x2f034e)}; };
      const auto rand = [&](uint32_t ret) {
        call(0x67732, 0, ret, {});
        return ctx.eax;
      };
      const auto thermal = [&] { return std::array<uint32_t, 3>{rand(0x2f2119), rand(0x2f213e), rand(0x2f2149)}; };
      noise();
      thermal();
      const auto first = noise();
      const auto firstThermal = thermal();
      check(noise() == first, "a second appFrand roll inside 1/30 s was not held");
      check(thermal() == firstThermal, "a second rand roll inside 1/30 s was not held");
      check(frand(0x2f0000) != frand(0x2f0000), "a roll at another site was held");
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      const auto fresh = noise();
      const auto freshThermal = thermal();
      for (int n = 0; n < 3; ++n) {
        check(fresh[n] != first[n], "an appFrand roll after 1/30 s was not fresh");
        check(freshThermal[n] != firstThermal[n], "a rand roll after 1/30 s was not fresh");
      }
    });

    run("cloth", [&] {
      constexpr uint32_t body = 0x28000, table = 0x28400, owner = 0x28800, level = 0x28c00, vertices = 0x29000;
      W32(body, table);
      W32(table + 0xc8, 0x500000);
      W32(table + 0xcc, 0x500010);
      W32(body + 0x5c, owner);
      W32(owner + kLevel, level);
      W32(body + 0x94, vertices);
      W32(body + 0x98, 2);
      WriteFloat(body + 0x17c, 1.0f / 60);
      WriteFloat(level + kDeltaSeconds, 1.0f / 60);
      for (uint32_t n = 0; n < 6; ++n) { WriteFloat(vertices + n / 3 * 0x48 + 4 + n % 3 * 4, float(n)); }
      const auto vertex = [&](uint32_t n) { return ReadFloat(vertices + n / 3 * 0x48 + 4 + n % 3 * 4); };
      const std::array<float, 4> shown = {0, 0, 0.5f, 1};
      for (uint32_t n = 1; n <= 4; ++n) {
        call(0x25e82d, body, 0x12345678, {});
        check(clothSteps == n / 2, "cloth did not step once per two 60 Hz frames");
        check(attachmentCalls == n && boundsCalls == n, "cloth attachments and bounds did not update every frame");
        check(ReadFloat(level + kDeltaSeconds) == 1.0f / 60, "the frame's DeltaSeconds was not restored");
        for (uint32_t m = 0; m < 6; ++m) { check(vertex(m) == float(m) + shown[n - 1], "cloth vertices were not the blend of the last two steps"); }
      }
      check(!clothWrongStep, "a cloth step did not run at 1/30 s");
    });
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  if (failures) {
    std::printf("high-fps-hooks failures=%d\n", failures);
    return 1;
  }
  std::puts("high-fps-hooks-contracts-ok");
}
