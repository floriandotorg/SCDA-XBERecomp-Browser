#include "browser/menu-shaders.h"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
  gpu::State s;
  gpu::TexInfo tex[4]{};
  s.viewport = {16, 8, 320, 240, 0.25f, 0.75f};
  for (auto index : {2, 3, 256}) {
    for (int n = 0; n < 4; ++n) {
      s.transforms[index][n * 5] = 1;
    }
  }
  s.transforms[256][12] = 4;
  s.vsc[96] = {1, 2, 3, 4};
  s.rs[gpu::kRsTextureFactor] = 0x80402010;
  s.simple[gpu::kMAlphaTestEnable / 4] = 1;
  s.simple[gpu::kMAlphaFunc / 4] = 0x204;
  s.simple[gpu::kMAlphaRef / 4] = 0x180;
  const auto c = browser::menu::BuildConstants(s, 640, 480);
  assert(c.vsc[96] == s.vsc[96]);
  assert(c.misc[0] == 128 && c.misc[1] == 4);
  assert(c.surf[0] == 640 && c.surf[2] == 1 / s.zscale);
  assert(c.wvp[0] == 0.5f && c.wvp[5] == 0.5f && c.wvp[10] == 0.5f);
  assert(std::abs(c.wvp[12] - 1.55f) < 0.00001f);
  assert(std::abs(c.tfactor[0] - 64 / 255.0f) < 0.00001f);
  static_assert(sizeof(browser::menu::ShaderConstants) == 3504);
  const auto shader = browser::menu::GenerateShaders(s, 1 | 8 | 512, true, tex);
  assert(shader.supported && shader.diagnostic.empty());
  assert(shader.vertex.find("@location(9) v9") != std::string::npos);
  assert(shader.vertex.find("p.z * u.surf.z") != std::string::npos);
  assert(shader.vertex.find("rhw.z / u.surf.z") != std::string::npos);
  assert(shader.fragment.find("floor(outc.a * 255.0 + 0.5)") != std::string::npos);
  assert(browser::menu::GenerateShaders(s, 1 | 8 | 512, true, tex).vertex == shader.vertex);
  const auto failure = browser::menu::DescribeShaderFailure(shader, "fixture compile error");
  assert(failure.find("fixture compile error") != std::string::npos);
  assert(failure.find(shader.vertex) != std::string::npos);
  assert(failure.find(shader.state) != std::string::npos);
  s.tss[0][gpu::kTssColorOp] = 4;
  s.tss[0][gpu::kTssColorArg1] = 2;
  s.tss[0][gpu::kTssColorArg2] = 0;
  s.tss[0][gpu::kTssAlphaOp] = 2;
  s.tss[0][gpu::kTssAlphaArg1] = 2;
  s.tss[0][gpu::kTssTextureTransformFlags] = 0x102;
  tex[0].kind = gpu::kTex2D;
  tex[0].rect = 1;
  const auto textured = browser::menu::GenerateShaders(s, 1 | 8 | 512, true, tex);
  assert(textured.supported);
  assert(textured.fragment.find("i.t0.xy / i.t0.w") != std::string::npos);
  assert(textured.fragment.find("/ u.texsize[0].xy") != std::string::npos);
  assert(textured.fragment.find("textureSample(tex0, smp0") != std::string::npos);
  if (argc == 2) {
    const auto save = [&](const std::string& name, const browser::menu::ShaderResult& fixture) {
      assert(fixture.supported);
      std::ofstream(std::string(argv[1]) + "/" + name + ".vs.wgsl") << fixture.vertex;
      std::ofstream(std::string(argv[1]) + "/" + name + ".fs.wgsl") << fixture.fragment;
    };
    save("plain", shader);
    save("textured", textured);
    auto stages = s;
    gpu::TexInfo stage_tex[4]{};
    for (int n = 0; n < 4; ++n) {
      stages.tss[n] = s.tss[0];
      stages.tss[n][gpu::kTssAlphaKill] = 4;
      stages.tss[n][gpu::kTssResultArg] = n == 1 ? 5 : 1;
      stage_tex[n] = tex[0];
    }
    stages.simple[gpu::kMShadeMode / 4] = 0x1d00;
    save("stages", browser::menu::GenerateShaders(stages, 0xffff, false, stage_tex));
    for (int n = 2; n <= 24; ++n) {
      if (n > 16 && n < 23) {
        continue;
      }
      auto variant = s;
      variant.tss[0][gpu::kTssColorOp] = uint32_t(n);
      variant.tss[0][gpu::kTssAlphaOp] = uint32_t(n);
      variant.tss[0][gpu::kTssColorArg0] = 3;
      variant.tss[0][gpu::kTssAlphaArg0] = 3;
      variant.tss[0][gpu::kTssColorArg1] = 0x32;
      variant.tss[0][gpu::kTssAlphaArg1] = 0x12;
      save("op" + std::to_string(n), browser::menu::GenerateShaders(variant, 0xffff, false, tex));
    }
    const auto constants = browser::menu::BuildConstants(s, 4, 4);
    std::ofstream bytes(std::string(argv[1]) + "/constants.bin", std::ios::binary);
    bytes.write(reinterpret_cast<const char*>(&constants), sizeof(constants));
  }
  s.tss[0][gpu::kTssColorOp] = 99;
  auto rejected = browser::menu::GenerateShaders(s, 1, true, tex);
  assert(!rejected.supported && rejected.vertex.empty() && rejected.fragment.empty());
  assert(rejected.diagnostic.find("color operation 99") != std::string::npos);
  s.tss[0][gpu::kTssColorOp] = 4;
  s.ps = std::make_shared<gpu::PixelShader>();
  rejected = browser::menu::GenerateShaders(s, 1, true, tex);
  assert(!rejected.supported && rejected.diagnostic.find("programmable pixel") != std::string::npos);
  s.ps.reset();
  s.vs_handle = 1;
  s.vs = std::make_shared<gpu::VertexShader>();
  s.vs->program = {0, 0, 0, 1};
  rejected = browser::menu::GenerateShaders(s, 1, false, tex);
  assert(!rejected.supported && rejected.diagnostic.find("programmable vertex") != std::string::npos);
  bool threw = false;
  try {
    browser::menu::BuildConstants(s, 0, 480);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  assert(threw);
  std::cout << "menu-shaders-ok\n";
}
