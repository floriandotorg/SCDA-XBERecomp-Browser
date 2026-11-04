#include "browser/menu-texture.h"

#include <array>
#include <cassert>
#include <iostream>

using browser::menu::DecodeTexture;

int main() {
  gpu::TextureDesc d;
  d.format = 0x12;
  d.width = 2;
  d.height = 1;
  d.pitch = 12;
  d.linear = true;
  std::array<uint8_t, 12> bgra{1, 2, 3, 4, 5, 6, 7, 8};
  auto r = DecodeTexture(d, bgra);
  assert(r.supported && r.format == "rgba8unorm" && r.row_pitch == 8);
  assert((r.pixels == std::vector<uint8_t>{3, 2, 1, 4, 7, 6, 5, 8}));
  assert(!DecodeTexture(d, std::span(bgra).first(7)).supported);
  d.pitch = 4;
  assert(!DecodeTexture(d, bgra).supported);
  d.width = d.height = d.depth = 1;
  d.pitch = 1;
  d.format = 0x1f;
  const std::array<uint8_t, 1> alpha{37};
  r = DecodeTexture(d, alpha);
  assert((r.pixels == std::vector<uint8_t>{255, 255, 255, 37}));
  d.format = 0x20;
  d.pitch = 2;
  r = DecodeTexture(d, std::array<uint8_t, 2>{20, 50});
  assert((r.pixels == std::vector<uint8_t>{20, 20, 20, 50}));
  d.format = 0x3f;
  d.pitch = 4;
  r = DecodeTexture(d, std::array<uint8_t, 4>{10, 20, 30, 40});
  assert((r.pixels == std::vector<uint8_t>{10, 20, 30, 40}));
  d.format = 0x40;
  r = DecodeTexture(d, std::array<uint8_t, 4>{10, 20, 30, 40});
  assert((r.pixels == std::vector<uint8_t>{20, 30, 40, 10}));
  d.format = 0x41;
  r = DecodeTexture(d, std::array<uint8_t, 4>{10, 20, 30, 40});
  assert((r.pixels == std::vector<uint8_t>{40, 30, 20, 10}));
  d.format = 0x00;
  d.width = 4;
  d.height = 2;
  d.linear = false;
  const std::array<uint8_t, 8> morton{0, 1, 4, 5, 2, 3, 6, 7};
  r = DecodeTexture(d, morton);
  assert(r.supported);
  for (int n = 0; n < 8; ++n) {
    assert(r.pixels[n * 4] == n && r.pixels[n * 4 + 3] == 255);
  }
  d.width = d.height = 1;
  d.format = 0x0b;
  std::array<uint32_t, 256> palette{};
  palette[37] = 0x7f123456;
  r = DecodeTexture(d, alpha, palette);
  assert((r.pixels == std::vector<uint8_t>{0x12, 0x34, 0x56, 0x7f}));
  assert(!DecodeTexture(d, alpha).supported);
  d.format = 0x0c;
  d.width = d.height = 4;
  const std::array<uint8_t, 8> bc1{0, 0xf8, 0xe0, 7, 0xe4, 0xe4, 0xe4, 0xe4};
  r = DecodeTexture(d, bc1);
  assert(r.supported);
  assert((std::vector<uint8_t>(r.pixels.begin(), r.pixels.begin() + 16) == std::vector<uint8_t>{255, 0, 0, 255, 0, 255, 0, 255, 170, 85, 0, 255, 85, 170, 0, 255}));
  const std::array<uint8_t, 8> bc1transparent{0, 0, 255, 255, 255, 255, 255, 255};
  r = DecodeTexture(d, bc1transparent);
  assert(r.pixels[3] == 0 && r.pixels[0] == 0);
  d.format = 0x0e;
  std::array<uint8_t, 16> bc2{0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe, 0, 0xf8, 0, 0, 0, 0, 0, 0};
  r = DecodeTexture(d, bc2);
  assert(r.supported);
  for (int n = 0; n < 16; ++n) {
    assert(r.pixels[n * 4] == 255 && r.pixels[n * 4 + 3] == n * 17);
  }
  d.format = 0x0f;
  std::array<uint8_t, 16> bc3{255, 0, 0x88, 0xc6, 0xfa, 0x88, 0xc6, 0xfa, 0, 0xf8, 0, 0, 0, 0, 0, 0};
  r = DecodeTexture(d, bc3);
  const std::array<uint8_t, 8> alphas{255, 0, 218, 182, 145, 109, 72, 36};
  for (int n = 0; n < 16; ++n) {
    assert(r.pixels[n * 4 + 3] == alphas[n % 8]);
  }
  bc3[0] = 0;
  bc3[1] = 100;
  r = DecodeTexture(d, bc3);
  const std::array<uint8_t, 8> lowalphas{0, 100, 20, 40, 60, 80, 0, 255};
  for (int n = 0; n < 16; ++n) {
    assert(r.pixels[n * 4 + 3] == lowalphas[n % 8]);
  }
  d.width = 3;
  d.height = 2;
  r = DecodeTexture(d, bc3);
  assert(r.supported && r.pixels.size() == 24);
  assert(!DecodeTexture(d, std::span(bc3).first(15)).supported);
  d.format = 0x66;
  assert(!DecodeTexture(d, bc3).supported);
  d.width = 0xffffffff;
  assert(!DecodeTexture(d, bc3).supported);
  d.width = d.height = 1;
  d.format = 0x00;
  d.levels = 2;
  r = DecodeTexture(d, std::array<uint8_t, 2>{10, 20}, {}, 1);
  assert(r.supported && r.pixels[0] == 20);
  d.cube = true;
  std::array<uint8_t, 129> cube{};
  cube[128] = 42;
  r = DecodeTexture(d, cube, {}, 0, 1);
  assert(r.supported && r.pixels[0] == 42);
  assert(!DecodeTexture(d, cube, {}, 0, 6).supported);
  const auto layout = browser::menu::DescribeTextureLayout(d, 0, 1);
  assert(layout.supported && layout.offset == 128 && layout.bytes == 1 && layout.total_bytes == 768);
  d.cube = false;
  d.levels = 1;
  d.width = 4;
  d.height = 2;
  d.depth = 2;
  std::array<uint8_t, 16> volume{0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15};
  r = DecodeTexture(d, volume);
  assert(r.supported && r.depth == 2);
  for (int n = 0; n < 16; ++n) {
    assert(r.pixels[n * 4] == n);
    assert(!DecodeTexture(d, std::span(volume).first(n)).supported);
  }
  d.width = 3;
  assert(!browser::menu::DescribeTextureLayout(d).supported);
  d.width = d.height = d.depth = 1;
  d.linear = true;
  d.format = 0x12;
  d.pitch = 0xffffffff;
  assert(!browser::menu::DescribeTextureLayout(d).supported);
  struct Fixture {
    uint32_t format;
    std::array<uint8_t, 4> bytes;
    std::array<uint8_t, 4> pixel;
    uint32_t pitch;
  };
  const Fixture fixtures[] = {
    {0x10, {0x00, 0xfc}, {255, 0, 0, 255}, 2},
    {0x1c, {0xe0, 0x03}, {0, 255, 0, 255}, 2},
    {0x1d, {0x21, 0x43}, {51, 34, 17, 68}, 2},
    {0x11, {0xe0, 0x07}, {0, 255, 0, 255}, 2},
    {0x1e, {1, 2, 3, 0}, {3, 2, 1, 255}, 4},
    {0x1b, {42}, {42, 42, 42, 42}, 1},
    {0x16, {10, 20}, {20, 10, 10, 20}, 2},
    {0x17, {10, 20}, {10, 20, 10, 20}, 2},
    {0x35, {10, 20}, {20, 20, 20, 255}, 2},
    {0x36, {10, 20, 30, 40}, {20, 40, 0, 255}, 4},
    {0x37, {0x1f, 0xfc}, {255, 0, 255, 255}, 2},
    {0x3d, {1, 0xf8}, {255, 0, 0, 255}, 2},
    {0x3e, {0x21, 0x43}, {68, 51, 34, 17}, 2}
  };
  for (const auto& fixture : fixtures) {
    d.format = fixture.format;
    d.pitch = fixture.pitch;
    r = DecodeTexture(d, std::span(fixture.bytes).first(d.pitch));
    assert(r.supported && std::equal(r.pixels.begin(), r.pixels.end(), fixture.pixel.begin()));
  }
  for (uint32_t n = 0; n < 256; ++n) {
    d.format = n;
    const auto probe = browser::menu::DescribeTextureLayout(d);
    if (probe.supported) {
      assert(!DecodeTexture(d, {}).supported);
    }
  }
  {
    gpu::TextureDesc surface;
    surface.width = surface.height = 4;
    surface.format = 6;
    const std::array<uint32_t, 16> order{0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};
    std::vector<uint8_t> rgba(64), original(64, 99), expected(64);
    for (uint32_t n = 0; n < 16; ++n) {
      rgba[n * 4] = uint8_t(n); rgba[n * 4 + 1] = uint8_t(n + 1); rgba[n * 4 + 2] = uint8_t(n + 2); rgba[n * 4 + 3] = 200;
      expected[order[n] * 4] = uint8_t(n + 2); expected[order[n] * 4 + 1] = uint8_t(n + 1);
      expected[order[n] * 4 + 2] = uint8_t(n); expected[order[n] * 4 + 3] = 200;
    }
    const auto encoded = browser::menu::EncodeSurface(surface, original, rgba);
    assert(encoded.supported && encoded.bytes == expected);
    assert(!browser::menu::EncodeSurface(surface, original, std::span(rgba).first(60)).supported);
    surface.linear = true; surface.format = 0x1e; surface.pitch = 20;
    original.assign(80, 99);
    const auto linear = browser::menu::EncodeSurface(surface, original, rgba);
    assert(linear.supported && linear.bytes[3] == 255 && linear.bytes[16] == 99 && linear.bytes[36] == 99);
  }
  std::cout << "menu-texture-ok\n";
}
