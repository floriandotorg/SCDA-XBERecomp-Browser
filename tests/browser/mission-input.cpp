#include "browser/mission-input.h"
#include "cpu/x86_ops.h"
#include <bit>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
std::vector<std::array<uint32_t, 4>> events;
X86_FUNC(axis) {
  auto& memory = browser::Memory::From(base);
  std::array<uint32_t, 5> frame{};
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  check(ctx.ecx == 0x21000, "mouse event has the wrong engine");
  events.push_back({frame[1], frame[2], frame[3], frame[4]});
  ctx.eax = 0xdeadbeef;
  ctx.esp += 20;
}
X86_FUNC(__imp__sub_0004B525) {}
X86_FUNC(__imp__sub_001D2A4E) {}
X86_FUNC(__imp__sub_001CDA8A) {}
X86_FUNC(__imp__sub_00039AD4) {}
int main() {
  for (const bool enabled : {false, true}) {
    browser::Runtime videoRuntime({}, {});
    browser::PreparedImage videoImage;
    videoImage.memory = std::make_unique<browser::Memory>();
    auto& videoMemory = *videoImage.memory;
    videoMemory.Ensure(0x10000, 4096);
    auto videoStorage = std::make_shared<browser::MissionInput::SharedStorage>();
    browser::MissionInput video(videoRuntime, videoImage, {}, videoStorage);
    if (enabled) { video.ConfigureDevelopmentStart(); }
    videoRuntime.PublishImageMemory(videoMemory, videoRuntime.KernelRevision(), [&] { videoImage.importsBound = true; videoImage.boundRuntime = &videoRuntime; });
    bool rejected = false;
    try { video.ConfigureDevelopmentStart(); } catch (const browser::Fault&) { rejected = true; }
    check(rejected, "published runtime accepted a changed development mode");
    X86Context videoContext{};
    videoContext.host = &videoRuntime;
    videoContext.esp = 0x10f00;
    videoContext.eax = 0x12345678;
    videoRuntime.Launch(videoContext, videoMemory.Base(), [&](X86Context& ctx, uint8_t* base) {
      const auto before = ctx;
      check(video.Dispatch(ctx, base, 135) != enabled, "video dispatch ignored development mode");
      check(ctx.esp == before.esp + (enabled ? 24 : 0) && ctx.eax == (enabled ? 0 : before.eax) && ctx.ecx == before.ecx && ctx.edx == before.edx, "development video skip changed the guest ABI");
      if (!enabled) { return; }
      for (const uint32_t stack : {0xfffffff0u, 0x10ff0u}) {
        ctx = before; ctx.esp = stack;
        bool rejected = false;
        try { video.Dispatch(ctx, base, 135); } catch (const browser::Fault&) { rejected = true; }
        check(rejected && ctx.esp == stack && ctx.eax == before.eax, "invalid development video frame changed registers");
      }
    });
    if (const auto failure = videoRuntime.Join()) { std::rethrow_exception(failure); }
  }
  const std::array<X86FuncMapping, 1> mappings = {{{0x30000, axis}}};
  browser::Runtime runtime(mappings, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  auto& memory = *image.memory;
  memory.Ensure(0x10000, 4096);
  memory.Ensure(0x20000, 0x4000);
  memory.Ensure(0x6a6000, 4096);
  auto storage = std::make_shared<browser::MissionInput::SharedStorage>();
  std::vector<std::array<float, 2>> samples;
  browser::MissionInput input(runtime, image, {}, storage, [&](const auto& sample) {
    check(!browser::Memory::HasBorrow(), "simulation observer retained a guest memory lease");
    check(sample.level == 0x23000, "simulation sample has the wrong level");
    samples.push_back({sample.seconds, sample.deltaSeconds});
  });
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [&] { image.importsBound = true; image.boundRuntime = &runtime; });
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x10f00;
  context.ecx = 0x20000;
  runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto put = [&](uint32_t address, uint32_t value) { memory.Write(ctx, address, &value, 4); };
    put(0x20034, 0x23600);
    put(0x236f4, 0x23000);
    put(0x233bc, std::bit_cast<uint32_t>(12.5f));
    put(0x233f4, std::bit_cast<uint32_t>(1.0f / 30));
    put(0x20018, 0x20800);
    put(0x2082c, 0x21000);
    put(0x20830, 0x20900);
    put(0x20900, 0x20000);
    put(0x21000, 0x22000);
    put(0x22078, 0x30000);
    (*storage)[0] = 1250;
    (*storage)[1] = uint32_t(-2500);
    const auto before = ctx;
    input.Dispatch(ctx, base, 132);
    check(events.size() == 2 && events[0] == std::array<uint32_t, 4>{0x20000, 0xe4, 4, std::bit_cast<uint32_t>(1.25f)} &&
      events[1] == std::array<uint32_t, 4>{0x20000, 0xe5, 4, std::bit_cast<uint32_t>(-2.5f)}, "relative counts did not reach the game axes");
    check(ctx.ecx == before.ecx && ctx.esp == before.esp && ctx.eax == before.eax, "axis injection changed the original result");
    input.Dispatch(ctx, base, 132);
    check(events.size() == 2 && !(*storage)[0] && !(*storage)[1], "mouse counts replayed");
    check(samples.size() == 2 && samples[0] == std::array<float, 2>{12.5f, 1.0f / 30}, "simulation time was tied to mouse movement");
    put(0x6a6638, 0x23000);
    put(0x2303c, 0x23100);
    put(0x23040, 1);
    put(0x23100, 0x23200);
    put(0x23218, 0x23300);
    put(0x23304, 0x23400);
    put(0x23308, 0x23408);
    put(0x23400, 0x23500);
    put(0x2351c, 1);
    put(0x23524, 0x23600);
    (*storage)[0] = 1000;
    input.Dispatch(ctx, base, 132);
    check(events.size() == 2 && !(*storage)[0], "menu motion moved the camera or queued a later jump");
    put(0x2351c, 100);
    (*storage)[0] = 3000;
    input.Dispatch(ctx, base, 132);
    check(events.size() == 3, "HUD blocked gameplay motion");
    (*storage)[0] = 1000;
    (*storage)[2] = 1;
    input.Dispatch(ctx, base, 132);
    check(events.size() == 3 && !(*storage)[0], "suspended motion reached the game");
    check(samples.size() == 4, "suspended input sampled simulation time");
    (*storage)[2] = 0;
    put(0x233bc, 0x7fc00000);
    input.Dispatch(ctx, base, 132);
    check(samples.size() == 4, "invalid simulation time was published");
    put(0x20034, 0);
    input.Dispatch(ctx, base, 132);
    check(samples.size() == 4, "missing view actor was sampled");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  std::puts("mission-input-contracts-ok");
}
