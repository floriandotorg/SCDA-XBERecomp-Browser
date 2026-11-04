#include "browser/menu-pixel-shaders.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <string>

namespace {

uint32_t Inputs(uint32_t a, uint32_t b, uint32_t c = 0, uint32_t d = 0) {
  return a << 24 | b << 16 | c << 8 | d;
}

gpu::PixelKey Key() {
  gpu::PixelKey key{};
  key.programmable = 1;
  key.count = 1;
  key.rgb_in[0] = Inputs(4, 0x20);
  key.rgb_out[0] = 12 << 4;
  key.a_in[0] = Inputs(0x14, 0x20);
  key.a_out[0] = 12 << 4;
  return key;
}

std::string Body(const gpu::PixelKey& key) {
  const auto result = browser::menu::TranslatePixel(key);
  assert(result.supported && result.diagnostic.empty() && !result.body.empty());
  return result.body;
}

void Contains(const std::string& body, const std::string& text) {
  if (body.find(text) == std::string::npos) {
    std::cerr << "Missing: " << text << "\n" << body;
    assert(false);
  }
}

void Reject(const gpu::PixelKey& key, const std::string& diagnostic) {
  const auto result = browser::menu::TranslatePixel(key);
  assert(!result.supported && result.body.empty());
  Contains(result.diagnostic, diagnostic);
}

}

