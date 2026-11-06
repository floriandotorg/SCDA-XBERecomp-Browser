#include "browser/menu-pixel-shaders.h"
#include "browser/menu-shaders.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace browser::menu {

namespace {

std::string Number(uint32_t value) {
  return std::to_string(value);
}

std::string Scalar(bool alpha, const std::string& value) {
  return alpha ? value : "vec3<f32>(" + value + ")";
}

std::string Destination(uint32_t reg) {
  switch (reg) {
    case 0: return {};
    case 4: return "v0";
    case 5: return "v1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    default: throw std::invalid_argument("destination " + Number(reg));
  }
}

struct Combiner {
  const gpu::PixelKey& key;
  uint32_t stage = 0;
  std::string e, f;

  std::string Register(uint32_t reg) const {
    const uint32_t flags = key.count >> 8;
    switch (reg) {
      case 0: return "vec4<f32>(0.0)";
      case 1: return "u.pc[" + Number(stage == 8 ? 16 : flags & 0x10 ? stage * 2 : 0) + "]";
      case 2: return "u.pc[" + Number(stage == 8 ? 17 : flags & 0x100 ? stage * 2 + 1 : 1) + "]";
      case 4: case 5: case 8: case 9: case 10: case 11: case 12: case 13: return Destination(reg);
      case 14: {
        if (stage != 8) { break; }
        const auto left = std::string(key.final_efg & 0x40 ? "(vec3<f32>(1.0) - v1.rgb)" : "v1.rgb");
        const auto right = std::string(key.final_efg & 0x20 ? "(vec3<f32>(1.0) - r0.rgb)" : "r0.rgb");
        auto sum = left + " + " + right;
        if (key.final_efg & 0x80) { sum = "clamp(" + sum + ", vec3<f32>(0.0), vec3<f32>(1.0))"; }
        return "vec4<f32>(" + sum + ", 0.0)";
      }
      case 15:
        if (stage == 8 && !e.empty() && !f.empty()) { return "vec4<f32>(" + e + " * " + f + ", 0.0)"; }
        break;
      default: break;
    }
    throw std::invalid_argument(std::string(stage == 8 ? "final combiner register " : "combiner register ") + Number(reg));
  }

  std::string Input(uint32_t input, bool alpha) const {
    const auto value = "(" + Register(input & 15) + ")" + (alpha ? input & 16 ? ".a" : ".b" : input & 16 ? ".aaa" : ".rgb");
    const auto zero = Scalar(alpha, "0.0");
    const auto one = Scalar(alpha, "1.0");
    const auto half = Scalar(alpha, "0.5");
    switch (input & 0xe0) {
      case 0x00: return "max(" + value + ", " + zero + ")";
      case 0x20: return "(" + one + " - clamp(" + value + ", " + zero + ", " + one + "))";
      case 0x40: return "(2.0 * max(" + value + ", " + zero + ") - " + one + ")";
      case 0x60: return "(-2.0 * max(" + value + ", " + zero + ") + " + one + ")";
      case 0x80: return "(max(" + value + ", " + zero + ") - " + half + ")";
      case 0xa0: return "(-max(" + value + ", " + zero + ") + " + half + ")";
      case 0xc0: return value;
      case 0xe0: return "(-" + value + ")";
    }
    throw std::logic_error("unvalidated input mapping");
  }

  static std::string Map(const std::string& value, uint32_t mapping, bool alpha) {
    const auto half = Scalar(alpha, "0.5");
    switch (mapping) {
      case 0x00: return value;
      case 0x08: return "(" + value + " - " + half + ")";
      case 0x10: return "(" + value + " * 2.0)";
      case 0x18: return "((" + value + " - " + half + ") * 2.0)";
      case 0x20: return "(" + value + " * 4.0)";
      case 0x30: return "(" + value + " / 2.0)";
      default: throw std::invalid_argument("output mapping " + Number(mapping));
    }
  }

