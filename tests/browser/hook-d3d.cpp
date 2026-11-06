#include "xdk/d3d8.h"
#include "browser/cpu_runtime.h"
#include "browser/hook-graphics.h"
#include <array>
#include <cstdio>
#include <vector>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

struct FakeBackend final : gpu::Backend {
  std::vector<float> occlusion, filter;
  uint32_t applied = 0;
  bool Init(uint32_t, uint32_t) override { return true; }
  void Clear(const gpu::State&, uint32_t, const uint8_t*, uint32_t, uint32_t, float, uint32_t) override {}
  void Draw(const gpu::State&, uint32_t, uint32_t, uint32_t, const uint8_t*) override {}
  void Copy(const gpu::Surface&, const gpu::Surface&, uint32_t, uint32_t, uint32_t) override {}
  void Present(const gpu::Surface&, bool) override {}
  void Sync(uint32_t, uint32_t) override {}
  void SyncLate(uint32_t) override {}
  void Flush() override {}
  void SetGamma(const uint8_t*) override {}
  void Occlusion(const gpu::State&, const float* p) override { occlusion.assign(p, p + gpu::kOcclusionParams); }
  void FilterShadows(const gpu::State&, const float* p) override { filter.assign(p, p + gpu::kShadowFilterParams); }
  void ApplyShadows(const gpu::State&) override { ++applied; }
};

void FogPass(gpu::State& state) {
  state.simple[gpu::kMBlendEnable >> 2] = 1;
  state.simple[gpu::kMSrcBlend >> 2] = 0x302;
  state.simple[gpu::kMDestBlend >> 2] = 0x303;
  state.simple[gpu::kMPsConstant0 >> 2] = 0x80ffffff;
  state.vsc[107] = {0, 0, 2, 1};
  state.vsc[116] = {0, 0, 0.5f, -0.25f};
}

int main() {
  gpu::State state;
  std::recursive_mutex lock;
  browser::HookGraphics graphics(state, lock);
  bool threw = false;
  try { d3d::State(); } catch (const browser::Fault&) { threw = true; }
  check(threw, "an unbound d3d shim handed out graphics state");
  browser::hooks::BindGraphics(graphics);
  check(&d3d::State() == &state && &d3d::Lock() == &lock, "d3d::State or d3d::Lock is not the live device state");
  d3d::State().zscale = 3;
  check(state.zscale == 3, "d3d::State is a copy");

  const std::array<uint32_t, 5> ramp{0x00040001, 0, 0, 0x00010029 | (6u << 20), 0};
  const std::array<uint32_t, 5> wide{0x00040001, 0, 0, 0x00010029 | (6u << 20) | (1u << 24), 0};
  FogPass(state);
  graphics.LatchFog(wide);
  check(graphics.Fog().amount == 0, "a 64 x 2 texture was taken for the fog ramp");
  graphics.LatchFog(ramp);
  check(graphics.FogPasses() == std::array<uint32_t, 2>{2, 1}, "fog passes and ramp passes were not counted");
  const auto fog = graphics.Fog();
  check(fog.slope == 0.25f && fog.offset == -0.5f && std::abs(fog.amount - 128.0f / 255) < 1e-6f, "fog plane latch");
  graphics.ClearFog();
  check(graphics.FogPasses() == std::array<uint32_t, 2>{0, 0}, "Present did not reset the fog pass counts");
  state.vsc[116] = {1, 0, 0.5f, -0.25f};
  graphics.LatchFog(ramp);
  check(graphics.Fog().amount == 0, "a plane not parallel to the view depth was latched as fog");
  state.vsc[116] = {0, 0, 0.5f, -0.25f};
  state.simple[gpu::kMDestBlend >> 2] = 0x304;
  graphics.LatchFog(ramp);
  check(graphics.Fog().amount == 0, "a pass that does not blend by source alpha was latched as fog");
  FogPass(state);
  graphics.LatchFog(ramp);

  const std::array<float, 16> projection{2, 0, 0, 0, 0, 3, 0, 0, 0.1f, 0.2f, 1.5f, 1, 0, 0, -4, 0};
  d3d::Occlusion(projection.data(), true);
  d3d::FilterShadows(projection.data());
  d3d::ApplyShadows();
  check(graphics.UnsupportedEffects() == 0, "an absent backend counted unsupported effects");
  FakeBackend backend;
  graphics.SetBackend(&backend, {});
  d3d::Occlusion(projection.data(), true);
  d3d::FilterShadows(projection.data());
  d3d::ApplyShadows();
  check(graphics.UnsupportedEffects() == 3 && backend.occlusion.empty() && backend.filter.empty() && !backend.applied,
    "an unsupported effect reached the backend or was not counted");
  graphics.SetBackend(&backend, [](browser::GraphicsEffect) { return true; });
  d3d::Occlusion(projection.data(), true);
  const std::vector<float> expected{0.5f, 1.0f / 3, 0.1f, 0.2f, -4, 1.5f, 0.25f, -0.5f, 128.0f / 255, 1};
  check(backend.occlusion.size() == expected.size(), "occlusion parameter count");
  for (size_t n = 0; n < expected.size(); ++n) { check(std::abs(backend.occlusion[n] - expected[n]) < 1e-6f, "occlusion parameters"); }
  d3d::FilterShadows(projection.data());
  check(backend.filter == std::vector<float>{-4, 1.5f}, "shadow filter parameters");
  d3d::ApplyShadows();
  check(backend.applied == 1, "shadows were not applied");
  auto orthographic = projection;
  orthographic[11] = 0;
  backend.occlusion.clear();
  d3d::Occlusion(orthographic.data(), false);
  check(backend.occlusion.empty() && graphics.UnsupportedEffects() == 3, "a non perspective projection reached occlusion");

  state.viewport = {10, 20, 640, 480, 0, 1};
  graphics.ViewportConstants();
  const auto plain = state.vsc[58];
  d3d::ViewScale(2);
  check(state.vsc[58][0] == plain[0] * 2 && state.vsc[58][1] == plain[1] * 2 && state.vsc[58][2] == plain[2], "ViewScale(2) did not double c58 x and y");
  check(state.vsc[59][0] == 10 + 320 && state.vsc[59][1] == 20 + 240, "viewport centre");
  d3d::ViewScale(1);
  check(state.vsc[58] == plain, "ViewScale(1) did not restore c58");
  browser::hooks::UnbindGraphics();
  std::puts("hook-d3d-contracts-ok");
}
