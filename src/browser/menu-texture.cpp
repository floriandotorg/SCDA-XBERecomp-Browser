#include "browser/menu-texture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace browser::menu {
namespace {

struct Format {
  uint32_t bytes = 0;
  uint32_t color = 0;
  bool linear = false;
  bool compressed = false;
};

Format GetFormat(uint32_t fmt) {
  switch (fmt) {
    case 0x00: return {1, 0x00};
    case 0x01: return {1, 0x01};
    case 0x02: case 0x03: case 0x04: case 0x05: return {2, fmt};
    case 0x06: case 0x07: return {4, fmt};
    case 0x0b: return {1, fmt};
    case 0x0c: return {8, fmt, false, true};
    case 0x0e: case 0x0f: return {16, fmt, false, true};
    case 0x10: return {2, 0x02, true};
    case 0x11: return {2, 0x05, true};
    case 0x12: return {4, 0x06, true};
    case 0x13: return {1, 0x00, true};
    case 0x16: return {2, 0x29, true};
    case 0x17: return {2, 0x28, true};
    case 0x19: return {1, fmt};
    case 0x1a: return {2, fmt};
    case 0x1b: return {1, 0x01, true};
    case 0x1c: return {2, 0x03, true};
    case 0x1d: return {2, 0x04, true};
    case 0x1e: return {4, 0x07, true};
    case 0x1f: return {1, 0x19, true};
    case 0x20: return {2, 0x1a, true};
    case 0x27: case 0x28: case 0x29: case 0x32: return {2, fmt};
    case 0x33: return {4, fmt};
    case 0x35: return {2, 0x32, true};
    case 0x36: return {4, 0x33, true};
    case 0x37: return {2, 0x27, true};
    case 0x38: case 0x39: return {2, fmt};
    case 0x3a: case 0x3b: case 0x3c: return {4, fmt};
    case 0x3d: return {2, 0x38, true};
    case 0x3e: return {2, 0x39, true};
    case 0x3f: return {4, 0x3a, true};
    case 0x40: return {4, 0x3b, true};
    case 0x41: return {4, 0x3c, true};
    default: return {};
  }
}

uint8_t Expand5(uint32_t v) {
  return uint8_t((v << 3) | (v >> 2));
}

uint8_t Expand6(uint32_t v) {
  return uint8_t((v << 2) | (v >> 4));
}

using Pixel = std::array<uint8_t, 4>;

Pixel Rgb565(uint32_t v) {
  return {Expand5(v >> 11), Expand6((v >> 5) & 63), Expand5(v & 31), 255};
}

Pixel Texel(uint32_t fmt, std::span<const uint8_t> p, std::span<const uint32_t> palette) {
  const uint32_t v = p[0] | (p.size() >= 2 ? uint32_t(p[1]) << 8 : 0);
  switch (fmt) {
    case 0x00: return {p[0], p[0], p[0], 255};
    case 0x01: return {p[0], p[0], p[0], p[0]};
    case 0x02: case 0x03: return {Expand5((v >> 10) & 31), Expand5((v >> 5) & 31), Expand5(v & 31), uint8_t(fmt == 0x03 || (v & 0x8000) ? 255 : 0)};
    case 0x04: return {uint8_t((v >> 8 & 15) * 17), uint8_t((v >> 4 & 15) * 17), uint8_t((v & 15) * 17), uint8_t((v >> 12) * 17)};
    case 0x05: return Rgb565(v);
    case 0x06: return {p[2], p[1], p[0], p[3]};
    case 0x07: return {p[2], p[1], p[0], 255};
    case 0x0b: {
      const uint32_t c = palette[p[0]];
      return {uint8_t(c >> 16), uint8_t(c >> 8), uint8_t(c), uint8_t(c >> 24)};
    }
    case 0x19: return {255, 255, 255, p[0]};
    case 0x1a: return {p[0], p[0], p[0], p[1]};
    case 0x27: return {Expand5(v & 31), Expand5((v >> 5) & 31), Expand6(v >> 10), 255};
    case 0x28: return {p[0], p[1], p[0], p[1]};
    case 0x29: return {p[1], p[0], p[0], p[1]};
    case 0x32: return {p[1], p[1], p[1], 255};
    case 0x33: return {p[1], p[3], 0, 255};
    case 0x38: return {Expand5(v >> 11), Expand5((v >> 6) & 31), Expand5((v >> 1) & 31), uint8_t(v & 1 ? 255 : 0)};
    case 0x39: return {uint8_t((v >> 12) * 17), uint8_t((v >> 8 & 15) * 17), uint8_t((v >> 4 & 15) * 17), uint8_t((v & 15) * 17)};
    case 0x3a: return {p[0], p[1], p[2], p[3]};
    case 0x3b: return {p[1], p[2], p[3], p[0]};
    case 0x3c: return {p[3], p[2], p[1], p[0]};
    default: throw std::logic_error("unvalidated menu texture color format");
  }
}

uint64_t ReadLittle(std::span<const uint8_t> p) {
  uint64_t value = 0;
  for (size_t n = 0; n < p.size(); ++n) {
    value |= uint64_t(p[n]) << (8 * n);
  }
  return value;
}

std::array<Pixel, 16> Block(uint32_t fmt, std::span<const uint8_t> p) {
  const uint32_t color_offset = fmt == 0x0c ? 0 : 8;
  const auto c = p.subspan(color_offset, 8);
  const uint32_t c0 = uint32_t(ReadLittle(c.first(2)));
  const uint32_t c1 = uint32_t(ReadLittle(c.subspan(2, 2)));
  std::array<Pixel, 4> colors{Rgb565(c0), Rgb565(c1), Pixel{}, Pixel{}};
  const bool four = c0 > c1 || fmt != 0x0c;
  for (int n = 0; n < 3; ++n) {
    colors[2][n] = uint8_t(four ? (2 * colors[0][n] + colors[1][n]) / 3 : (colors[0][n] + colors[1][n]) / 2);
    colors[3][n] = uint8_t(four ? (colors[0][n] + 2 * colors[1][n]) / 3 : 0);
  }
  colors[2][3] = 255;
  colors[3][3] = four ? 255 : 0;
  std::array<uint8_t, 8> alpha{};
  uint64_t alpha_indices = 0;
  if (fmt == 0x0f) {
    alpha[0] = p[0];
    alpha[1] = p[1];
    const int denominator = p[0] > p[1] ? 7 : 5;
    for (int n = 1; n < denominator; ++n) {
      alpha[n + 1] = uint8_t(((denominator - n) * p[0] + n * p[1]) / denominator);
    }
    if (denominator == 5) {
      alpha[6] = 0;
      alpha[7] = 255;
    }
    alpha_indices = ReadLittle(p.subspan(2, 6));
  }
  const uint32_t indices = uint32_t(ReadLittle(c.subspan(4, 4)));
  const uint64_t explicit_alpha = fmt == 0x0e ? ReadLittle(p.first(8)) : 0;
  std::array<Pixel, 16> out{};
  for (int n = 0; n < 16; ++n) {
    out[n] = colors[(indices >> (2 * n)) & 3];
    if (fmt == 0x0e) {
      out[n][3] = uint8_t(((explicit_alpha >> (4 * n)) & 15) * 17);
    }
    if (fmt == 0x0f) {
      out[n][3] = alpha[(alpha_indices >> (3 * n)) & 7];
    }
  }
  return out;
}

uint32_t Morton(uint32_t x, uint32_t y, uint32_t z, uint32_t width, uint32_t height, uint32_t depth) {
  uint32_t offset = 0;
  uint32_t outbit = 1;
  for (uint32_t n = 1; n < std::max({width, height, depth}); n <<= 1) {
    if (n < width) {
      if (x & n) {
        offset |= outbit;
      }
      outbit <<= 1;
    }
    if (n < height) {
      if (y & n) {
        offset |= outbit;
      }
      outbit <<= 1;
    }
    if (n < depth) {
      if (z & n) {
        offset |= outbit;
      }
      outbit <<= 1;
    }
  }
  return offset;
}

uint64_t LevelBytes(const gpu::TextureDesc& d, Format f, uint32_t level) {
  if (d.linear) {
    return uint64_t(d.pitch) * d.height;
  }
  const uint64_t w = std::max(d.width >> level, 1u);
  const uint64_t h = std::max(d.height >> level, 1u);
  const uint64_t z = std::max(d.depth >> level, 1u);
  return f.compressed ? ((w + 3) / 4) * ((h + 3) / 4) * z * f.bytes : w * h * z * f.bytes;
}

}

