#include "browser/menu-backend.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <source_location>

int main() {
  using namespace browser;
  using webgpu::Opcode;
  const auto check = [](bool value, std::source_location location = std::source_location::current()) { if (!value) { throw std::runtime_error("released surface contract failed at " + std::to_string(location.line())); } };
  Runtime runtime({}, {});
  PreparedImage image;
  image.memory = std::make_unique<Memory>();
  image.boundRuntime = &runtime;
  image.memory->Ensure(0x10000, 0x10000);
  image.memory->Ensure(0x10012000, 0x2000);
  uint32_t sequence = 0;
  std::map<uint32_t, std::vector<uint8_t>> pixels;
  MenuBackend backend(runtime, image, [&](auto opcode, auto resource, auto bytes) {
    check(!Memory::HasBorrow());
    if (opcode == Opcode::CreateTexture) {
      webgpu::TexturePacket packet;
      std::memcpy(&packet, bytes.data(), sizeof(packet));
      pixels[resource].assign(bytes.begin() + sizeof(packet), bytes.begin() + sizeof(packet) + packet.width * packet.height * 4);
    }
    if (opcode == Opcode::Copy) {
      webgpu::CopyPacket packet;
      std::memcpy(&packet, bytes.data(), sizeof(packet));
      pixels.at(resource) = pixels.at(packet.source);
    }
    if (opcode == Opcode::Destroy) { pixels.erase(resource); }
    if (opcode == Opcode::Readback) { return webgpu::Result{webgpu::Status::Ok, ++sequence, resource, pixels.at(resource)}; }
    return webgpu::Result{webgpu::Status::Ok, ++sequence, resource, {}};
  });
  check(backend.Init(4, 4));

  const std::array<uint32_t, 5> source{0x50001, 0x10012000, 0, 0x00001200, 3 | (3 << 12)};
  const std::array<uint32_t, 5> face{0x50001, 0x10013000, 0, 0x0223062d, 0};
  image.memory->Write({}, 0x10100, source.data(), sizeof(source));
  image.memory->Write({}, 0x10200, face.data(), sizeof(face));
  std::array<uint8_t, 64> texels{};
  for (uint32_t n = 0; n < 16; ++n) { texels[n * 4] = uint8_t(n); texels[n * 4 + 1] = uint8_t(n + 1); texels[n * 4 + 2] = uint8_t(n + 2); texels[n * 4 + 3] = 255; }
  for (uint32_t row = 0; row < 4; ++row) { image.memory->Write({}, source[1] + row * 64, texels.data() + row * 16, 16); }

  backend.Copy({0x10100, source[1], source[3], source[4]}, {0x10200, face[1], face[3], face[4]}, 0, 0, 0);
  check(backend.HasGpuContents(face[1]));
  check(backend.ReleaseResource(0x10200, face[1]));

  std::array<uint8_t, 64> written{};
  image.memory->Read({}, face[1], written.data(), written.size());
  const std::array<uint32_t, 16> order{0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};
  for (uint32_t n = 0; n < 16; ++n) {
    check(std::memcmp(written.data() + order[n] * 4, texels.data() + n * 4, 4) == 0);
  }
  std::puts("released-surface-ok");
}
