#pragma once
#include <array>
#include <cstdint>
#include <string>

namespace browser::menu {
struct GuestVertexFormat {
  uint32_t format = 0, bytes = 0;
  std::string input, decode;
  uint32_t extraFormat = 0, extraOffset = 0;
  std::string extraInput;
};
inline GuestVertexFormat GuestVertex(uint32_t type) {
  if (type == 0x16) {
    return {32, 4, "u32", "vec4<f32>(guest_snorm(bitcast<i32>(v << 21u) >> 21, 10u), guest_snorm(bitcast<i32>(v << 10u) >> 21, 10u), guest_snorm(bitcast<i32>(v) >> 22, 9u), 1.0)"};
  }
  if (type == 0x40) { return {3, 4, "vec4<u32>", "vec4<f32>(guest_unorm(v.z, 8u), guest_unorm(v.y, 8u), guest_unorm(v.x, 8u), guest_unorm(v.w, 8u))"}; }
  if (type == 0x72) { return {30, 12, "vec3<f32>", "vec4<f32>(v.xy, 0.0, v.z)"}; }
  const uint32_t count = type >> 4, kind = type & 15;
  if (!count || count > 4) { return {}; }
  const auto vector = [count](const char* scalar) { return count == 1 ? std::string(scalar) : "vec" + std::to_string(count) + "<" + scalar + ">"; };
  const auto expand = [count](std::string value) {
    return count == 4 ? value : "vec4<f32>(" + value + (count == 1 ? ", 0.0, 0.0, 1.0)" : count == 2 ? ", 0.0, 1.0)" : ", 1.0)");
  };
  const auto normalize = [count](const char* function, uint32_t bits) {
    std::string value;
    for (uint32_t n = 0; n < count; ++n) {
      if (n) { value += ", "; }
      value += std::string(function) + "(" + (count == 1 ? std::string("v") : "v." + std::string(1, "xyzw"[n])) + ", " + std::to_string(bits) + "u)";
    }
    return count == 1 ? value : "vec" + std::to_string(count) + "<f32>(" + value + ")";
  };
  if (kind == 2) { return {27 + count, count * 4, vector("f32"), expand("v")}; }
  if (count == 3 && (kind == 1 || kind == 5 || kind == 4)) {
    if (kind == 4) { return {2, 3, "vec2<u32>", "vec4<f32>(guest_unorm(v.x, 8u), guest_unorm(v.y, 8u), guest_unorm($, 8u), 1.0)", 1, 2, "u32"}; }
    const std::string value = "vec3<f32>(vec3<i32>(v, $))";
    return {17, 6, "vec2<i32>", "vec4<f32>(" + (kind == 5 ? value : std::string("vec3<f32>(guest_snorm(v.x, 15u), guest_snorm(v.y, 15u), guest_snorm($, 15u))")) + ", 1.0)", 16, 4, "i32"};
  }
  if (kind == 1 || kind == 5) {
    const auto value = count == 1 ? "f32(v)" : "vec" + std::to_string(count) + "<f32>(v)";
    const auto decoded = kind == 5 ? value : normalize("guest_snorm", 15);
    return {count == 1 ? 16u : count == 2 ? 17u : 18u, count * 2, vector("i32"), expand(decoded)};
  }
  if (kind == 4 && count != 3) {
    return {count == 1 ? 1u : count == 2 ? 2u : 3u, count, vector("u32"), expand(normalize("guest_unorm", 8))};
  }
  return {};
}
// the per draw path only needs the formats and sizes: decoded once per D3DVSDT type instead of building WGSL each call
inline const GuestVertexFormat& GuestVertexLayout(uint32_t type) {
  static const auto table = [] {
    std::array<GuestVertexFormat, 256> formats;
    for (uint32_t n = 0; n < formats.size(); ++n) { formats[n] = GuestVertex(n); }
    return formats;
  }();
  static const GuestVertexFormat unsupported;
  return type < table.size() ? table[type] : unsupported;
}
}
