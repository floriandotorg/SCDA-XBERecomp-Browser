#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "gpu/shaders.h"

namespace browser::menu {

struct alignas(16) ShaderConstants {
  std::array<std::array<float, 4>, 192> vsc{};
  std::array<float, 4> surf{};
  std::array<float, 4> misc{};
  std::array<float, 4> tfactor{};
  std::array<std::array<float, 4>, 4> texsize{};
  std::array<float, 16> wvp{};
  std::array<std::array<float, 16>, 4> tm{};
  std::array<std::array<float, 4>, 18> pc{};
  std::array<float, 4> lod_bias{};
  std::array<float, 4> depth_bias{};
  // HDR scene targets hold colour divided by y: x scales what a draw writes, y what a sampled HDR target reads
  std::array<float, 4> hdr{1, 8, 0, 0};
};

// HDR sampling on 2D targets and per pixel normalization cubes; kTexWide stays native only
inline bool SupportedFlags(const gpu::TexInfo& texture) {
  const bool hdr = !(texture.flags & gpu::kTexHdr) || texture.kind == gpu::kTex2D;
  const bool cube = !(texture.flags & gpu::kTexNormCube) || texture.kind == gpu::kTexCube;
  return !(texture.flags & gpu::kTexWide) && hdr && cube;
}

struct ShaderResult {
  bool supported = false;
  std::string vertex;
  std::string fragment;
  std::string diagnostic;
  std::string state;
};

ShaderConstants BuildConstants(const gpu::State& state, uint32_t width, uint32_t height,
  const std::array<std::array<float, 4>, 4>& texsize = {});
ShaderResult GenerateShaders(const gpu::State& state, uint16_t regs, bool pretransformed,
  const gpu::TexInfo (&textures)[4], uint64_t decode = 0, bool colorTarget = true, bool depthBias = false);
std::string GuestVertexInputs(std::string vertex, const gpu::VertexShader& layout, uint16_t regs);
std::string DescribeShaderFailure(const ShaderResult& shader, std::string_view error);

}
