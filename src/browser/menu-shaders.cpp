#include "browser/menu-shaders.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <stdexcept>

namespace browser::menu {

static_assert(sizeof(ShaderConstants) == 3504);
static_assert(offsetof(ShaderConstants, surf) == 3072);
static_assert(offsetof(ShaderConstants, misc) == 3088);
static_assert(offsetof(ShaderConstants, tfactor) == 3104);
static_assert(offsetof(ShaderConstants, texsize) == 3120);
static_assert(offsetof(ShaderConstants, wvp) == 3184);
static_assert(offsetof(ShaderConstants, tm) == 3248);

namespace {

using Matrix = std::array<float, 16>;

Matrix Multiply(const Matrix& a, const Matrix& b) {
  Matrix out{};
  for (int n = 0; n < 16; ++n) {
    const int row = n / 4 * 4;
    const int col = n % 4;
    out[n] = a[row] * b[col] + a[row + 1] * b[col + 4] + a[row + 2] * b[col + 8] + a[row + 3] * b[col + 12];
  }
  return out;
}

std::array<float, 4> Color(uint32_t c) {
  return {float((c >> 16) & 255) / 255, float((c >> 8) & 255) / 255, float(c & 255) / 255, float(c >> 24) / 255};
}

const char* kUniform = R"(struct Constants {
  vsc: array<vec4<f32>, 192>,
  surf: vec4<f32>,
  misc: vec4<f32>,
  tfactor: vec4<f32>,
  texsize: array<vec4<f32>, 4>,
  wvp: array<vec4<f32>, 4>,
  tm: array<array<vec4<f32>, 4>, 4>,
}
@group(0) @binding(0) var<uniform> u: Constants;
fn rowmul(p: vec4<f32>, m: array<vec4<f32>, 4>) -> vec4<f32> {
  return p.x * m[0] + p.y * m[1] + p.z * m[2] + p.w * m[3];
}
)";

std::string Varyings(bool flat) {
  const std::string interpolation = flat ? " @interpolate(flat)" : "";
  std::string out = "struct VOut {\n  @builtin(position) @invariant pos: vec4<f32>,\n";
  out += "  @location(0)" + interpolation + " d0: vec4<f32>,\n";
  out += "  @location(1)" + interpolation + " d1: vec4<f32>,\n";
  for (int n = 0; n < 4; ++n) {
    out += "  @location(" + std::to_string(n + 2) + ") t" + std::to_string(n) + ": vec4<f32>,\n";
  }
  return out + "}\n";
}

std::string Arg(uint32_t arg, int stage, bool alpha) {
  const std::string bases[] = {"i.d0", "cur", "t" + std::to_string(stage), "u.tfactor", "v1", "tmp"};
  std::string value = bases[arg & 15];
  if (arg & 32) {
    value = "(" + value + ").aaaa";
  }
  if (arg & 16) {
    value = "(vec4<f32>(1.0) - " + value + ")";
  }
  return "(" + value + ")" + (alpha ? ".a" : ".rgb");
}

std::string Op(uint32_t op, const std::string& a, const std::string& b, const std::string& c, int stage) {
  switch (op) {
    case 2: return a;
    case 3: return b;
    case 4: return a + " * " + b;
    case 5: return a + " * " + b + " * 2.0";
    case 6: return a + " * " + b + " * 4.0";
    case 7: return a + " + " + b;
    case 8: return a + " + " + b + " - 0.5";
    case 9: return "(" + a + " + " + b + " - 0.5) * 2.0";
    case 10: return a + " - " + b;
    case 11: return a + " + " + b + " * (1.0 - " + a + ")";
    case 12: return "(" + b + " * (1.0 - i.d0.a) + " + a + " * i.d0.a)";
    case 13: return "(" + b + " * (1.0 - cur.a) + " + a + " * cur.a)";
    case 14: return "(" + b + " * (1.0 - t" + std::to_string(stage) + ".a) + " + a + " * t" + std::to_string(stage) + ".a)";
    case 15: return "(" + b + " * (1.0 - u.tfactor.a) + " + a + " * u.tfactor.a)";
    case 16: return a + " + " + b + " * (1.0 - t" + std::to_string(stage) + ".a)";
    case 23: return c + " + " + a + " * " + b;
    case 24: return "(" + b + " * (1.0 - " + c + ") + " + a + " * " + c + ")";
    default: throw std::logic_error("unvalidated texture operation");
  }
}

bool SupportedOp(uint32_t op) {
  return (op >= 2 && op <= 16) || op == 23 || op == 24;
}

