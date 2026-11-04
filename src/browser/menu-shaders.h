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
};

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
std::string DescribeShaderFailure(const ShaderResult& shader, std::string_view error);

}
