#include "browser/guest-vertex.h"
#include "browser/menu-shaders.h"
#include "browser/menu-pixel-shaders.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <sstream>
#include <stdexcept>

namespace browser::menu {

static_assert(sizeof(ShaderConstants) == 3840);
static_assert(offsetof(ShaderConstants, pc) == 3504);
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
  pc: array<vec4<f32>, 18>,
  lod_bias: vec4<f32>,
  depth_bias: vec4<f32>,
  hdr: vec4<f32>,
}
@group(0) @binding(0) var<uniform> u: Constants;
fn rowmul(p: vec4<f32>, m: array<vec4<f32>, 4>) -> vec4<f32> {
  return p.x * m[0] + p.y * m[1] + p.z * m[2] + p.w * m[3];
}
)";

const char* kVertexHelpers = R"(fn xrcc(v: f32) -> f32 {
  let magnitude = clamp(1.0 / max(abs(v), 1.0 / 1.84467e19), 5.42101e-20, 1.84467e19);
  return select(magnitude, -magnitude, (bitcast<u32>(v) & 0x80000000u) != 0u);
}
fn xrcp(v: f32) -> f32 {
  if (v == 0.0) { return bitcast<f32>(bitcast<u32>(v) | 0x7f800000u); }
  return 1.0 / v;
}
fn xrsq(v: f32) -> f32 {
  if (v == 0.0) { return bitcast<f32>((bitcast<u32>(v) & 0x7fffffffu) | 0x7f800000u); }
  return inverseSqrt(abs(v));
}
)";

void ReplaceAll(std::string& value, const std::string& source, const std::string& replacement) {
  size_t offset = 0;
  while ((offset = value.find(source, offset)) != std::string::npos) {
    value.replace(offset, source.size(), replacement);
    offset += replacement.size();
  }
}