  std::string Stage(uint32_t inputs, uint32_t outputs, bool alpha, std::string& writes) const {
    const auto flags = outputs >> 12;
    if (flags & ~(alpha ? 0x3cu : 0xffu)) {
      throw std::invalid_argument(alpha ? "alpha output flags" : "RGB output flags");
    }
    if ((flags & 4) && !(key.count & 0x100)) {
      throw std::invalid_argument("combiner LSB mux");
    }
    const auto a = Input(inputs >> 24, alpha);
    const auto b = Input((inputs >> 16) & 255, alpha);
    const auto c = Input((inputs >> 8) & 255, alpha);
    const auto d = Input(inputs & 255, alpha);
    const auto ab = flags & 2 ? "vec3<f32>(dot(" + a + ", " + b + "))" : "(" + a + " * " + b + ")";
    const auto cd = flags & 1 ? "vec3<f32>(dot(" + c + ", " + d + "))" : "(" + c + " * " + d + ")";
    const auto sum = flags & 4 ? "select(" + ab + ", " + cd + ", r0.a >= 0.5)" : "(" + ab + " + " + cd + ")";
    const std::string suffix = alpha ? "a" : "c";
    std::string body;
    const auto emit = [&](const std::string& name, const std::string& expression) {
      body += "    let " + name + suffix + " = clamp(" + Map(expression, flags & 0x38, alpha) + ", " + Scalar(alpha, "-1.0") + ", " + Scalar(alpha, "1.0") + ");\n";
    };
    emit("ab", ab);
    emit("cd", cd);
    emit("ms", sum);
    const auto write = [&](uint32_t reg, const std::string& name, bool copy_blue) {
      const auto destination = Destination(reg);
      if (destination.empty()) {
        return;
      }
      if (alpha) {
        writes += "    " + destination + ".a = " + name + suffix + ";\n";
      } else {
        writes += "    " + destination + " = vec4<f32>(" + name + suffix + ", " + destination + ".a);\n";
        if (copy_blue) {
          writes += "    " + destination + ".a = " + name + suffix + ".b;\n";
        }
      }
    };
    write((outputs >> 4) & 15, "ab", flags & 0x80);
    write(outputs & 15, "cd", flags & 0x40);
    write((outputs >> 8) & 15, "ms", false);
    return body;
  }
};

std::string Textures(const gpu::PixelKey& key) {
  if (key.tex_modes & ~0xfffffu) {
    throw std::invalid_argument("texture mode bits");
  }
  if (key.compare_mode) {
    throw std::invalid_argument("texture compare mode");
  }
  if (key.dot_mapping & ~0x777u) { throw std::invalid_argument("texture dot mapping bits"); }
  if (key.input_texture & ~0xff0000u) { throw std::invalid_argument("input texture bits"); }
  if (key.alphakill & ~15u) {
    throw std::invalid_argument("alpha kill bits");
  }
  std::string body;
  std::array<bool, 4> dots{};
  for (int n = 0; n < 4; ++n) {
    const auto name = Number(n);
    const auto mode = key.tex_modes >> (n * 5) & 31;
    const auto& texture = key.tex[n];
    const auto prefix = "texture stage " + name + " ";
    if (texture.shadow > 2 || (texture.shadow && (texture.kind != gpu::kTex2D || (mode != 1 && mode != 2)))) {
      throw std::invalid_argument(prefix + "shadow compare");
    }
    if (!SupportedFlags(texture)) {
      throw std::invalid_argument(prefix + "texture flags " + Number(texture.flags));
    }
    if (texture.kind != gpu::kTexNone && texture.kind != gpu::kTex2D && texture.kind != gpu::kTexCube && texture.kind != gpu::kTex3D) {
      throw std::invalid_argument(prefix + "texture dimensionality " + Number(texture.kind));
    }
    const auto dot = [&]() {
      if (!n) { throw std::invalid_argument(prefix + "dot product requires a preceding stage"); }
      const uint32_t input = n == 1 ? 0 : (key.input_texture >> (n == 2 ? 16 : 20)) & 15;
      if (input >= uint32_t(n)) { throw std::invalid_argument(prefix + "dot input must precede its consumer"); }
      const auto mapping = (key.dot_mapping >> ((n - 1) * 4)) & 15;
      auto value = "t" + Number(input) + ".rgb";
      switch (mapping) {
        case 0: case 4: case 5: case 6: case 7: break;
        case 1: value = "(" + value + " * 255.0 - vec3<f32>(128.0)) / 127.0"; break;
        case 2: value = "select((" + value + " * 255.0 + vec3<f32>(0.5)) / 127.5, (" + value + " * 255.0 - vec3<f32>(255.5)) / 127.5, " + value + " * 255.0 >= vec3<f32>(128.0))"; break;
        case 3: value = "select(" + value + " * 255.0 / 127.0, (" + value + " * 255.0 - vec3<f32>(256.0)) / 127.0, " + value + " * 255.0 >= vec3<f32>(128.0))"; break;
        default: throw std::invalid_argument(prefix + "dot mapping");
      }
      body += "  let dot" + name + " = dot(i.t" + name + ".xyz, " + value + ");\n";
      dots[n] = true;
    };
    std::string value;
    switch (mode) {
      case 0: value = "vec4<f32>(0.0, 0.0, 0.0, 1.0)"; break;
      case 1: {
        if (texture.kind != gpu::kTex2D && texture.kind != gpu::kTexCube && texture.kind != gpu::kTex3D) {
          throw std::invalid_argument(prefix + "missing 2D texture");
        }
        auto uv = "(i.t" + name + ".xy / i.t" + name + ".w)";
        if (texture.kind == gpu::kTexCube || texture.kind == gpu::kTex3D) { uv = "vec3<f32>(" + uv + (texture.kind == gpu::kTex3D ? ", 0.0)" : ", 1.0)"); }
        else {
          if (texture.rect) { uv += " / u.texsize[" + name + "].xy"; }
          uv += " + u.texsize[" + name + "].zw";
        }
        value = texture.shadow ? "shadow_sample" + name + "(" + uv + ", " + std::string(mode == 1 ? "0.0" : "i.t" + name + ".z / i.t" + name + ".w") + ")"
          : "textureSampleBias(tex" + name + ", smp" + name + ", " + uv + ", u.lod_bias[" + name + "])";
        break;
      }
      case 2: case 3: {
        if (texture.kind != gpu::kTexCube && texture.kind != gpu::kTex2D && texture.kind != gpu::kTex3D) { throw std::invalid_argument(prefix + "missing sampled texture"); }
        auto uv = "i.t" + name + (texture.kind == gpu::kTexCube || texture.kind == gpu::kTex3D ? ".xyz" : ".xy");
        if (mode == 2) { uv = "(" + uv + " / i.t" + name + ".w)"; }
        if (texture.kind == gpu::kTex2D) {
          if (texture.rect) { uv += " / u.texsize[" + name + "].xy"; }
          uv += " + u.texsize[" + name + "].zw";
        }
        value = texture.shadow ? "shadow_sample" + name + "(" + uv + ", " + std::string(mode == 1 ? "0.0" : "i.t" + name + ".z / i.t" + name + ".w") + ")"
          : "textureSampleBias(tex" + name + ", smp" + name + ", " + uv + ", u.lod_bias[" + name + "])";
        break;
      }
      case 4: value = "i.t" + name; break;
      case 9: {
        if (n < 2 || !dots[n - 1]) { throw std::invalid_argument(prefix + "dot lookup requires a preceding dot stage"); }
        dot();
        auto coordinate = "vec2<f32>(dot" + Number(n - 1) + ", dot" + name + ")";
        if (texture.kind == gpu::kTex2D) {
          if (texture.rect) { coordinate += " / u.texsize[" + name + "].xy"; }
          coordinate += " + u.texsize[" + name + "].zw";
        } else if (texture.kind == gpu::kTexCube || texture.kind == gpu::kTex3D) { coordinate = "vec3<f32>(" + coordinate + (texture.kind == gpu::kTex3D ? ", 0.0)" : ", 1.0)"); }
        else { throw std::invalid_argument(prefix + "missing dot lookup texture"); }
        value = "textureSampleBias(tex" + name + ", smp" + name + ", " + coordinate + ", u.lod_bias[" + name + "])";
        break;
      }
      case 15: case 16: {
        if (!n) { throw std::invalid_argument(prefix + "dependent lookup requires a preceding stage"); }
        const uint32_t input = n == 1 ? 0 : (key.input_texture >> (n == 2 ? 16 : 20)) & 15;
        if (input >= uint32_t(n)) { throw std::invalid_argument(prefix + "input must precede its consumer"); }
        auto coordinate = "t" + Number(input) + (mode == 15 ? ".ar" : ".gb");
        if (texture.kind == gpu::kTex2D) {
          if (texture.rect) { coordinate += " / u.texsize[" + name + "].xy"; }
          coordinate += " + u.texsize[" + name + "].zw";
        } else if (texture.kind == gpu::kTexCube || texture.kind == gpu::kTex3D) { coordinate = "vec3<f32>(" + coordinate + (texture.kind == gpu::kTex3D ? ", 0.0)" : ", 1.0)"); }
        else { throw std::invalid_argument(prefix + "missing dependent lookup texture"); }
        value = "textureSampleBias(tex" + name + ", smp" + name + ", " + coordinate + ", u.lod_bias[" + name + "])";
        break;
      }
      case 17:
        dot();
        value = "vec4<f32>(0.0)";
        break;
      case 12: {
        if (n != 3 || !dots[1] || !dots[2]) { throw std::invalid_argument(prefix + "reflection requires dot stages 1 and 2"); }
        dot();
        body += "  let normal3 = vec3<f32>(dot1, dot2, dot3);\n  let eye3 = vec3<f32>(i.t1.w, i.t2.w, i.t3.w);\n";
        body += "  let reflected3 = 2.0 * normal3 * dot(normal3, eye3) / dot(normal3, normal3) - eye3;\n";
        auto coordinate = std::string("reflected3");
        if (texture.kind == gpu::kTex2D) {
          coordinate += ".xy";
          if (texture.rect) { coordinate += " / u.texsize[" + name + "].xy"; }
          coordinate += " + u.texsize[" + name + "].zw";
        } else if (texture.kind != gpu::kTexCube && texture.kind != gpu::kTex3D) { throw std::invalid_argument(prefix + "missing reflection texture"); }
        value = "textureSampleBias(tex" + name + ", smp" + name + ", " + coordinate + ", u.lod_bias[" + name + "])";
        break;
      }
      default: throw std::invalid_argument(prefix + "texture mode " + Number(mode));
    }
    body += "  var t" + name + " = " + value + ";\n";
  }
  for (int n = 0; n < 4; ++n) {
    const auto mode = key.tex_modes >> (n * 5) & 31;
    if (((mode >= 1 && mode <= 3) || mode == 9 || mode == 12 || mode == 15 || mode == 16) && (key.alphakill & (1u << n))) {
      body += "  if (t" + Number(n) + ".a == 0.0) { discard; }\n";
    }
  }
  return body;
}

}