TextureLayout DescribeTextureLayout(const gpu::TextureDesc& d, uint32_t level, uint32_t face) {
  TextureLayout out;
  const auto reject = [&](std::string error) {
    out.diagnostic = "unsupported menu texture format=" + std::to_string(d.format) + " level=" + std::to_string(level)
      + " face=" + std::to_string(face) + ": " + error;
    return out;
  };
  const auto f = GetFormat(d.format);
  if (!f.bytes) {
    return reject("format is not a supported color format");
  }
  if (!d.width || !d.height || !d.depth || d.width > 4096 || d.height > 4096 || d.depth > 4096
    || uint64_t(d.width) * d.height * d.depth > 16 * 1024 * 1024) {
    return reject("dimensions exceed bounded decode capacity");
  }
  if (!d.levels || d.levels > 13 || level >= d.levels || (d.cube ? face >= 6 : face != 0)) {
    return reject("invalid mip level or cube face");
  }
  if (d.cube && (d.depth != 1 || d.width != d.height)) {
    return reject("invalid cube dimensions");
  }
  if (d.linear != f.linear) {
    return reject("format and linear storage disagree");
  }
  if (d.linear && (d.levels != 1 || d.depth != 1 || d.pitch < uint64_t(d.width) * f.bytes)) {
    return reject("invalid linear pitch, mip count or depth");
  }
  if (!d.linear && !f.compressed && (!std::has_single_bit(d.width) || !std::has_single_bit(d.height) || !std::has_single_bit(d.depth))) {
    return reject("swizzled dimensions must be powers of two");
  }
  uint64_t face_bytes = 0;
  for (uint32_t n = 0; n < d.levels; ++n) {
    const auto bytes = LevelBytes(d, f, n);
    if (n < level) {
      out.offset += bytes;
    }
    face_bytes += bytes;
  }
  const auto face_stride = (face_bytes + 127) & ~uint64_t(127);
  out.offset += d.cube ? face * face_stride : 0;
  out.bytes = LevelBytes(d, f, level);
  out.total_bytes = d.cube ? face_stride * 6 : face_bytes;
  if (out.total_bytes > 256 * 1024 * 1024) {
    return reject("resource exceeds bounded input capacity");
  }
  out.width = d.linear ? d.width : std::max(d.width >> level, 1u);
  out.height = d.linear ? d.height : std::max(d.height >> level, 1u);
  out.depth = d.linear ? 1 : std::max(d.depth >> level, 1u);
  out.supported = true;
  return out;
}

