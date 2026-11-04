#include "browser/menu-shaders.h"

#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

std::array<uint32_t, 4> Instruction(uint32_t mac, uint32_t output, uint32_t input, bool end = false) {
  return {0, (mac << 21) | (96 << 13) | (input << 9) | 0x1b,
    (2 << 26) | 0x360000 | (3 << 11) | 0x6c,
    (2u << 28) | (15 << 12) | (1 << 11) | (output << 3) | uint32_t(end)};
}

void Append(gpu::VertexShader& shader, const std::array<uint32_t, 4>& instruction) {
  shader.program.insert(shader.program.end(), instruction.begin(), instruction.end());
}

}

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
  s.vsc[97] = {0.5f, 1, 0.25f, 1};
  s.rs[gpu::kRsTextureFactor] = 0x80402010;
  for (int n = 0; n < 8; ++n) {
    s.simple[(gpu::kMPsConstant0 + n * 4) / 4] = 0x80000000u | (uint32_t(n + 1) << 16);
    s.simple[(gpu::kMPsConstant1 + n * 4) / 4] = 0x40000000u | (uint32_t(n + 1) << 8);
  }
  s.simple[gpu::kMPsFinalConstant0 / 4] = 0x10203040;
  s.simple[gpu::kMPsFinalConstant1 / 4] = 0x50607080;
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
  static_assert(sizeof(browser::menu::ShaderConstants) == 3824);
  static_assert(offsetof(browser::menu::ShaderConstants, pc) == 3504);
  for (int n = 0; n < 8; ++n) {
    assert(c.pc[n * 2][0] == float(n + 1) / 255 && c.pc[n * 2][3] == 128 / 255.0f);
    assert(c.pc[n * 2 + 1][1] == float(n + 1) / 255 && c.pc[n * 2 + 1][3] == 64 / 255.0f);
  }
  assert((c.pc[16] == std::array<float, 4>{32 / 255.0f, 48 / 255.0f, 64 / 255.0f, 16 / 255.0f}));
  assert((c.pc[17] == std::array<float, 4>{96 / 255.0f, 112 / 255.0f, 128 / 255.0f, 80 / 255.0f}));
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
  assert(textured.fragment.find("textureSampleBias(tex0, smp0") != std::string::npos);
  auto borderStorage = std::make_unique<gpu::State>(s);
  auto& borderState = *borderStorage;
  borderState.tss[0][gpu::kTssAddressU] = 4;
  borderState.tss[0][gpu::kTssAddressV] = 4;
  borderState.tss[0][gpu::kTssBorderColor] = 0xff0000ff;
  borderState.tss[0][gpu::kTssMinFilter] = borderState.tss[0][gpu::kTssMagFilter] = 2;
  borderState.tss[0][gpu::kTssMipFilter] = 1;
  borderState.simple[gpu::kMAlphaTestEnable / 4] = 0;
  const auto bordered = browser::menu::GenerateShaders(borderState, 1 | 8 | 512, true, tex);
  assert(bordered.supported && bordered.fragment.find("fn border_sample0") != std::string::npos);
  auto flat = s;
  flat.simple[gpu::kMShadeMode / 4] = 0x1d00;
  const auto flat_shader = browser::menu::GenerateShaders(flat, 1 | 8 | 512, true, tex);
  assert(flat_shader.supported);
  for (const auto* source : {&flat_shader.vertex, &flat_shader.fragment}) {
    assert(source->find("@location(0) @interpolate(flat) d0") != std::string::npos);
    assert(source->find("@location(1) @interpolate(flat) d1") != std::string::npos);
    assert(source->find("@interpolate(flat) t0") == std::string::npos);
  }
  for (int n = 18; n <= 22; ++n) {
    auto variant = s;
    variant.tss[0][gpu::kTssColorOp] = uint32_t(n);
    variant.tss[0][gpu::kTssAlphaOp] = uint32_t(n);
    variant.tss[0][gpu::kTssColorArg1] = 0x32;
    variant.tss[0][gpu::kTssColorArg2] = 3;
    variant.tss[0][gpu::kTssAlphaArg1] = 0x12;
    variant.tss[0][gpu::kTssAlphaArg2] = n == 22 ? 3 : 0xffffffff;
    const auto combined = browser::menu::GenerateShaders(variant, 0xffff, false, tex);
    assert(combined.supported);
    if (n == 22) {
      assert(combined.fragment.find("vec3<f32>(dot(") != std::string::npos);
      assert(combined.fragment.find("vec3<f32>(((vec4<f32>(1.0) - t0)).a, 0.0, 0.0)") != std::string::npos);
    } else {
      assert(combined.fragment.find("let alpha = clamp(((vec4<f32>(1.0) - t0)).a, 0.0, 1.0)") != std::string::npos);
      assert(combined.fragment.find("((vec4<f32>(1.0) - (t0).aaaa)).a") != std::string::npos);
    }
  }
  auto pixel_state = s;
  pixel_state.ps = std::make_shared<gpu::PixelShader>();
  pixel_state.ps->def[53] = 1;
  pixel_state.rs[gpu::kRsPsTextureModes] = 1;
  pixel_state.ps->def[34] = (4 << 24) | (8 << 16);
  pixel_state.ps->def[45] = 12 << 4;
  pixel_state.ps->def[0] = (0x14 << 24) | (0x18 << 16);
  pixel_state.ps->def[26] = 12 << 4;
  pixel_state.tss[0][gpu::kTssColorOp] = 26;
  const auto pixel_shader = browser::menu::GenerateShaders(pixel_state, 1 | 8 | 512, true, tex);
  if (!pixel_shader.supported) {
    throw std::runtime_error(pixel_shader.diagnostic + "\n" + pixel_shader.state);
  }
  assert(pixel_shader.fragment.find("var outc = r0") != std::string::npos);
  assert(pixel_shader.fragment.find("if (!ok) { discard; }") != std::string::npos);
  assert(pixel_shader.fragment.find("outc.rgb + i.d1.rgb") == std::string::npos);
  auto invalid_pixel = pixel_state;
  invalid_pixel.ps = std::make_shared<gpu::PixelShader>(*pixel_state.ps);
  invalid_pixel.rs[gpu::kRsPsTextureModes] = 5;
  const auto pixel_failure = browser::menu::GenerateShaders(invalid_pixel, 1 | 8 | 512, true, tex);
  assert(!pixel_failure.supported && pixel_failure.fragment.empty());
  assert(pixel_failure.diagnostic.find("texture mode 5") != std::string::npos);
  auto programmable = s;
  programmable.vs_handle = 1;
  programmable.vs = std::make_shared<gpu::VertexShader>();
  programmable.vs->program_type = 0x2078;
  Append(*programmable.vs, Instruction(1, 3, 3));
  Append(*programmable.vs, Instruction(1, 9, 9));
  Append(*programmable.vs, Instruction(1, 0, 0, true));
  const auto program = browser::menu::GenerateShaders(programmable, 1 | 8 | 512, false, tex);
  assert(program.supported);
  assert(program.vertex.find("oPos.x = mr.x") != std::string::npos);
  assert(program.vertex.find("p.z * u.surf.z") != std::string::npos);
  assert(program.fragment.find("i.t0.xy / i.t0.w") != std::string::npos);
  auto invalid = programmable;
  invalid.vs = std::make_shared<gpu::VertexShader>(*programmable.vs);
  invalid.vs->program.pop_back();
  auto invalid_program = browser::menu::GenerateShaders(invalid, 1, false, tex);
  assert(!invalid_program.supported && invalid_program.diagnostic.find("instruction length") != std::string::npos);
  for (const auto& fixture : std::array<std::pair<std::array<uint32_t, 4>, std::string>, 4>{
    std::pair{std::array<uint32_t, 4>{0, (1 << 21) | (192 << 13), 3 << 26, (15 << 12) | (1 << 11) | 1}, "constant register 192"},
    std::pair{std::array<uint32_t, 4>{0, 1 << 21, (13u << 28) | (1 << 26), (15 << 12) | (1 << 11) | 1}, "temporary register 13"},
    std::pair{Instruction(1, 6, 0, true), "output register 6"},
    std::pair{Instruction(14, 0, 0, true), "MAC operation 14"}}) {
    invalid.vs->program.assign(fixture.first.begin(), fixture.first.end());
    invalid_program = browser::menu::GenerateShaders(invalid, 1, false, tex);
    assert(!invalid_program.supported && invalid_program.vertex.empty());
    assert(invalid_program.diagnostic.find(fixture.second) != std::string::npos);
  }
  auto reciprocal = programmable;
  reciprocal.vs = std::make_shared<gpu::VertexShader>();
  reciprocal.vs->program_type = 0x2078;
  auto rcc = Instruction(0, 3, 3);
  rcc[1] |= 3u << 25;
  rcc[2] = (rcc[2] & ~(255u << 2)) | (0x72u << 2);
  rcc[3] |= 4;
  Append(*reciprocal.vs, rcc);
  rcc[3] = (rcc[3] & ~(255u << 3)) | (9u << 3);
  Append(*reciprocal.vs, rcc);
  Append(*reciprocal.vs, Instruction(1, 0, 0, true));
  const auto reciprocal_shader = browser::menu::GenerateShaders(reciprocal, 1 | 8 | 512, false, tex);
  assert(reciprocal_shader.supported);
  assert(reciprocal_shader.vertex.find("let ir = vec4<f32>(xrcc(((i.v3).yyyy).x))") != std::string::npos);
  assert(reciprocal_shader.vertex.find("bitcast<u32>(v) & 0x80000000u") != std::string::npos);
  assert(reciprocal_shader.vertex.find("1.0 / max(abs(v), 1.0 / 1.84467e19)") != std::string::npos);
  auto reciprocalStorage = std::make_unique<gpu::State>(reciprocal);
  auto& plainReciprocal = *reciprocalStorage;
  plainReciprocal.vs = std::make_shared<gpu::VertexShader>(*reciprocal.vs);
  for (size_t n = 0; n < plainReciprocal.vs->program.size(); n += 4) {
    auto& token = plainReciprocal.vs->program[n + 1];
    if (((token >> 25) & 7) == 3) { token = (token & ~(7u << 25)) | (2u << 25); }
  }
  const auto plainReciprocalShader = browser::menu::GenerateShaders(plainReciprocal, 1 | 8 | 512, false, tex);
  assert(plainReciprocalShader.supported && plainReciprocalShader.vertex.find("let ir = vec4<f32>(xrcp(") != std::string::npos);
  auto normalizedStorage = std::make_unique<gpu::State>(reciprocal);
  auto& normalized = *normalizedStorage;
  normalized.vs = std::make_shared<gpu::VertexShader>(*reciprocal.vs);
  for (size_t n = 0; n < normalized.vs->program.size(); n += 4) {
    auto& token = normalized.vs->program[n + 1];
    if (((token >> 25) & 7) == 3) { token = (token & ~(7u << 25)) | (4u << 25); }
  }
  normalized.simple[gpu::kMShadeMode / 4] = 0x1d00;
  const auto normalization_shader = browser::menu::GenerateShaders(normalized, 1 | 8 | 512, false, tex);
  assert(normalization_shader.supported && normalization_shader.vertex.find("xrsq(") != std::string::npos);
  auto paired_rcc = programmable;
  paired_rcc.vs = std::make_shared<gpu::VertexShader>();
  paired_rcc.vs->program_type = 0x2078;
  auto paired_reciprocal = rcc;
  paired_reciprocal[1] |= 4u << 21;
  paired_reciprocal[3] |= (15u << 24) | (15u << 16);
  Append(*paired_rcc.vs, paired_reciprocal);
  Append(*paired_rcc.vs, Instruction(1, 0, 0, true));
  const auto paired_rcc_shader = browser::menu::GenerateShaders(paired_rcc, 1 | 8 | 512, false, tex);
  assert(paired_rcc_shader.supported);
  assert(paired_rcc_shader.vertex.find(" + ((i.v3).yyyy)") != std::string::npos);
  assert(paired_rcc_shader.vertex.find("R1.x = ir.x") != std::string::npos);
  assert(paired_rcc_shader.vertex.find("R0.x = mr.x") != std::string::npos);
  assert(paired_rcc_shader.vertex.find("let ir =") < paired_rcc_shader.vertex.find("R0.x = mr.x"));
  invalid.vs->program = {0, 5u << 25, 0, 1};
  invalid_program = browser::menu::GenerateShaders(invalid, 1, false, tex);
  assert(!invalid_program.supported && invalid_program.diagnostic.find("ILU operation 5") != std::string::npos);
  invalid.vs->program = programmable.vs->program;
  invalid.vs->program.back() &= ~1u;
  invalid_program = browser::menu::GenerateShaders(invalid, 1, false, tex);
  assert(!invalid_program.supported && invalid_program.diagnostic.find("end token") != std::string::npos);
  for (int n = 1; n <= 13; ++n) {
    auto variant = programmable;
    variant.vs = std::make_shared<gpu::VertexShader>();
    variant.vs->program_type = 0x2078;
    Append(*variant.vs, Instruction(uint32_t(n), 3, 3));
    Append(*variant.vs, Instruction(1, 0, 0, true));
    assert(browser::menu::GenerateShaders(variant, 1 | 8, false, tex).supported);
  }
  auto paired = programmable;
  paired.vs = std::make_shared<gpu::VertexShader>();
  paired.vs->program_type = 0x2078;
  auto initialize = Instruction(1, 3, 3);
  initialize[3] = 15u << 24;
  Append(*paired.vs, initialize);
  auto simultaneous = Instruction(1, 3, 4);
  simultaneous[1] |= 1u << 25;
  simultaneous[3] = (1u << 28) | (15 << 24) | (15 << 16) | (15 << 12) | (1 << 11) | (3 << 3) | 4;
  Append(*paired.vs, simultaneous);
  Append(*paired.vs, Instruction(1, 0, 0, true));
  const auto paired_shader = browser::menu::GenerateShaders(paired, 1 | 8 | 512, false, tex);
  assert(paired_shader.supported);
  assert(paired_shader.vertex.find("let ir = ((R0).xyzw)") < paired_shader.vertex.find("R0.x = mr.x", paired_shader.vertex.find("let ir =")));
  auto relative = programmable;
  relative.vs = std::make_shared<gpu::VertexShader>();
  relative.vs->program_type = 0x2078;
  auto address = Instruction(13, 0, 0);
  address[2] = (address[2] & ~(3u << 26)) | (3u << 26);
  Append(*relative.vs, address);
  auto indexed = Instruction(1, 3, 0);
  indexed[2] = (indexed[2] & ~(3u << 26)) | (3u << 26);
  indexed[3] |= 2;
  Append(*relative.vs, indexed);
  Append(*relative.vs, Instruction(1, 0, 0, true));
  const auto relative_shader = browser::menu::GenerateShaders(relative, 1 | 8 | 512, false, tex);
  assert(relative_shader.supported && relative_shader.vertex.find("u.vsc[clamp(a0 + 96, 0, 191)]") != std::string::npos);
  if (argc == 2) {
    const auto save = [&](const std::string& name, const browser::menu::ShaderResult& fixture) {
      assert(fixture.supported);
      std::ofstream(std::string(argv[1]) + "/" + name + ".vs.wgsl") << fixture.vertex;
      std::ofstream(std::string(argv[1]) + "/" + name + ".fs.wgsl") << fixture.fragment;
    };
    save("plain", shader);
    save("textured", textured);
    const auto depthOnly = browser::menu::GenerateShaders(s, 1 | 8 | 512, true, tex, 0, false);
    assert(depthOnly.supported && depthOnly.fragment.find("return outc") == std::string::npos);
    save("depth-only", depthOnly);
    gpu::TexInfo shadowTextures[4]{};
    shadowTextures[0].kind = gpu::kTex2D;
    shadowTextures[0].rect = 1;
    auto shadowState = std::make_unique<gpu::State>(s);
    shadowState->simple[gpu::kMAlphaTestEnable / 4] = 0;
    shadowState->tss[0][gpu::kTssColorOp] = shadowState->tss[0][gpu::kTssAlphaOp] = 2;
    shadowState->tss[0][gpu::kTssColorArg1] = shadowState->tss[0][gpu::kTssAlphaArg1] = 2;
    shadowState->tss[0][gpu::kTssAddressU] = shadowState->tss[0][gpu::kTssAddressV] = 3;
    shadowState->tss[0][gpu::kTssMagFilter] = shadowState->tss[0][gpu::kTssMinFilter] = 2;
    for (uint32_t n = 0; n < 16; ++n) {
      shadowTextures[0].shadow = n < 8 ? 1 : 2;
      shadowState->rs[gpu::kRsShadowFunc] = 0x200 + n % 8;
      const auto shadow = browser::menu::GenerateShaders(*shadowState, 1 | 8 | 512, true, shadowTextures);
      assert(shadow.supported && shadow.fragment.find("texture_depth_2d") != std::string::npos);
      save("shadow" + std::to_string(n), shadow);
      shadowState->ps = pixel_state.ps;
      shadowState->rs[gpu::kRsPsTextureModes] = 2;
      save("psshadow" + std::to_string(n), browser::menu::GenerateShaders(*shadowState, 1 | 8 | 512, true, shadowTextures));
      shadowState->ps.reset();
    }

    save("depth-bias", browser::menu::GenerateShaders(s, 1 | 8 | 512, true, tex, 0, false, true));
    save("depth-bias-color", browser::menu::GenerateShaders(s, 1 | 8 | 512, true, tex, 0, true, true));
    save("border", bordered);
    borderState.tss[0][gpu::kTssMipFilter] = 2;
    save("border-mip", browser::menu::GenerateShaders(borderState, 1 | 8 | 512, true, tex));
    gpu::TexInfo cubeTextures[4]{};
    cubeTextures[0].kind = gpu::kTexCube;
    const auto cube = browser::menu::GenerateShaders(s, 1 | 8 | 512, true, cubeTextures);
    assert(cube.supported && cube.fragment.find("texture_cube<f32>") != std::string::npos);
    save("cube", cube);
    auto cubeState = pixel_state;
    cubeState.rs[gpu::kRsPsTextureModes] = 3;
    save("pscube", browser::menu::GenerateShaders(cubeState, 1 | 8 | 512, true, cubeTextures));
    gpu::TexInfo volumeTextures[4]{};
    volumeTextures[0].kind = gpu::kTex3D;
    const auto volume = browser::menu::GenerateShaders(s, 1 | 8 | 512, true, volumeTextures);
    assert(volume.supported && volume.fragment.find("texture_3d<f32>") != std::string::npos);
    save("volume", volume);
    for (uint32_t n = 1; n <= 3; ++n) {
      auto volumeState = pixel_state;
      volumeState.rs[gpu::kRsPsTextureModes] = n;
      const auto shader = browser::menu::GenerateShaders(volumeState, 1 | 8 | 512, true, volumeTextures);
      assert(shader.supported && shader.fragment.find("texture_3d<f32>") != std::string::npos);
      save("psvolume" + std::to_string(n), shader);
    }
    auto reflected = pixel_state;
    reflected.ps = std::make_shared<gpu::PixelShader>(*pixel_state.ps);
    reflected.ps->def[34] = (11u << 24) | (0x20u << 16);
    reflected.ps->def[0] = (0x1bu << 24) | (0x20u << 16);
    reflected.ps->def[55] = 0x111;
    reflected.rs[gpu::kRsPsTextureModes] = 1 | (17u << 5) | (17u << 10) | (12u << 15);
    for (uint32_t n = 0; n < 4; ++n) { reflected.tss[n][gpu::kTssTexCoordIndex] = n; }
    gpu::TexInfo reflectionTextures[4]{};
    reflectionTextures[0].kind = gpu::kTex2D;
    reflectionTextures[3].kind = gpu::kTexCube;
    save("psreflect", browser::menu::GenerateShaders(reflected, 0xffff, true, reflectionTextures));
    reflected.ps->def[55] = 0;
    reflected.rs[gpu::kRsPsTextureModes] = 1 | (17u << 10) | (9u << 15);
    reflectionTextures[3].kind = gpu::kTex2D;
    save("psdotst", browser::menu::GenerateShaders(reflected, 0xffff, true, reflectionTextures));
    for (const uint32_t mode : {15u, 16u}) {
      auto dependent = pixel_state;
      dependent.ps = std::make_shared<gpu::PixelShader>(*pixel_state.ps);
      dependent.ps->def[34] = (10u << 24) | (0x20u << 16);
      dependent.ps->def[0] = (0x1au << 24) | (0x20u << 16);
      dependent.rs[gpu::kRsPsTextureModes] = 4 | (mode << 10);
      for (uint32_t n = 0; n < 4; ++n) { dependent.tss[n][gpu::kTssTexCoordIndex] = n; }
      gpu::TexInfo dependentTextures[4]{};
      dependentTextures[2].kind = gpu::kTex2D;
      save("psdep" + std::to_string(mode), browser::menu::GenerateShaders(dependent, 0xffff, true, dependentTextures));
    }

    save("flat", flat_shader);
    save("pixel", pixel_shader);
    for (uint32_t n = 0; n < 8; ++n) {
      auto summed = pixel_state;
      summed.ps = std::make_shared<gpu::PixelShader>(*pixel_state.ps);
      summed.ps->def[34] = (4u << 24) | (8u << 16) | (4u << 8) | 0x20u;
      summed.ps->def[45] = (12u << 4) | 5u;
      summed.ps->def[8] = (0x8eu << 24) | (0x20u << 16);
      summed.ps->def[9] = (0x20u << 8) | (n << 5);
      save("pssum" + std::to_string(n), browser::menu::GenerateShaders(summed, 1 | 8 | 512, true, tex));
    }
    auto multiplied = pixel_state;
    multiplied.ps = std::make_shared<gpu::PixelShader>(*pixel_state.ps);
    multiplied.ps->def[8] = 15;
    multiplied.ps->def[9] = (4u << 24) | (8u << 16) | (0x20u << 8);
    save("psef", browser::menu::GenerateShaders(multiplied, 1 | 8 | 512, true, tex));
    for (uint32_t n = 1; n <= 3; ++n) {
      auto dotted = pixel_state;
      dotted.ps = std::make_shared<gpu::PixelShader>(*pixel_state.ps);
      dotted.ps->def[34] = (4u << 24) | (8u << 16) | (4u << 8) | 8u;
      dotted.ps->def[45] = (n << 12) | (n == 1 ? 12u : n == 2 ? 12u << 4 : 12u << 8);
      save("psdot" + std::to_string(n), browser::menu::GenerateShaders(dotted, 1 | 8 | 512, true, tex));
    }
    auto kill = pixel_state;
    kill.tss[0][gpu::kTssAlphaKill] = 4;
    kill.simple[gpu::kMAlphaTestEnable / 4] = 0;
    save("pskill", browser::menu::GenerateShaders(kill, 1 | 8 | 512, true, tex));
    auto both = pixel_state;
    both.vs_handle = programmable.vs_handle;
    both.vs = programmable.vs;
    save("pixel-program", browser::menu::GenerateShaders(both, 1 | 8 | 512, false, tex));
    const auto ps_variant = [&]() {
      auto variant = pixel_state;
      variant.ps = std::make_shared<gpu::PixelShader>(*pixel_state.ps);
      variant.simple[gpu::kMAlphaTestEnable / 4] = 0;
      return variant;
    };
    for (int n = 0; n < 8; ++n) {
      auto variant = ps_variant();
      variant.ps->def[34] = ((4u | (uint32_t(n) << 5)) << 24) | (0x20 << 16);
      variant.ps->def[0] = ((0x14u | (uint32_t(n) << 5)) << 24) | (0x20 << 16);
      save("psinput" + std::to_string(n), browser::menu::GenerateShaders(variant, 1 | 8 | 512, true, tex));
    }
    for (const auto mapping : {0u, 8u, 16u, 24u, 32u, 48u}) {
      auto variant = ps_variant();
      variant.ps->def[34] = (4 << 24) | (0x20 << 16);
      variant.ps->def[0] = (0x14 << 24) | (0x20 << 16);
      variant.ps->def[45] |= mapping << 12;
      variant.ps->def[26] |= mapping << 12;
      save("psoutput" + std::to_string(mapping), browser::menu::GenerateShaders(variant, 1 | 8 | 512, true, tex));
    }
    auto final_constants = ps_variant();
    final_constants.ps->def[8] = (1 << 24) | (2 << 16);
    final_constants.ps->def[9] = 0x12 << 8;
    save("psconstants", browser::menu::GenerateShaders(final_constants, 1 | 8 | 512, true, tex));
    auto mux = ps_variant();
    mux.ps->def[53] |= 1 << 8;
    mux.ps->def[34] = (4 << 24) | (0x20 << 16) | (8 << 8) | 0x20;
    mux.ps->def[45] = (12 << 8) | (4 << 12);
    save("psmux", browser::menu::GenerateShaders(mux, 1 | 8 | 512, true, tex));
    auto simultaneous_pixel = ps_variant();
    simultaneous_pixel.ps->def[53] = 2;
    simultaneous_pixel.ps->def[34] = (4 << 24) | (0x20 << 16);
    simultaneous_pixel.ps->def[35] = (0x1c << 24) | (0x20 << 16);
    simultaneous_pixel.ps->def[46] = 12 << 4;
    simultaneous_pixel.ps->def[1] = (12 << 24) | (0x20 << 16);
    simultaneous_pixel.ps->def[27] = 12 << 4;
    save("pssimultaneous", browser::menu::GenerateShaders(simultaneous_pixel, 1 | 8 | 512, true, tex));
    save("program", program);
    save("paired", paired_shader);
    save("relative", relative_shader);
    save("rcc", reciprocal_shader);
    save("rcp", plainReciprocalShader);
    save("rsq", normalization_shader);
    for (int n = 1; n <= 13; ++n) {
      auto variant = programmable;
      variant.vs = std::make_shared<gpu::VertexShader>();
      variant.vs->program_type = 0x2078;
      Append(*variant.vs, Instruction(uint32_t(n), 3, 3));
      Append(*variant.vs, Instruction(1, 0, 0, true));
      save("mac" + std::to_string(n), browser::menu::GenerateShaders(variant, 1 | 8 | 512, false, tex));
    }
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
      if (n == 17) {
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
    for (int n = 18; n <= 22; ++n) {
      auto variant = s;
      variant.tss[0][gpu::kTssColorOp] = uint32_t(n);
      variant.tss[0][gpu::kTssAlphaOp] = uint32_t(n);
      variant.tss[0][gpu::kTssColorArg1] = 2;
      variant.tss[0][gpu::kTssColorArg2] = 3;
      variant.tss[0][gpu::kTssAlphaArg1] = 2;
      variant.tss[0][gpu::kTssAlphaArg2] = 3;
      save("combine" + std::to_string(n), browser::menu::GenerateShaders(variant, 1 | 8 | 512, true, tex));
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
  s.ps->def[53] = 1;
  s.ps->def[34] = 6u << 24;
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
