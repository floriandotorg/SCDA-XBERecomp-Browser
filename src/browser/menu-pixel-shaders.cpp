#include "browser/menu-pixel-shaders.h"

#include <algorithm>
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

  std::string Register(uint32_t reg) const {
    const uint32_t flags = key.count >> 8;
    switch (reg) {
      case 0: return "vec4<f32>(0.0)";
      case 1: return "u.pc[" + Number(stage == 8 ? 16 : flags & 0x10 ? stage * 2 : 0) + "]";
      case 2: return "u.pc[" + Number(stage == 8 ? 17 : flags & 0x100 ? stage * 2 + 1 : 1) + "]";
      case 4: case 5: case 8: case 9: case 10: case 11: case 12: case 13: return Destination(reg);
      default: throw std::invalid_argument(std::string(stage == 8 ? "final combiner register " : "combiner register ") + Number(reg));
    }
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
    if (flags & 3) {
      throw std::invalid_argument("combiner dot product");
    }
    if (flags & ~(alpha ? 0x3cu : 0xfcu)) {
      throw std::invalid_argument(alpha ? "alpha output flags" : "RGB output flags");
    }
    if ((flags & 4) && !(key.count & 0x100)) {
      throw std::invalid_argument("combiner LSB mux");
    }
    const auto a = Input(inputs >> 24, alpha);
    const auto b = Input((inputs >> 16) & 255, alpha);
    const auto c = Input((inputs >> 8) & 255, alpha);
    const auto d = Input(inputs & 255, alpha);
    const auto ab = "(" + a + " * " + b + ")";
    const auto cd = "(" + c + " * " + d + ")";
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
  if (key.dot_mapping) {
    throw std::invalid_argument("texture dot mapping");
  }
  if (key.input_texture) {
    throw std::invalid_argument("dependent input texture");
  }
  if (key.alphakill & ~15u) {
    throw std::invalid_argument("alpha kill bits");
  }
  std::string body;
  for (int n = 0; n < 4; ++n) {
    const auto name = Number(n);
    const auto mode = key.tex_modes >> (n * 5) & 31;
    const auto& texture = key.tex[n];
    const auto prefix = "texture stage " + name + " ";
    if (texture.shadow) {
      throw std::invalid_argument(prefix + "shadow compare");
    }
    if (texture.flags) {
      throw std::invalid_argument(prefix + "texture flags " + Number(texture.flags));
    }
    if (texture.kind != gpu::kTexNone && texture.kind != gpu::kTex2D) {
      throw std::invalid_argument(prefix + "texture dimensionality " + Number(texture.kind));
    }
    std::string value;
    switch (mode) {
      case 0: value = "vec4<f32>(0.0, 0.0, 0.0, 1.0)"; break;
      case 1: {
        if (texture.kind != gpu::kTex2D) {
          throw std::invalid_argument(prefix + "missing 2D texture");
        }
        auto uv = "(i.t" + name + ".xy / i.t" + name + ".w)";
        if (texture.rect) {
          uv += " / u.texsize[" + name + "].xy";
        }
        uv += " + u.texsize[" + name + "].zw";
        value = "textureSample(tex" + name + ", smp" + name + ", " + uv + ")";
        break;
      }
      case 4: value = "i.t" + name; break;
      default: throw std::invalid_argument(prefix + "texture mode " + Number(mode));
    }
    body += "  var t" + name + " = " + value + ";\n";
  }
  for (int n = 0; n < 4; ++n) {
    const auto mode = key.tex_modes >> (n * 5) & 31;
    if (mode == 1 && (key.alphakill & (1u << n))) {
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
      if (key.final_efg & 255) {
        throw std::invalid_argument("final combiner flags " + Number(key.final_efg & 255));
      }
      combiner.Input(key.final_efg >> 24, false);
      combiner.Input((key.final_efg >> 16) & 255, false);
      const auto a = combiner.Input(key.final_abcd >> 24, false);
      const auto b = combiner.Input((key.final_abcd >> 16) & 255, false);
      const auto c = combiner.Input((key.final_abcd >> 8) & 255, false);
      const auto d = combiner.Input(key.final_abcd & 255, false);
      const auto g = combiner.Input((key.final_efg >> 8) & 255, true);
      body += "  var outc = vec4<f32>(" + d + " + mix(" + c + ", " + b + ", " + a + "), " + g + ");\n";
    } else {
      body += "  var outc = r0;\n";
    }
    body += "  outc = vec4<f32>(clamp(outc.rgb, vec3<f32>(0.0), vec3<f32>(1.0)), outc.a);\n";
    result.body = std::move(body);
    result.supported = true;
  } catch (const std::invalid_argument& error) {
    result.diagnostic = error.what();
  }
  return result;
}

}