PixelTranslation TranslatePixel(const gpu::PixelKey& key) {
  PixelTranslation result;
  try {
    if (!key.programmable) {
      throw std::invalid_argument("pixel key is not programmable");
    }
    if (key.two_sided) {
      throw std::invalid_argument("two-sided pixel lighting");
    }
    if (key.quad) {
      throw std::invalid_argument("quad coordinate unstretch");
    }
    if (key.count & ~0x111ffu) {
      throw std::invalid_argument("combiner count flags");
    }
    std::string body = "  var v0 = i.d0;\n  var v1 = i.d1;\n  if (u.misc.w == 0.0) { v1 = vec4<f32>(0.0, 0.0, 0.0, 1.0); }\n";
    body += Textures(key);
    body += "  var r0 = vec4<f32>(0.0, 0.0, 0.0, " + std::string(key.tex_modes & 31 ? "t0.a" : "1.0") + ");\n  var r1 = vec4<f32>(0.0);\n";
    Combiner combiner{key};
    const auto count = std::min(key.count & 255, 8u);
    for (uint32_t n = 0; n < count; ++n) {
      combiner.stage = n;
      std::string color_writes, alpha_writes;
      body += "  {\n";
      body += combiner.Stage(key.rgb_in[n], key.rgb_out[n], false, color_writes);
      body += combiner.Stage(key.a_in[n], key.a_out[n], true, alpha_writes);
      body += color_writes + alpha_writes + "  }\n";
    }
    if (key.final_abcd || key.final_efg) {
      combiner.stage = 8;
      if (key.final_efg & 0x1f) {
        throw std::invalid_argument("final combiner flags " + Number(key.final_efg & 255));
      }
      combiner.e = combiner.Input(key.final_efg >> 24, false);
      combiner.f = combiner.Input((key.final_efg >> 16) & 255, false);
      const auto a = combiner.Input(key.final_abcd >> 24, false);
      const auto b = combiner.Input((key.final_abcd >> 16) & 255, false);
      const auto c = combiner.Input((key.final_abcd >> 8) & 255, false);
      const auto d = combiner.Input(key.final_abcd & 255, false);
      const auto g = combiner.Input((key.final_efg >> 8) & 255, true);
      body += "  var outc = vec4<f32>(" + d + " + mix(" + c + ", " + b + ", " + a + "), " + g + ");\n";
    } else {
      body += "  var outc = r0;\n";
    }
    body += "  outc = vec4<f32>(clamp(outc.rgb, vec3<f32>(0.0), vec3<f32>(1.0)) * u.hdr.x, outc.a);\n";
    result.body = std::move(body);
    result.supported = true;
  } catch (const std::invalid_argument& error) {
    result.diagnostic = error.what();
  }
  return result;
}

}