std::string ValidateArgs(const uint32_t* f, bool alpha) {
  const int base = alpha ? 4 : 0;
  const uint32_t op = f[base];
  if (alpha && op <= 1) {
    return {};
  }
  if (!SupportedOp(op)) {
    return std::string(alpha ? "alpha" : "color") + " operation " + std::to_string(op);
  }
  for (int n = 1; n <= 3; ++n) {
    if ((n == 1 && op == 3) || (n == 2 && op == 2) || (n == 3 && op != 23 && op != 24)) {
      continue;
    }
    const uint32_t arg = f[base + n];
    if ((arg & 15) > 5 || (arg & ~63u)) {
      return "texture argument " + std::to_string(arg);
    }
  }
  return {};
}

std::string StageExpression(const uint32_t* f, int stage, bool alpha) {
  const int base = alpha ? 4 : 0;
  const uint32_t op = f[base];
  if (alpha && op <= 1) {
    return "cur.a";
  }
  const auto arg = [&](int n) {
    if ((n == 1 && op == 3) || (n == 2 && op == 2) || (n == 3 && op != 23 && op != 24)) {
      return std::string("0.0");
    }
    return Arg(f[base + n], stage, alpha);
  };
  return Op(op, arg(1), arg(2), arg(3), stage);
}

}

ShaderConstants BuildConstants(const gpu::State& s, uint32_t width, uint32_t height,
  const std::array<std::array<float, 4>, 4>& texsize) {
  if (!width || !height || width > 16384 || height > 16384 || !std::isfinite(s.zscale) || s.zscale <= 0) {
    throw std::invalid_argument("invalid menu surface dimensions or depth scale");
  }
  ShaderConstants c;
  c.vsc = s.vsc;
  c.surf = {float(width), float(height), 1 / s.zscale, 0};
  uint32_t func = s.simple[gpu::kMAlphaFunc / 4];
  if (s.simple[gpu::kMAlphaTestEnable / 4] && (func < 0x200 || func > 0x207)) {
    throw std::invalid_argument("unsupported alpha comparison " + std::to_string(func));
  }
  c.misc = {float(s.simple[gpu::kMAlphaRef / 4] & 255), s.simple[gpu::kMAlphaTestEnable / 4] ? float(func - 0x200) : 7.0f,
    0, s.rs[gpu::kRsSpecularEnable] ? 1.0f : 0.0f};
  c.tfactor = Color(s.rs[gpu::kRsTextureFactor]);
  c.texsize = texsize;
  for (int n = 0; n < 4; ++n) {
    if (c.texsize[n][0] == 0) {
      c.texsize[n][0] = 1;
    }
    if (c.texsize[n][1] == 0) {
      c.texsize[n][1] = 1;
    }
    c.tm[n] = s.transforms[16 + n];
  }
  const auto& v = s.viewport;
  const Matrix viewport{float(v.w) / width, 0, 0, 0, 0, float(v.h) / height, 0, 0, 0, 0, v.max_z - v.min_z, 0,
    (2.0f * v.x + v.w) / width - 1, 1 - (2.0f * v.y + v.h) / height, v.min_z, 1};
  c.wvp = Multiply(Multiply(Multiply(s.transforms[256], s.transforms[2]), s.transforms[3]), viewport);
  return c;
}

