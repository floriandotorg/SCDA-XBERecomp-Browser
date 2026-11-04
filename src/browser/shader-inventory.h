#pragma once
#include "gpu/shaders.h"
#include <stdexcept>

namespace browser {
struct ShaderInventory {
  uint32_t macMask = 0, iluMask = 0, textureModes = 0, paths = 0;
  std::array<uint32_t, 8> formats{};
  void Texture(uint32_t format) {
    if (format >= 256) { throw std::out_of_range("shader inventory texture format"); }
    formats[format / 32] |= 1u << (format % 32);
  }
  void Observe(const gpu::State& state, bool pretransformed, const gpu::TexInfo* textures = nullptr) {
    const bool program = (state.vs_handle & 1) && state.vs && !state.vs->program.empty();
    paths |= pretransformed ? 1u : program ? 4u : 2u;
    if (program) {
      const auto& words = state.vs->program;
      if (words.size() % 4) { throw std::invalid_argument("shader inventory instruction span"); }
      for (size_t n = 0; n < words.size(); n += 4) {
        macMask |= 1u << ((words[n + 1] >> 21) & 15);
        iluMask |= 1u << ((words[n + 1] >> 25) & 7);
        if (words[n + 3] & 1) { break; }
      }
    }
    if (state.ps) {
      paths |= 8;
      for (uint32_t n = 0; n < 4; ++n) {
        uint32_t mode = (state.rs[gpu::kRsPsTextureModes] >> (n * 5)) & 31;
        const auto kind = textures ? textures[n].kind : state.textures[n] ? gpu::kTex2D : gpu::kTexNone;
        if (!kind && mode != 4 && mode != 5 && mode != 10 && mode != 17) { mode = 0; }
        else if (mode >= 1 && mode <= 3) { mode = kind == gpu::kTexCube ? 3 : kind == gpu::kTex3D ? 2 : 1; }
        else if (mode == 13 || mode == 14) { mode = kind == gpu::kTexCube ? 14 : 13; }
        textureModes |= 1u << mode;
      }
    }
  }
};
}