TextureResult DecodeTexture(const gpu::TextureDesc& d, std::span<const uint8_t> resource,
  std::span<const uint32_t> palette, uint32_t level, uint32_t face) {
  TextureResult out;
  const auto layout = DescribeTextureLayout(d, level, face);
  if (!layout.supported) {
    out.diagnostic = layout.diagnostic;
    return out;
  }
  if (layout.offset > resource.size() || layout.bytes > resource.size() - layout.offset) {
    out.diagnostic = "menu texture bounded span truncated: need offset=" + std::to_string(layout.offset) + " bytes=" + std::to_string(layout.bytes)
      + " available=" + std::to_string(resource.size());
    return out;
  }
  if (d.format == 0x0b && palette.size() < 256) {
    out.diagnostic = "menu P8 texture requires 256 palette entries";
    return out;
  }
  const auto f = GetFormat(d.format);
  const auto src = resource.subspan(size_t(layout.offset), size_t(layout.bytes));
  out.width = layout.width;
  out.height = layout.height;
  out.depth = layout.depth;
  out.row_pitch = out.width * 4;
  out.pixels.resize(size_t(out.row_pitch) * out.height * out.depth);
  const auto store = [&](uint32_t x, uint32_t y, uint32_t z, const Pixel& pixel) {
    const size_t offset = ((size_t(z) * out.height + y) * out.width + x) * 4;
    std::copy(pixel.begin(), pixel.end(), out.pixels.begin() + offset);
  };
  if (f.compressed) {
    const uint32_t bw = (out.width + 3) / 4;
    const uint32_t bh = (out.height + 3) / 4;
    const uint32_t blocks = bw * bh * out.depth;
    for (uint32_t n = 0; n < blocks; ++n) {
      const auto block = Block(d.format, src.subspan(size_t(n) * f.bytes, f.bytes));
      const uint32_t bx = n % bw * 4;
      const uint32_t by = n / bw % bh * 4;
      const uint32_t z = n / (bw * bh);
      for (uint32_t n = 0; n < 16; ++n) {
        const uint32_t x = bx + n % 4;
        const uint32_t y = by + n / 4;
        if (x < out.width && y < out.height) {
          store(x, y, z, block[n]);
        }
      }
    }
  } else {
    const uint32_t count = out.width * out.height * out.depth;
    for (uint32_t n = 0; n < count; ++n) {
      const uint32_t x = n % out.width;
      const uint32_t y = n / out.width % out.height;
      const uint32_t z = n / (out.width * out.height);
      const size_t offset = d.linear ? size_t(y) * d.pitch + x * f.bytes
        : size_t(Morton(x, y, z, out.width, out.height, out.depth)) * f.bytes;
      store(x, y, z, Texel(f.color, src.subspan(offset, f.bytes), palette));
    }
  }
  out.supported = true;
  return out;
}

SurfaceEncodeResult EncodeSurface(const gpu::TextureDesc& d, std::span<const uint8_t> original, std::span<const uint8_t> rgba) {
  SurfaceEncodeResult result;
  const auto layout = DescribeTextureLayout(d);
  if (!layout.supported || d.cube || d.depth != 1 || d.levels != 1 ||
      (d.format != 0x06 && d.format != 0x07 && d.format != 0x12 && d.format != 0x1e) ||
      original.size() != layout.total_bytes || rgba.size() != uint64_t(d.width) * d.height * 4) {
    result.diagnostic = "unsupported or truncated color surface readback";
    return result;
  }
  result.bytes.assign(original.begin(), original.end());
  for (uint32_t n = 0; n < d.width * d.height; ++n) {
    const uint32_t x = n % d.width, y = n / d.width;
    const size_t destination = d.linear ? size_t(y) * d.pitch + x * 4 : size_t(Morton(x, y, 0, d.width, d.height, 1)) * 4;
    result.bytes[destination] = rgba[n * 4 + 2];
    result.bytes[destination + 1] = rgba[n * 4 + 1];
    result.bytes[destination + 2] = rgba[n * 4];
    result.bytes[destination + 3] = d.format == 0x07 || d.format == 0x1e ? 255 : rgba[n * 4 + 3];
  }
  result.supported = true;
  return result;
}

}