std::string ShadowSampling(const gpu::State& state, uint32_t stage, uint32_t kind) {
  const auto& sampling = state.tss[stage];
  const uint32_t compare = state.rs[gpu::kRsShadowFunc];
  if (compare < 0x200 || compare > 0x207 || kind < 1 || kind > 2) {
    throw std::invalid_argument("shadow comparison or format");
  }
  const std::array<std::string, 8> comparisons{"false", "depth < reference", "depth == reference", "depth <= reference",
    "depth > reference", "depth != reference", "depth >= reference", "true"};
  std::string code = R"(fn shadow_index@(p: i32, size: i32, mode: u32) -> i32 {
  if (mode == 1u) { return ((p % size) + size) % size; }
  if (mode == 2u) {
    let value = ((p % (size * 2)) + size * 2) % (size * 2);
    return min(value, size * 2 - 1 - value);
  }
  return clamp(p, 0, size - 1);
}
fn shadow_tap@(p: vec2<i32>, reference: f32) -> f32 {
  let size = vec2<i32>(textureDimensions(tex@));
  var depth = #BORDER;
  if (!((#U == 4u && (p.x < 0 || p.x >= size.x)) || (#V == 4u && (p.y < 0 || p.y >= size.y)))) {
    depth = textureLoad(tex@, vec2<i32>(shadow_index@(p.x, size.x, #U), shadow_index@(p.y, size.y, #V)), 0);
  }
  return select(0.0, 1.0, #COMPARE);
}
fn shadow_sample@(uv: vec2<f32>, z: f32) -> vec4<f32> {
  let reference = clamp(z, 0.0, #MAX) / #MAX;
  let position = uv * vec2<f32>(textureDimensions(tex@));
  if (!#LINEAR) { return vec4<f32>(shadow_tap@(vec2<i32>(floor(position)), reference)); }
  let center = position - vec2<f32>(0.5);
  let origin = vec2<i32>(floor(center));
  let weight = fract(center);
  return vec4<f32>(mix(mix(shadow_tap@(origin, reference), shadow_tap@(origin + vec2<i32>(1, 0), reference), weight.x),
    mix(shadow_tap@(origin + vec2<i32>(0, 1), reference), shadow_tap@(origin + vec2<i32>(1, 1), reference), weight.x), weight.y));
}
)";
  ReplaceAll(code, "@", std::to_string(stage));
  ReplaceAll(code, "#U", std::to_string(sampling[gpu::kTssAddressU]) + "u");
  ReplaceAll(code, "#V", std::to_string(sampling[gpu::kTssAddressV]) + "u");
  ReplaceAll(code, "#BORDER", std::to_string(float((sampling[gpu::kTssBorderColor] >> 16) & 255) / 255));
  ReplaceAll(code, "#COMPARE", comparisons[compare - 0x200]);
  ReplaceAll(code, "#MAX", kind == 2 ? "65535.0" : "16777215.0");
  ReplaceAll(code, "#LINEAR", sampling[gpu::kTssMinFilter] == 2 || sampling[gpu::kTssMagFilter] == 2 ? "true" : "false");
  return code;
}

std::string BorderSampling(const gpu::State& state, uint32_t stage) {
  const auto& sampling = state.tss[stage];
  const auto u = sampling[gpu::kTssAddressU], v = sampling[gpu::kTssAddressV];
  const auto mag = sampling[gpu::kTssMagFilter], min = sampling[gpu::kTssMinFilter], mip = sampling[gpu::kTssMipFilter];
  if (u < 1 || u > 4 || v < 1 || v > 4 || mag < 1 || mag > 2 || min < 1 || min > 2 || mip > 2) {
    throw std::invalid_argument("border sampler state");
  }
  const uint32_t color = sampling[gpu::kTssBorderColor];
  std::string code = R"(fn border_index@(p: i32, size: i32, mode: u32) -> i32 {
  if (mode == 1u) { return ((p % size) + size) % size; }
  if (mode == 2u) {
    let period = size * 2;
    let value = ((p % period) + period) % period;
    return min(value, period - 1 - value);
  }
  return clamp(p, 0, size - 1);
}
fn border_tap@(p: vec2<i32>, level: i32) -> vec4<f32> {
  let size = vec2<i32>(textureDimensions(tex@, level));
  if ((#U == 4u && (p.x < 0 || p.x >= size.x)) || (#V == 4u && (p.y < 0 || p.y >= size.y))) {
    return unpack4x8unorm(#COLOR);
  }
  return textureLoad(tex@, vec2<i32>(border_index@(p.x, size.x, #U), border_index@(p.y, size.y, #V)), level);
}
fn border_level@(uv: vec2<f32>, level: i32, linear: bool) -> vec4<f32> {
  let position = uv * vec2<f32>(textureDimensions(tex@, level));
  if (!linear) { return border_tap@(vec2<i32>(floor(position)), level); }
  let center = position - vec2<f32>(0.5);
  let origin = vec2<i32>(floor(center));
  let weight = fract(center);
  return mix(mix(border_tap@(origin, level), border_tap@(origin + vec2<i32>(1, 0), level), weight.x),
    mix(border_tap@(origin + vec2<i32>(0, 1), level), border_tap@(origin + vec2<i32>(1, 1), level), weight.x), weight.y);
}
fn border_sample@(uv: vec2<f32>, bias: f32) -> vec4<f32> {
  let position = uv * vec2<f32>(textureDimensions(tex@, 0));
  let dx = dpdx(position);
  let dy = dpdy(position);
  let lod = 0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-30)) + bias;
  let last = f32(textureNumLevels(tex@) - 1u);
  let first = min(f32(#FIRST), last);
  let level = clamp(lod, first, select(first, last, #MIP != 0u));
  let linear = select(#MIN == 2u, #MAG == 2u, lod <= 0.0);
  if (#MIP != 2u) { return border_level@(uv, i32(floor(level + 0.5)), linear); }
  return mix(border_level@(uv, i32(floor(level)), linear), border_level@(uv, i32(min(floor(level) + 1.0, last)), linear), fract(level));
}
)";
  ReplaceAll(code, "@", std::to_string(stage));
  for (const auto& [name, value] : std::array<std::pair<const char*, uint32_t>, 7>{{
      {"#U", u}, {"#V", v}, {"#MIN", min}, {"#MAG", mag}, {"#MIP", mip},
      {"#FIRST", sampling[gpu::kTssMaxMipLevel]}, {"#COLOR", ((color >> 16) & 255) | (color & 0xff00) | ((color & 255) << 16) | (color & 0xff000000)}}}) {
    ReplaceAll(code, name, std::to_string(value) + "u");
  }
  return code;
}

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

std::string Op(uint32_t op, const std::string& a, const std::string& b, const std::string& c,
  int stage, bool alpha, const std::string& aa) {
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
    case 18: return alpha ? a : a + " + " + b + " * " + aa;
    case 19: return alpha ? a : a + " * " + b + " + " + aa;
    case 20: return alpha ? a : a + " + " + b + " * (1.0 - " + aa + ")";
    case 21: return alpha ? a : "(1.0 - " + a + ") * " + b + " + " + aa;
    case 22: {
      const auto x = alpha ? "vec3<f32>(" + a + ", 0.0, 0.0)" : a;
      const auto y = alpha ? "vec3<f32>(" + b + ", 0.0, 0.0)" : b;
      const auto dot = "dot((" + x + " - 0.5) * 2.0, (" + y + " - 0.5) * 2.0)";
      return alpha ? dot : "vec3<f32>(" + dot + ")";
    }
    case 23: return c + " + " + a + " * " + b;
    case 24: return "(" + b + " * (1.0 - " + c + ") + " + a + " * " + c + ")";
    default: throw std::logic_error("unvalidated texture operation");
  }
}

bool SupportedOp(uint32_t op) {
  return op >= 2 && op <= 24 && op != 17;
}

bool UsesArgument(uint32_t op, int n, bool alpha) {
  if (n == 1) {
    return op != 3;
  }
  if (n == 2) {
    return op != 2 && !(alpha && op >= 18 && op <= 21);
  }
  return op == 23 || op == 24;
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
    if (!UsesArgument(op, n, alpha)) {
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
    if (!UsesArgument(op, n, alpha)) {
      return std::string("0.0");
    }
    return Arg(f[base + n], stage, alpha);
  };
  return Op(op, arg(1), arg(2), arg(3), stage, alpha, alpha || !UsesArgument(op, 1, false) ? "0.0" : Arg(f[1], stage, true));
}

struct VertexToken {
  const uint32_t* words;

  uint32_t Field(int word, int bit, int length) const {
    return (words[word] >> bit) & ((1u << length) - 1);
  }
};

std::string Register(uint32_t reg) {
  if (reg > 12) {
    throw std::invalid_argument("temporary register " + std::to_string(reg));
  }
  return reg == 12 ? "oPos" : "R" + std::to_string(reg);
}

std::string ProgramSource(const VertexToken& k, int source, uint16_t regs) {
  uint32_t neg = 0, reg = 0, mux = 0;
  std::array<uint32_t, 4> sw{};
  if (source == 0) {
    neg = k.Field(1, 8, 1);
    reg = k.Field(2, 28, 4);
    mux = k.Field(2, 26, 2);
    for (int n = 0; n < 4; ++n) {
      sw[n] = k.Field(1, 6 - n * 2, 2);
    }
  } else if (source == 1) {
    neg = k.Field(2, 25, 1);
    reg = k.Field(2, 13, 4);
    mux = k.Field(2, 11, 2);
    for (int n = 0; n < 4; ++n) {
      sw[n] = k.Field(2, 23 - n * 2, 2);
    }
  } else {
    neg = k.Field(2, 10, 1);
    reg = k.Field(2, 0, 2) << 2 | k.Field(3, 30, 2);
    mux = k.Field(3, 28, 2);
    for (int n = 0; n < 4; ++n) {
      sw[n] = k.Field(2, 8 - n * 2, 2);
    }
    if (k.Field(1, 25, 3) >= 2 && k.Field(1, 25, 3) <= 6) {
      sw.fill(sw[0]);
    }
  }
  std::string value;
  if (mux == 1) {
    value = Register(reg);
  } else if (mux == 2) {
    const auto input = k.Field(1, 9, 4);
    value = regs & (1u << input) ? "i.v" + std::to_string(input)
      : input == 3 ? "vec4<f32>(1.0)" : "vec4<f32>(0.0, 0.0, 0.0, 1.0)";
  } else {
    const auto index = k.Field(1, 13, 8);
    if (!k.Field(3, 1, 1) && index >= 192) {
      throw std::invalid_argument("constant register " + std::to_string(index));
    }
    value = "u.vsc[" + (k.Field(3, 1, 1) ? "clamp(a0 + " + std::to_string(index) + ", 0, 191)" : std::to_string(index)) + "]";
  }
  std::string swizzle;
  for (int n = 0; n < 4; ++n) {
    swizzle += "xyzw"[sw[n]];
  }
  return std::string(neg ? "(-(" : "((") + value + ")." + swizzle + ")";
}

std::string MacExpression(uint32_t op, const std::string& a, const std::string& b, const std::string& c) {
  switch (op) {
    case 1: case 13: return a;
    case 2: return "select(" + a + " * " + b + ", vec4<f32>(0.0), (" + a + " == vec4<f32>(0.0)) | (" + b + " == vec4<f32>(0.0)))";
    case 3: return a + " + " + c;
    case 4: return "(" + MacExpression(2, a, b, c) + " + " + c + ")";
    case 5: return "vec4<f32>(dot(" + a + ".xyz, " + b + ".xyz))";
    case 6: return "vec4<f32>(dot(vec4<f32>(" + a + ".xyz, 1.0), " + b + "))";
    case 7: return "vec4<f32>(dot(" + a + ", " + b + "))";
    case 8: return "vec4<f32>(1.0, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
    case 9: return "min(" + a + ", " + b + ")";
    case 10: return "max(" + a + ", " + b + ")";
    case 11: return "select(vec4<f32>(0.0), vec4<f32>(1.0), " + a + " < " + b + ")";
    case 12: return "select(vec4<f32>(0.0), vec4<f32>(1.0), " + a + " >= " + b + ")";
    default: throw std::logic_error("unvalidated vertex MAC operation");
  }
}

std::string WriteRegister(const std::string& destination, uint32_t mask, const std::string& value) {
  std::string out;
  for (int n = 0; n < 4; ++n) {
    if (mask & (8u >> n)) {
      const std::string component(1, "xyzw"[n]);
      out += "    " + destination + "." + component + " = " + value + "." + component + ";\n";
    }
  }
  return out;
}

struct VertexTranslation {
  std::string body;
  std::string diagnostic;
};

VertexTranslation TranslateVertex(const gpu::VertexShader& shader, uint16_t regs) {
  VertexTranslation out;
  try {
    if (shader.program_type != 0x2078) {
      throw std::invalid_argument("program type " + std::to_string(shader.program_type));
    }
    if (shader.program.size() % 4 || shader.program.size() > 1024) {
      throw std::invalid_argument("instruction length " + std::to_string(shader.program.size()));
    }
    out.body = "  var a0 = 0;\n  var oPos = vec4<f32>(0.0);\n  var oD0 = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n  var oD1 = vec4<f32>(0.0);\n";
    for (int n = 0; n < 12; ++n) {
      out.body += "  var R" + std::to_string(n) + " = vec4<f32>(0.0);\n";
    }
    for (int n = 0; n < 4; ++n) {
      out.body += "  var oT" + std::to_string(n) + " = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n";
    }
    bool ended = false;
    for (size_t n = 0; n < shader.program.size(); n += 4) {
      const VertexToken k{shader.program.data() + n};
      const auto mac = k.Field(1, 21, 4);
      const auto ilu = k.Field(1, 25, 3);
      if (mac > 13) {
        throw std::invalid_argument("MAC operation " + std::to_string(mac));
      }
      if (ilu > 4) {
        throw std::invalid_argument("ILU operation " + std::to_string(ilu));
      }
      const auto source = [&](int which) { return ProgramSource(k, which, regs); };
      out.body += "  {\n";
      if (mac) {
        const auto a = source(0);
        const auto b = mac == 2 || (mac >= 4 && mac <= 12) ? source(1) : "vec4<f32>(0.0)";
        const auto c = mac == 3 || mac == 4 ? source(2) : "vec4<f32>(0.0)";
        out.body += "    let mr = " + MacExpression(mac, a, b, c) + ";\n";
      }
      if (ilu) {
        const auto c = source(2);
        out.body += "    let ir = " + (ilu >= 2 ? "vec4<f32>(" + std::string(ilu == 2 ? "xrcp(" : ilu == 3 ? "xrcc(" : "xrsq(") + c + ".x))" : c) + ";\n";
      }
      const auto reg = k.Field(3, 20, 4);
      if (mac == 13) {
        out.body += "    a0 = i32(floor(mr.x + 0.001));\n";
      } else if (mac) {
        const auto mask = ilu && reg == 1 ? 0 : k.Field(3, 24, 4);
        if (mask) {
          out.body += WriteRegister(Register(reg), mask, "mr");
        }
      }
      if (ilu && k.Field(3, 16, 4)) {
        out.body += WriteRegister(Register(mac ? 1 : reg), k.Field(3, 16, 4), "ir");
      }
      const auto mask = k.Field(3, 12, 4);
      const bool use_ilu = k.Field(3, 2, 1);
      if (mask && ((use_ilu && ilu) || (!use_ilu && mac && mac != 13))) {
        if (!k.Field(3, 11, 1)) {
          throw std::invalid_argument("constant output write");
        }
        const auto output = k.Field(3, 3, 8);
        std::string destination;
        if (output == 0) {
          destination = "oPos";
        } else if (output == 3 || output == 4) {
          destination = "oD" + std::to_string(output - 3);
        } else if (output >= 9 && output <= 12) {
          destination = "oT" + std::to_string(output - 9);
        } else {
          throw std::invalid_argument("output register " + std::to_string(output));
        }
        out.body += WriteRegister(destination, mask, use_ilu ? "ir" : "mr");
      }
      out.body += "  }\n";
      if (k.Field(3, 0, 1)) {
        ended = true;
        break;
      }
    }
    if (!ended) {
      throw std::invalid_argument("missing end token");
    }
  } catch (const std::invalid_argument& error) {
    out.body.clear();
    out.diagnostic = "programmable vertex " + std::string(error.what());
  }
  return out;
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
  for (int n = 0; n < 8; ++n) {
    c.pc[n * 2] = Color(s.simple[(gpu::kMPsConstant0 + n * 4) / 4]);
    c.pc[n * 2 + 1] = Color(s.simple[(gpu::kMPsConstant1 + n * 4) / 4]);
  }
  c.pc[16] = Color(s.simple[gpu::kMPsFinalConstant0 / 4]);
  c.pc[17] = Color(s.simple[gpu::kMPsFinalConstant1 / 4]);
  c.texsize = texsize;
  for (int n = 0; n < 4; ++n) {
    if (c.texsize[n][0] == 0) {
      c.texsize[n][0] = 1;
    }
    if (c.texsize[n][1] == 0) {
      c.texsize[n][1] = 1;
    }
    c.tm[n] = s.transforms[16 + n];
    c.lod_bias[n] = std::bit_cast<float>(s.tss[n][gpu::kTssMipLodBias]);
  }
  const auto& v = s.viewport;
  const Matrix viewport{float(v.w) / width, 0, 0, 0, 0, float(v.h) / height, 0, 0, 0, 0, v.max_z - v.min_z, 0,
    (2.0f * v.x + v.w) / width - 1, 1 - (2.0f * v.y + v.h) / height, v.min_z, 1};
  c.wvp = Multiply(Multiply(Multiply(s.transforms[256], s.transforms[2]), s.transforms[3]), viewport);
  return c;
}

ShaderResult GenerateShaders(const gpu::State& s, uint16_t regs, bool pretransformed,
  const gpu::TexInfo (&textures)[4], uint64_t decode, bool colorTarget, bool depthBias) {
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
  const bool programmable = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
  VertexTranslation program;
  if (programmable) {
    program = TranslateVertex(*s.vs, regs);
    if (!program.diagnostic.empty()) {
      return reject(program.diagnostic);
    }
  }
  PixelTranslation pixel;
  if (key.programmable) {
    pixel = TranslatePixel(key);
    if (!pixel.supported) {
      return reject("programmable pixel " + pixel.diagnostic);
    }
  }
  if (s.rs[gpu::kRsFogEnable] || s.rs[gpu::kRsRangeFogEnable]) {
    return reject("fog");
  }
  if (!programmable && !pretransformed && (fixed.light || s.rs[gpu::kRsVertexBlend])) {
    return reject("vertex lighting or blend");
  }
  if (s.rs[gpu::kRsPointSpriteEnable] || s.rs[gpu::kRsPointScaleEnable] || s.rs[gpu::kRsWrap0]) {
    return reject("point sprite, point scale or coordinate wrap");
  }
  if (key.alphatest && (s.simple[gpu::kMAlphaFunc / 4] < 0x200 || s.simple[gpu::kMAlphaFunc / 4] > 0x207)) {
    return reject("alpha comparison " + std::to_string(s.simple[gpu::kMAlphaFunc / 4]));
  }
  for (int n = 0; n < 4 && !key.programmable; ++n) {
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
    if (textures[n].kind != gpu::kTexNone && textures[n].kind != gpu::kTex2D && textures[n].kind != gpu::kTexCube && textures[n].kind != gpu::kTex3D) {
      return reject("stage " + std::to_string(n) + " texture dimensionality");
    }
    if (textures[n].shadow > 2 || !SupportedFlags(textures[n])) {
      return reject("stage " + std::to_string(n) + " shadow or HDR texture flags");
    }
    if (s.tss[n][gpu::kTssColorKeyOp] || s.tss[n][gpu::kTssColorSign]) {
      return reject("stage " + std::to_string(n) + " color key or signed texture");
    }
  }
  for (int n = 0; n < 4; ++n) {
    const uint32_t f = fixed.tex[n];
    if (!programmable && !pretransformed && (((f >> 4) & 7) || ((f >> 8) & 7) > 4)) {
      return reject("texture coordinate generation or transform count at stage " + std::to_string(n));
    }
  }
  const auto input = [&](int reg, const char* fallback) {
    return (regs & (1u << reg)) ? "i.v" + std::to_string(reg) : std::string(fallback);
  };
  std::string vertex = kUniform;
  if (programmable) {
    vertex += kVertexHelpers;
  }
  vertex += Varyings(key.flat);
  vertex += "struct VIn {\n";
  for (int n = 0; n < 16; ++n) {
    if (regs & (1u << n)) {
      vertex += "  @location(" + std::to_string(n) + ") v" + std::to_string(n) + ": vec4<f32>,\n";
    }
  }
  vertex += "}\n@vertex fn vs_main(i: VIn) -> VOut {\n  var o: VOut;\n";
  if (programmable || pretransformed) {
    if (programmable) {
      vertex += program.body + "  var p = oPos;\n";
    } else {
      vertex += "  let rhw = i.v0;\n  var w = 1.0;\n  if (rhw.w != 0.0) { w = 1.0 / rhw.w; }\n";
      vertex += "  var p = vec4<f32>(rhw.xy, rhw.z / u.surf.z, w);\n";
    }
    vertex += "  let magnitude = clamp(abs(p.w), 5.42101e-20, 1.84467e19);\n  p.w = select(magnitude, -magnitude, p.w < 0.0);\n";
    vertex += "  if (abs(p.x - (u.surf.x - 0.5)) < 0.001) { p.x = u.surf.x; }\n  if (abs(p.y - (u.surf.y - 0.5)) < 0.001) { p.y = u.surf.y; }\n";
    vertex += "  o.pos = vec4<f32>((p.x * 2.0 / u.surf.x - 1.0) * p.w, (1.0 - p.y * 2.0 / u.surf.y) * p.w, p.z * u.surf.z * p.w, p.w);\n";
  } else {
    vertex += "  var cp = rowmul(vec4<f32>(i.v0.xyz, 1.0), u.wvp);\n";
    vertex += "  let sp = (vec2<f32>(cp.x, -cp.y) / cp.w + vec2<f32>(1.0)) * 0.5 * u.surf.xy;\n";
    vertex += "  if (abs(sp.x - (u.surf.x - 0.5)) < 0.001) { cp.x = cp.w; }\n  if (abs(sp.y - (u.surf.y - 0.5)) < 0.001) { cp.y = -cp.w; }\n  o.pos = cp;\n";
  }
  vertex += "  o.d0 = clamp(" + (programmable ? "oD0" : input(3, "vec4<f32>(1.0)")) + ", vec4<f32>(0.0), vec4<f32>(1.0));\n";
  vertex += "  o.d1 = clamp(" + (programmable ? "oD1" : input(4, "vec4<f32>(0.0)")) + ", vec4<f32>(0.0), vec4<f32>(1.0));\n";
  for (int n = 0; n < 4; ++n) {
    if (programmable) {
      vertex += "  o.t" + std::to_string(n) + " = oT" + std::to_string(n) + ";\n";
      continue;
    }
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
    fragment += "@group(0) @binding(" + std::to_string(1 + n * 2) + ") var tex" + name + (textures[n].shadow ? ": texture_depth_2d;\n" : textures[n].kind == gpu::kTexCube ? ": texture_cube<f32>;\n" : textures[n].kind == gpu::kTex3D ? ": texture_3d<f32>;\n" : ": texture_2d<f32>;\n");
    fragment += "@group(0) @binding(" + std::to_string(2 + n * 2) + ") var smp" + name + ": sampler;\n";
  }
  for (uint32_t n = 0; n < 4; ++n) {
    if (textures[n].shadow) { fragment += ShadowSampling(s, n, textures[n].shadow); }
    else if (textures[n].kind == gpu::kTex2D && (s.tss[n][gpu::kTssAddressU] == 4 || s.tss[n][gpu::kTssAddressV] == 4)) {
      fragment += BorderSampling(s, n);
    }
  }
  if (depthBias) {
    fragment += "struct FragmentOutput {\n";
    if (colorTarget) { fragment += "  @location(0) color: vec4<f32>,\n"; }
    fragment += "  @builtin(frag_depth) depth: f32,\n}\n@fragment fn fs_main(i: VOut) -> FragmentOutput {\n";
    fragment += "  let biased_depth = clamp(i.pos.z + u.depth_bias.x + max(abs(dpdx(i.pos.z)), abs(dpdy(i.pos.z))) * u.depth_bias.y, 0.0, 1.0);\n";
  } else {
    fragment += colorTarget ? "@fragment fn fs_main(i: VOut) -> @location(0) vec4<f32> {\n" : "@fragment fn fs_main(i: VOut) {\n";
  }
  if (key.programmable) {
    fragment += pixel.body;
  } else {
    fragment += "  var cur = i.d0;\n  var tmp = vec4<f32>(0.0);\n  var v1 = i.d1;\n  if (u.misc.w == 0.0) { v1 = vec4<f32>(0.0, 0.0, 0.0, 1.0); }\n";
  }
  for (int n = 0; n < 4 && !key.programmable; ++n) {
    const auto* f = key.ff[n];
    if (f[0] <= 1) {
      break;
    }
    const auto name = std::to_string(n);
    std::string uv = "i.t" + name + (textures[n].kind == gpu::kTexCube || textures[n].kind == gpu::kTex3D ? ".xyz" : ".xy");
    if (key.proj & (1u << n)) {
      uv = "(" + uv + " / i.t" + name + ".w)";
    }
    if (textures[n].rect) {
      uv += " / u.texsize[" + name + "].xy";
    }
    if (textures[n].kind == gpu::kTex2D) { uv += " + u.texsize[" + name + "].zw"; }
    if (textures[n].shadow) {
      auto coordinate = "(i.t" + name + ".xy / i.t" + name + ".w)";
      if (textures[n].rect) { coordinate += " / u.texsize[" + name + "].xy"; }
      fragment += "  let t" + name + " = shadow_sample" + name + "(" + coordinate + " + u.texsize[" + name + "].zw, i.t" + name + ".z / i.t" + name + ".w);\n";
    } else {
      fragment += "  let t" + name + " = " + (textures[n].kind != gpu::kTexNone ? "textureSampleBias(tex" + name + ", smp" + name + ", " + uv + ", u.lod_bias[" + name + "])" : "vec4<f32>(1.0)") + ";\n";
    }
    if (key.alphakill & (1u << n)) {
      fragment += "  if (t" + name + ".a == 0.0) { discard; }\n";
    }
    const auto rgb = StageExpression(f, n, false);
    const auto alpha = StageExpression(f, n, true);
    fragment += "  { let rgb = clamp(" + rgb + ", vec3<f32>(0.0), vec3<f32>(1.0)); let alpha = clamp(" + alpha + ", 0.0, 1.0); ";
    fragment += std::string(f[8] == 5 ? "tmp" : "cur") + " = vec4<f32>(rgb, alpha); }\n";
  }
  if (!key.programmable) {
    fragment += "  var outc = vec4<f32>(clamp(cur.rgb + v1.rgb, vec3<f32>(0.0), vec3<f32>(1.0)) * u.hdr.x, cur.a);\n";
  }
  if (key.alphatest) {
    fragment += "  let v = floor(outc.a * 255.0 + 0.5);\n  let r = u.misc.x;\n  let f = u32(u.misc.y);\n";
    fragment += "  let ok = (f == 1u && v < r) || (f == 2u && v == r) || (f == 3u && v <= r) || (f == 4u && v > r) || (f == 5u && v != r) || (f == 6u && v >= r) || f == 7u;\n  if (!ok) { discard; }\n";
  }
  if (depthBias) {
    fragment += "  var result: FragmentOutput;\n";
    if (colorTarget) { fragment += "  result.color = outc;\n"; }
    fragment += "  result.depth = biased_depth;\n  return result;\n";
  } else if (colorTarget) { fragment += "  return outc;\n"; }
  fragment += "}\n";
  for (uint32_t n = 0; n < 4; ++n) {
    const auto name = std::to_string(n);
    const auto sample = "textureSampleBias(tex" + name + ", smp" + name + ",";
    if (textures[n].flags & gpu::kTexHdr) {
      ReplaceAll(fragment, sample, "hdr_sample" + name + "(");
      fragment += "fn hdr_sample" + name + "(uv: vec2<f32>, b: f32) -> vec4<f32> {\n  let t = " + sample + " uv, b);\n" +
        "  return vec4<f32>(clamp(t.rgb * u.hdr.y, vec3<f32>(0.0), vec3<f32>(1.0)), t.a);\n}\n";
    }
    if (textures[n].flags & gpu::kTexNormCube) {
      ReplaceAll(fragment, sample, "norm_cube" + name + "(");
      // a zero direction has no normal: the cube's own texel, where normalize would give NaN
      fragment += "fn norm_cube" + name + "(d: vec3<f32>, b: f32) -> vec4<f32> {\n  let t = " + sample + " d, b);\n" +
        "  return select(t, vec4<f32>(normalize(d) * 0.5 + vec3<f32>(0.5), t.a), dot(d, d) > 1e-30);\n}\n";
    }
  }
  for (uint32_t n = 0; n < 4; ++n) {
    if (textures[n].kind == gpu::kTex2D && (s.tss[n][gpu::kTssAddressU] == 4 || s.tss[n][gpu::kTssAddressV] == 4)) {
      const auto name = std::to_string(n);
      ReplaceAll(fragment, "textureSampleBias(tex" + name + ", smp" + name + ",", "border_sample" + name + "(");
    }
  }
  result.vertex = std::move(vertex);
  result.fragment = std::move(fragment);
  result.supported = true;
  return result;
}

std::string DescribeShaderFailure(const ShaderResult& shader, std::string_view error) {
  return "menu shader compile failure: " + std::string(error) + "\nstate: " + shader.state + "\nvertex WGSL:\n" + shader.vertex + "\nfragment WGSL:\n" + shader.fragment;
}

std::string GuestVertexInputs(std::string vertex, const gpu::VertexShader& layout, uint16_t regs) {
  std::string declaration = R"(
fn guest_unorm(value: u32, bit_count: u32) -> f32 {
  let divisor = (1u << bit_count) - 1u;
  if (value == 0u) { return 0.0; }
  if (value >= divisor) { return 1.0; }
  let lead = firstLeadingBit(value);
  let numerator = value << (31u - lead);
  var quotient = numerator / divisor;
  var remainder = numerator % divisor;
  let extra = bit_count - 8u;
  let tail = remainder << extra;
  quotient = (quotient << extra) + tail / divisor;
  remainder = tail % divisor;
  if (remainder * 2u > divisor) { quotient += 1u; }
  let exponent = u32(i32(lead) - i32(bit_count) + 127);
  return bitcast<f32>((exponent << 23u) | (quotient & 0x7fffffu));
}
fn guest_snorm(value: i32, bit_count: u32) -> f32 {
  let bits = bitcast<u32>(guest_unorm(u32(abs(value)), bit_count));
  return bitcast<f32>(bits | select(0u, 0x80000000u, value < 0));
}
struct RawIn {
)", values;
  uint32_t locations = regs;
  for (uint32_t n = 0; n < 16; ++n) {
    if (!(regs & (1u << n))) { continue; }
    const auto format = menu::GuestVertex(layout.inputs[n].type);
    if (!format.format) { throw std::runtime_error("unsupported guest vertex input"); }
    const auto name = "r" + std::to_string(n);
    declaration += "@location(" + std::to_string(n) + ") " + name + ": " + format.input + ",\n";
    auto decode = format.decode;
    if (format.extraFormat) {
      uint32_t location = 0;
      while (location < 16 && (locations & (1u << location))) { ++location; }
      if (location == 16) { throw std::runtime_error("guest vertex input location budget exceeded"); }
      locations |= 1u << location;
      declaration += "@location(" + std::to_string(location) + ") e" + name + ": " + format.extraInput + ",\n";
      decode.replace(decode.find('$'), 1, "raw.e" + name);
    }
    for (size_t at = 0; (at = decode.find('v', at)) != std::string::npos;) {
      if (at + 1 < decode.size() && decode[at + 1] == 'e') { ++at; continue; }
      decode.replace(at, 1, "raw." + name); at += name.size() + 4;
    }
    if (!values.empty()) { values += ", "; }
    values += decode;
  }
  declaration += "}\n@vertex fn vs_main(raw: RawIn) -> VOut {\n  let i = VIn(" + values + ");";
  const std::string original = "@vertex fn vs_main(i: VIn) -> VOut {";
  const auto at = vertex.find(original);
  if (at == std::string::npos) { throw std::runtime_error("missing raw vertex entry point"); }
  vertex.replace(at, original.size(), declaration);
  return vertex;
}
}
