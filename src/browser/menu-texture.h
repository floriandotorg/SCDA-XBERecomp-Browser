#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gpu/texture.h"

namespace browser::menu {

struct TextureLayout {
  bool supported = false;
  std::string diagnostic;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint64_t offset = 0;
  uint64_t bytes = 0;
  uint64_t total_bytes = 0;
};

TextureLayout DescribeTextureLayout(const gpu::TextureDesc& desc, uint32_t level = 0, uint32_t face = 0);

struct TextureResult {
  bool supported = false;
  std::string diagnostic;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 0;
  uint32_t row_pitch = 0;
  std::string format = "rgba8unorm";
  std::vector<uint8_t> pixels;
};

struct SurfaceEncodeResult {
  bool supported = false;
  std::string diagnostic;
  std::vector<uint8_t> bytes;
};
SurfaceEncodeResult EncodeSurface(const gpu::TextureDesc& desc, std::span<const uint8_t> original, std::span<const uint8_t> rgba);

TextureResult DecodeTexture(const gpu::TextureDesc& desc, std::span<const uint8_t> resource,
  std::span<const uint32_t> palette = {}, uint32_t level = 0, uint32_t face = 0);

}