int main(int argc, char** argv) {
  auto key = Key();
  const auto plain = Body(key);
  Contains(plain, "var v0 = i.d0;");
  Contains(plain, "if (u.misc.w == 0.0) { v1 = vec4<f32>(0.0, 0.0, 0.0, 1.0); }");
  Contains(plain, "var t0 = vec4<f32>(0.0, 0.0, 0.0, 1.0);");
  Contains(plain, "var r0 = vec4<f32>(0.0, 0.0, 0.0, 1.0);");
  Contains(plain, "var outc = r0;");
  Contains(plain, "outc = vec4<f32>(clamp(outc.rgb, vec3<f32>(0.0), vec3<f32>(1.0)), outc.a);");
  assert(plain == Body(key));
  const std::array<std::string, 8> rgb = {
    "max((v0).rgb, vec3<f32>(0.0))",
    "(vec3<f32>(1.0) - clamp((v0).rgb, vec3<f32>(0.0), vec3<f32>(1.0)))",
    "(2.0 * max((v0).rgb, vec3<f32>(0.0)) - vec3<f32>(1.0))",
    "(-2.0 * max((v0).rgb, vec3<f32>(0.0)) + vec3<f32>(1.0))",
    "(max((v0).rgb, vec3<f32>(0.0)) - vec3<f32>(0.5))",
    "(-max((v0).rgb, vec3<f32>(0.0)) + vec3<f32>(0.5))",
    "(v0).rgb", "(-(v0).rgb)"
  };
  const std::array<std::string, 8> native_rgb = {
    "max(v0.rgb, 0)", "(1 - saturate(v0.rgb))",
    "(2 * max(v0.rgb, 0) - 1)", "(-2 * max(v0.rgb, 0) + 1)",
    "(max(v0.rgb, 0) - 0.5)", "(-max(v0.rgb, 0) + 0.5)",
    "v0.rgb", "-v0.rgb"
  };
  const std::array<std::string, 8> alpha = {
    "max((v0).b, 0.0)", "(1.0 - clamp((v0).b, 0.0, 1.0))",
    "(2.0 * max((v0).b, 0.0) - 1.0)", "(-2.0 * max((v0).b, 0.0) + 1.0)",
    "(max((v0).b, 0.0) - 0.5)", "(-max((v0).b, 0.0) + 0.5)",
    "(v0).b", "(-(v0).b)"
  };
  for (int n = 0; n < 8; ++n) {
    key = Key();
    key.rgb_in[0] = Inputs(4 | uint32_t(n) << 5, 0x20);
    Contains(Body(key), rgb[n]);
    Contains(gpu::PixelHlsl(key), native_rgb[n]);
    key.rgb_in[0] = Inputs(0x14 | uint32_t(n) << 5, 0x20);
    auto replicated = rgb[n];
    replicated.replace(replicated.find(".rgb"), 4, ".aaa");
    Contains(Body(key), replicated);
    key.a_in[0] = Inputs(4 | uint32_t(n) << 5, 0x20);
    Contains(Body(key), alpha[n]);
    key.a_in[0] = Inputs(0x14 | uint32_t(n) << 5, 0x20);
    auto selected = alpha[n];
    selected.replace(selected.find(".b"), 2, ".a");
    Contains(Body(key), selected);
  }
  const std::array<uint32_t, 6> maps{0, 8, 16, 24, 32, 48};
  for (const auto mapping : maps) {
    key = Key();
    key.rgb_out[0] |= mapping << 12;
    key.a_out[0] |= mapping << 12;
    const auto body = Body(key);
    Contains(body, "let abc = clamp(");
    Contains(body, "let aba = clamp(");
    Contains(body, "vec3<f32>(-1.0), vec3<f32>(1.0)");
    Contains(body, ", -1.0, 1.0)");
    if (mapping == 8 || mapping == 24) {
      Contains(body, " - vec3<f32>(0.5)");
      Contains(body, " - 0.5");
    }
    if (mapping == 16 || mapping == 24) {
      Contains(body, " * 2.0");
    }
    if (mapping == 32) {
      Contains(body, " * 4.0");
    }
    if (mapping == 48) {
      Contains(body, " / 2.0");
    }
  }
  key = Key();
  key.count = 2 | (0x110u << 8);
  for (int n = 0; n < 2; ++n) {
    key.rgb_in[n] = Inputs(1, 2);
    key.rgb_out[n] = 12 << 4;
  }
  const auto constants = Body(key);
  for (int n = 0; n < 4; ++n) {
    Contains(constants, "u.pc[" + std::to_string(n) + "]");
  }
  key.count = 2;
  assert(Body(key).find("u.pc[2]") == std::string::npos);
  key = Key();
  key.rgb_in[0] = Inputs(0x1c, 0x20, 0x1d, 0x20);
  key.rgb_out[0] = (0xc0u << 12) | (13 << 8) | (12 << 4) | 5;
  key.a_in[0] = Inputs(12, 0x20);
  const auto simultaneous = Body(key);
  const auto alpha_read = simultaneous.find("let aba =");
  const auto color_write = simultaneous.find("r0 = vec4<f32>(abc, r0.a);");
  assert(alpha_read < color_write);
  Contains(simultaneous, "r0.a = abc.b;");
  Contains(simultaneous, "v1.a = cdc.b;");
  Contains(simultaneous, "r1 = vec4<f32>(msc, r1.a);");
  assert(color_write < simultaneous.find("r0.a = aba;"));
  const auto native_simultaneous = gpu::PixelHlsl(key);
  assert(native_simultaneous.find("float aba =") < native_simultaneous.find("r0.rgb = abc;"));
  Contains(native_simultaneous, "r0.a = abc.b;");
  Contains(native_simultaneous, "v1.a = cdc.b;");
  Contains(native_simultaneous, "r1.rgb = msc;");
  for (const auto destination : {4u, 5u, 8u, 9u, 10u, 11u, 12u, 13u}) {
    key = Key();
    key.rgb_out[0] = destination << 4 | destination << 8 | destination;
    key.a_out[0] = key.rgb_out[0];
    Body(key);
  }
  key = Key();
  key.count |= 1 << 8;
  key.rgb_out[0] |= 4 << 12;
  const auto mux = Body(key);
  Contains(mux, "select(");
  Contains(mux, "r0.a >= 0.5");
  assert(mux.find("select(clamp(") == std::string::npos);
  key.count &= ~(1 << 8);
  Reject(key, "LSB mux");
  key = Key();
  key.tex_modes = 1 | (4 << 5);
  key.tex[0].kind = gpu::kTex2D;
  key.tex[0].rect = 1;
  key.alphakill = 3;
  const auto textured = Body(key);
  Contains(textured, "textureSampleBias(tex0, smp0, (i.t0.xy / i.t0.w) / u.texsize[0].xy + u.texsize[0].zw, u.lod_bias[0])");
  Contains(textured, "var t1 = i.t1;");
  Contains(textured, "if (t0.a == 0.0) { discard; }");
  assert(textured.find("if (t1.a") == std::string::npos);
  Contains(textured, "var r0 = vec4<f32>(0.0, 0.0, 0.0, t0.a);");
  key = Key();
  key.final_abcd = Inputs(4, 1, 2, 12);
  key.final_efg = Inputs(0, 0, 0x1c);
  const auto final = Body(key);
  Contains(final, "u.pc[16]");
  Contains(final, "u.pc[17]");
  Contains(final, "mix(");
  Contains(final, "(r0).a");
  const auto native_final = gpu::PixelHlsl(key);
  Contains(native_final, "max(pc[16].rgb, 0)");
  Contains(native_final, "max(pc[17].rgb, 0)");
  Contains(native_final, "outc.rgb = max(r0.rgb, 0) + lerp(max(pc[17].rgb, 0), max(pc[16].rgb, 0), max(v0.rgb, 0));");
  Contains(native_final, "outc.a = max(r0.a, 0);");
  key.final_efg |= 0x80;
  assert(Body(key) == final);
  key.final_efg |= 1;
  Reject(key, "final combiner flags");
  key = Key();
  key.final_abcd = Inputs(14, 0x20);
  Contains(Body(key), "v1.rgb + r0.rgb");
  key.final_efg = 0xe0;
  Contains(Body(key), "vec3<f32>(1.0) - v1.rgb");
  Contains(Body(key), "vec3<f32>(1.0) - r0.rgb");
  Contains(Body(key), "clamp(");
  key.final_abcd = Inputs(15, 0x20);
  Contains(Body(key), "vec4<f32>(");
  key.final_abcd = Inputs(4, 0x20);
  key.final_efg = Inputs(15, 0);
  Reject(key, "final combiner register 15");
  for (const uint32_t mode : {15u, 16u}) {
    key = Key();
    key.tex_modes = 4 | (mode << 10);
    key.tex[2].kind = gpu::kTex2D;
    const auto body = Body(key);
    Contains(body, mode == 15 ? "t0.ar" : "t0.gb");
    Contains(body, "textureSampleBias(tex2, smp2");
    key.tex[2].rect = 1;
    Contains(Body(key), "/ u.texsize[2].xy");
    key.alphakill = 4;
    Contains(Body(key), "if (t2.a == 0.0) { discard; }");
    key.input_texture = 2u << 16;
    Reject(key, "input must precede");
  }
  for (int n = 0; n < 32; ++n) {
    if (n <= 4 || n == 9 || n == 12 || n == 15 || n == 16 || n == 17) {
      continue;
    }
    key = Key();
    key.tex_modes = uint32_t(n);
    key.tex[0].kind = gpu::kTex2D;
    Reject(key, "texture mode " + std::to_string(n));
  }
  for (const auto reg : {3u, 6u, 7u, 14u, 15u}) {
    key = Key();
    key.rgb_in[0] = Inputs(reg, 0);
    Reject(key, "register " + std::to_string(reg));
  }
  for (const auto reg : {1u, 2u, 3u, 6u, 7u, 14u, 15u}) {
    key = Key();
    key.rgb_out[0] = reg;
    Reject(key, "destination " + std::to_string(reg));
  }
  for (const auto flag : {1u, 2u}) {
    key = Key();
    key.rgb_out[0] |= flag << 12;
    Contains(Body(key), "vec3<f32>(dot(");
    key.a_out[0] |= flag << 12;
    Reject(key, "alpha output flags");
  }
  key = Key();
  key.a_out[0] |= 0x80u << 12;
  Reject(key, "alpha output flags");
  key = Key();
  key.tex_modes = 1;
  key.tex[0].shadow = 1;
  Reject(key, "shadow");
  key = Key();
  key.tex_modes = 1 | (17u << 5) | (17u << 10) | (12u << 15);
  key.tex[0].kind = gpu::kTex2D;
  key.tex[3].kind = gpu::kTexCube;
  key.dot_mapping = 0x111;
  const auto reflected = Body(key);
  Contains(reflected, "let dot1 = dot(i.t1.xyz");
  Contains(reflected, "(t0.rgb * 255.0 - vec3<f32>(128.0)) / 127.0");
  Contains(reflected, "let eye3 = vec3<f32>(i.t1.w, i.t2.w, i.t3.w)");
  Contains(reflected, "textureSampleBias(tex3, smp3, reflected3, u.lod_bias[3])");
  key = Key();
  key.tex_modes = 1 | (17u << 10) | (9u << 15);
  key.tex[0].kind = key.tex[3].kind = gpu::kTex2D;
  Contains(Body(key), "vec2<f32>(dot2, dot3)");
  key.tex_modes = 1 | (9u << 15);
  Reject(key, "preceding dot stage");
  key = Key();
  key.compare_mode = 1;
  Reject(key, "compare mode");
  key = Key();
  key.dot_mapping = 8;
  Reject(key, "dot mapping");
  key = Key();
  key.input_texture = 1;
  Reject(key, "input texture bits");
  key = Key();
  key.tex_modes = 1;
  Reject(key, "missing 2D texture");
  key.tex[0].kind = gpu::kTexCube;
  key.tex_modes = 3;
  Contains(Body(key), "textureSampleBias(tex0, smp0, i.t0.xyz, u.lod_bias[0])");
  key.tex_modes = 2;
  Contains(Body(key), "i.t0.xyz / i.t0.w");
  key.tex[0].kind = gpu::kTex2D;
  key.tex_modes = 1;
  key.tex[0].flags = gpu::kTexHdr;
  Reject(key, "texture flags");
  key = Key();
  key.rgb_out[0] |= 0x28u << 12;
  Reject(key, "output mapping");
  key = Key();
  key.rgb_out[0] |= 0x100u << 12;
  Reject(key, "RGB output flags");
  key = Key();
  key.alphakill = 16;
  Reject(key, "alpha kill bits");
  key = Key();
  key.tex_modes = 1 << 20;
  Reject(key, "texture mode bits");
  key = Key();
  key.count |= 2 << 8;
  Reject(key, "combiner count flags");
  key = Key();
  key.count = 0;
  const auto empty = Body(key);
  assert(empty.find("let abc") == std::string::npos);
  key.count = 9;
  const auto limited = Body(key);
  const auto stages = std::count(limited.begin(), limited.end(), '{') - 1;
  assert(stages == 8);
  key = Key();
  key.quad = 1;
  Reject(key, "quad coordinate unstretch");
  key = Key();
  key.two_sided = 1;
  Reject(key, "two-sided");
  key = Key();
  key.programmable = 0;
  Reject(key, "not programmable");
  if (argc == 2) {
    const auto save = [&](const std::string& name, const std::string& body) {
      std::ofstream(std::string(argv[1]) + "/" + name + ".body.wgsl") << body;
    };
    save("plain", plain);
    save("textured", textured);
    save("constants", constants);
    save("simultaneous", simultaneous);
    save("mux", mux);
    save("final", final);
  }
  std::cout << "menu-pixel-shaders-ok\n";
}