ShaderResult GenerateShaders(const gpu::State& s, uint16_t regs, bool pretransformed,
  const gpu::TexInfo (&textures)[4], uint64_t decode) {
  const gpu::PixelKey key = gpu::MakePixelKey(s, textures);
  const gpu::FixedKey fixed = gpu::MakeFixedKey(s);
  ShaderResult result;
  std::ostringstream description;
  description << "vs=" << s.vs_handle << " regs=" << regs << " pretransformed=" << pretransformed
    << " decode=" << decode << " ps=" << (s.ps ? s.ps->handle : 0) << " programmable=" << key.programmable
    << " lighting=" << fixed.light << " fog=" << s.rs[gpu::kRsFogEnable] << " alpha=" << key.alphatest
    << ":" << s.simple[gpu::kMAlphaFunc / 4] << ":" << s.simple[gpu::kMAlphaRef / 4];
  for (int n = 0; n < 4; ++n) {
    description << " stage" << n << "=";
    for (const auto value : key.ff[n]) {
      description << value << ",";
    }
    description << "tex:" << int(key.tex[n].kind) << ":" << int(key.tex[n].rect) << ":" << int(key.tex[n].shadow)
      << ":" << int(key.tex[n].flags) << " coord:" << fixed.tex[n];
  }
  result.state = description.str();
  const auto reject = [&](std::string diagnostic) {
    result.diagnostic = "unsupported menu shader: " + diagnostic;
    return result;
  };
  if (!(regs & 1)) {
    return reject("missing position register v0");
  }
  if (decode) {
    return reject("raw vertex decode requires canonical float4 registers");
  }
  if ((s.vs_handle & 1) && !s.vs) {
    return reject("missing owned vertex shader");
  }
  if ((s.vs_handle & 1) && s.vs && !s.vs->program.empty()) {
    return reject("programmable vertex microcode");
  }
  if (key.programmable) {
    return reject("programmable pixel combiners");
  }
  if (s.rs[gpu::kRsFogEnable] || s.rs[gpu::kRsRangeFogEnable]) {
    return reject("fog");
  }
  if (!pretransformed && (fixed.light || s.rs[gpu::kRsVertexBlend])) {
    return reject("vertex lighting or blend");
  }
  if (s.rs[gpu::kRsPointSpriteEnable] || s.rs[gpu::kRsPointScaleEnable] || s.rs[gpu::kRsWrap0]) {
    return reject("point sprite, point scale or coordinate wrap");
  }
  if (key.alphatest && (s.simple[gpu::kMAlphaFunc / 4] < 0x200 || s.simple[gpu::kMAlphaFunc / 4] > 0x207)) {
    return reject("alpha comparison " + std::to_string(s.simple[gpu::kMAlphaFunc / 4]));
  }
  for (int n = 0; n < 4; ++n) {
    const auto* f = key.ff[n];
    if (f[0] <= 1) {
      break;
    }
    for (const bool alpha : {false, true}) {
      auto error = ValidateArgs(f, alpha);
      if (!error.empty()) {
        return reject("stage " + std::to_string(n) + " " + error);
      }
    }
    if (f[8] != 1 && f[8] != 5 && f[8] != 0) {
      return reject("stage " + std::to_string(n) + " result argument " + std::to_string(f[8]));
    }
    if (textures[n].kind != gpu::kTexNone && textures[n].kind != gpu::kTex2D) {
      return reject("stage " + std::to_string(n) + " texture dimensionality");
    }
    if (textures[n].shadow || textures[n].flags) {
      return reject("stage " + std::to_string(n) + " shadow or HDR texture flags");
    }
    if (s.tss[n][gpu::kTssColorKeyOp] || s.tss[n][gpu::kTssColorSign]) {
      return reject("stage " + std::to_string(n) + " color key or signed texture");
    }
  }
  for (int n = 0; n < 4; ++n) {
    const uint32_t f = fixed.tex[n];
    if (!pretransformed && (((f >> 4) & 7) || ((f >> 8) & 7) > 4)) {
      return reject("texture coordinate generation or transform count at stage " + std::to_string(n));
    }
  }
  const auto input = [&](int reg, const char* fallback) {
    return (regs & (1u << reg)) ? "i.v" + std::to_string(reg) : std::string(fallback);
  };
  std::string vertex = kUniform;
  vertex += Varyings(key.flat);
  vertex += "struct VIn {\n";
  for (int n = 0; n < 16; ++n) {
    if (regs & (1u << n)) {
      vertex += "  @location(" + std::to_string(n) + ") v" + std::to_string(n) + ": vec4<f32>,\n";
    }
  }
  vertex += "}\n@vertex fn vs_main(i: VIn) -> VOut {\n  var o: VOut;\n";
  if (pretransformed) {
    vertex += "  let rhw = i.v0;\n  var w = 1.0;\n  if (rhw.w != 0.0) { w = 1.0 / rhw.w; }\n";
    vertex += "  var p = vec4<f32>(rhw.xy, rhw.z / u.surf.z, w);\n";
    vertex += "  let magnitude = clamp(abs(p.w), 5.42101e-20, 1.84467e19);\n  p.w = select(magnitude, -magnitude, p.w < 0.0);\n";
    vertex += "  if (abs(p.x - (u.surf.x - 0.5)) < 0.001) { p.x = u.surf.x; }\n  if (abs(p.y - (u.surf.y - 0.5)) < 0.001) { p.y = u.surf.y; }\n";
    vertex += "  o.pos = vec4<f32>((p.x * 2.0 / u.surf.x - 1.0) * p.w, (1.0 - p.y * 2.0 / u.surf.y) * p.w, p.z * u.surf.z * p.w, p.w);\n";
  } else {
    vertex += "  var cp = rowmul(vec4<f32>(i.v0.xyz, 1.0), u.wvp);\n";
    vertex += "  let sp = (vec2<f32>(cp.x, -cp.y) / cp.w + vec2<f32>(1.0)) * 0.5 * u.surf.xy;\n";
    vertex += "  if (abs(sp.x - (u.surf.x - 0.5)) < 0.001) { cp.x = cp.w; }\n  if (abs(sp.y - (u.surf.y - 0.5)) < 0.001) { cp.y = -cp.w; }\n  o.pos = cp;\n";
  }
  vertex += "  o.d0 = clamp(" + input(3, "vec4<f32>(1.0)") + ", vec4<f32>(0.0), vec4<f32>(1.0));\n";
  vertex += "  o.d1 = clamp(" + input(4, "vec4<f32>(0.0)") + ", vec4<f32>(0.0), vec4<f32>(1.0));\n";
  for (int n = 0; n < 4; ++n) {
    const auto f = fixed.tex[n];
    const auto value = input(9 + int(f & 3), "vec4<f32>(0.0, 0.0, 0.0, 1.0)");
    const auto count = (f >> 8) & 7;
    const auto size = (f >> 12) & 7;
    const auto name = std::to_string(n);
    if (pretransformed || !count) {
      vertex += "  o.t" + name + " = " + value + ";\n";
    } else {
      const auto coords = size == 1 ? "vec4<f32>(" + value + ".x, 1.0, 0.0, 0.0)"
        : size == 2 ? "vec4<f32>(" + value + ".xy, 1.0, 0.0)"
        : size == 3 ? "vec4<f32>(" + value + ".xyz, 1.0)" : value;
      vertex += "  let tc" + name + " = rowmul(" + coords + ", u.tm[" + name + "]);\n";
      const auto q = (f & 0x800) ? "tc" + name + "[" + std::to_string(count - 1) + "]" : "1.0";
      vertex += "  o.t" + name + " = vec4<f32>(tc" + name + ".xy, " + (count >= 3 || (f & 0x800) ? "tc" + name + ".z" : "0.0") + ", " + q + ");\n";
    }
  }
  vertex += "  return o;\n}\n";
  std::string fragment = kUniform;
  fragment += Varyings(key.flat);
  for (int n = 0; n < 4; ++n) {
    const auto name = std::to_string(n);
    fragment += "@group(0) @binding(" + std::to_string(1 + n * 2) + ") var tex" + name + ": texture_2d<f32>;\n";
    fragment += "@group(0) @binding(" + std::to_string(2 + n * 2) + ") var smp" + name + ": sampler;\n";
  }
  fragment += "@fragment fn fs_main(i: VOut) -> @location(0) vec4<f32> {\n  var cur = i.d0;\n  var tmp = vec4<f32>(0.0);\n  var v1 = i.d1;\n  if (u.misc.w == 0.0) { v1 = vec4<f32>(0.0, 0.0, 0.0, 1.0); }\n";
  for (int n = 0; n < 4; ++n) {
    const auto* f = key.ff[n];
    if (f[0] <= 1) {
      break;
    }
    const auto name = std::to_string(n);
    std::string uv = "i.t" + name + ".xy";
    if (key.proj & (1u << n)) {
      uv = "(" + uv + " / i.t" + name + ".w)";
    }
    if (textures[n].rect) {
      uv += " / u.texsize[" + name + "].xy";
    }
    uv += " + u.texsize[" + name + "].zw";
    fragment += "  let t" + name + " = " + (textures[n].kind == gpu::kTex2D ? "textureSample(tex" + name + ", smp" + name + ", " + uv + ")" : "vec4<f32>(1.0)") + ";\n";
    if (key.alphakill & (1u << n)) {
      fragment += "  if (t" + name + ".a == 0.0) { discard; }\n";
    }
    const auto rgb = StageExpression(f, n, false);
    const auto alpha = StageExpression(f, n, true);
    fragment += "  { let rgb = clamp(" + rgb + ", vec3<f32>(0.0), vec3<f32>(1.0)); let alpha = clamp(" + alpha + ", 0.0, 1.0); ";
    fragment += std::string(f[8] == 5 ? "tmp" : "cur") + " = vec4<f32>(rgb, alpha); }\n";
  }
  fragment += "  var outc = vec4<f32>(clamp(cur.rgb + v1.rgb, vec3<f32>(0.0), vec3<f32>(1.0)), cur.a);\n";
  if (key.alphatest) {
    fragment += "  let v = floor(outc.a * 255.0 + 0.5);\n  let r = u.misc.x;\n  let f = u32(u.misc.y);\n";
    fragment += "  let ok = (f == 1u && v < r) || (f == 2u && v == r) || (f == 3u && v <= r) || (f == 4u && v > r) || (f == 5u && v != r) || (f == 6u && v >= r) || f == 7u;\n  if (!ok) { discard; }\n";
  }
  fragment += "  return outc;\n}\n";
  result.vertex = std::move(vertex);
  result.fragment = std::move(fragment);
  result.supported = true;
  return result;
}

std::string DescribeShaderFailure(const ShaderResult& shader, std::string_view error) {
  return "menu shader compile failure: " + std::string(error) + "\nstate: " + shader.state + "\nvertex WGSL:\n" + shader.vertex + "\nfragment WGSL:\n" + shader.fragment;
}

}
