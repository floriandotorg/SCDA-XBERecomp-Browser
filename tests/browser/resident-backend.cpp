#include "browser/menu-backend.h"
#include <cstdio>
#include <cstring>
#include <source_location>

int main() {
  using namespace browser;
  using webgpu::Opcode;
  const auto check = [](bool value, std::source_location location = std::source_location::current()) { if (!value) { throw std::runtime_error("resident cache invalidation contract failed at " + std::to_string(location.line())); } };
  Runtime runtime({}, {});
  PreparedImage image;
  image.memory = std::make_unique<Memory>();
  image.boundRuntime = &runtime;
  image.memory->Ensure(0x10000, 0x10000);
  uint32_t sequence = 0, uploads = 0, draws = 0;
  std::vector<uint8_t> lastUpload;
  std::vector<uint32_t> geometryUploads;
  std::vector<uint32_t> bufferUploads;
  MenuBackendLimits limits;
  limits.residentVertices = true;
  limits.maxResources = 4;
  limits.maxResourceBytes = 256;
  MenuBackend backend(runtime, image, [&](auto opcode, auto resource, auto bytes) {
    check(!Memory::HasBorrow());
    if (opcode == Opcode::UploadBuffer) { bufferUploads.push_back(resource); ++uploads; lastUpload.assign(bytes.begin(), bytes.end()); if (bytes.size() == 60 + sizeof(webgpu::BufferUpload)) { geometryUploads.push_back(resource); } }
    if (opcode == Opcode::Draw) {
      webgpu::DrawPacket packet;
      std::memcpy(&packet, bytes.data(), sizeof(packet));
      check(packet.index32 == 2 && packet.vertex_bytes == 0 && packet.index_count != 0);
      ++draws;
    }
    return webgpu::Result{webgpu::Status::Ok, ++sequence, resource, {}};
  }, limits);
  check(backend.Init(4, 4));
  gpu::State state;
  state.vs_handle = 0x44;
  state.streams[0] = {0x10000, 20};
  state.rt = {0x10100, 0x10012000, 0x00001200, 3 | (3 << 12)};
  state.viewport.w = state.viewport.h = 4;
  state.simple[gpu::kMColorWriteEnable / 4] = 0x01010101;
  state.tss[0][gpu::kTssColorOp] = 1;
  image.memory->Ensure(0x10012000, 4096);
  uint32_t vb[] = {1, 0x80011000, 0};
  const uint32_t surface[] = {0x50001, state.rt.data, 0, state.rt.format, state.rt.size};
  image.memory->Write({}, 0x10000, vb, sizeof(vb));
  image.memory->Write({}, state.rt.header, surface, sizeof(surface));
  const float vertices[] = {0, 0, 0, 1, 1, 4, 0, 0, 1, 1, 0, 4, 0, 1, 1};
  image.memory->Write({}, vb[1], vertices, sizeof(vertices));
  backend.Draw(state, 5, 0, 3, nullptr);
  check(uploads == 2 && draws == 1);
  check(bufferUploads[0] == bufferUploads[1]);
  backend.Draw(state, 5, 0, 3, nullptr);
  check(uploads == 2 && draws == 2);
  image.memory->Store<float>({}, 0xf0011000, 2);
  backend.Draw(state, 5, 0, 3, nullptr);
  check(uploads == 3);
  check(geometryUploads.size() == 2 && geometryUploads[0] == geometryUploads[1]);
  {
    auto lease = image.memory->Borrow();
    auto* pointer = image.memory->Span({}, vb[1], 4);
    std::memcpy(pointer, vertices, 4);
  }
  backend.Draw(state, 5, 0, 3, nullptr);
  check(uploads == 4);
  image.memory->Ensure(0x20011000, 4096);
  vb[1] = 0x20011000;
  image.memory->Write({}, 0x10000, vb, sizeof(vb));
  image.memory->Write({}, vb[1], vertices, sizeof(vertices));
  backend.Draw(state, 5, 0, 3, nullptr);
  backend.Draw(state, 5, 0, 3, nullptr);
  check(uploads == 6);
  vb[1] = 0x80011000;
  image.memory->Write({}, 0x10000, vb, sizeof(vb));
  const uint16_t indices[] = {2, 0, 1, 2};
  const auto beforeIndexed = uploads;
  backend.Draw(state, 8, 0, 4, reinterpret_cast<const uint8_t*>(indices));
  backend.Draw(state, 8, 0, 4, reinterpret_cast<const uint8_t*>(indices));
  check(uploads == beforeIndexed + 1);
  const uint16_t changed[] = {1, 0, 2, 1};
  backend.Draw(state, 8, 0, 4, reinterpret_cast<const uint8_t*>(changed));
  check(uploads == beforeIndexed + 2);
  backend.Draw(state, 3, 0, 3, reinterpret_cast<const uint8_t*>(indices));
  backend.Draw(state, 7, 0, 3, reinterpret_cast<const uint8_t*>(indices));
  check(backend.Snapshot().resourceCount <= limits.maxResources);
  const uint16_t shifted[] = {3, 1, 2, 3};
  const auto beforeShift = uploads;
  backend.Draw(state, 8, 0, 4, reinterpret_cast<const uint8_t*>(shifted));
  check(uploads == beforeShift + 1);
  for (uint32_t n = 0; n < 20; ++n) {
    state.base_vertex = n % 10; backend.Draw(state, 5, 0, 3, reinterpret_cast<const uint8_t*>(indices));
    check(backend.Snapshot().resourceCount <= limits.maxResources);
  }
  state.index_buffer = state.streams[0].vb;
  bool rejectedIndexHeader = false;
  try { backend.Draw(state, 5, 0, 3, reinterpret_cast<const uint8_t*>(indices)); }
  catch (const std::exception& error) { rejectedIndexHeader = std::string(error.what()).find("index buffer") != std::string::npos; }
  check(rejectedIndexHeader); state.index_buffer = 0;
  backend.Retire();
  bool sawFallback = false;
  MenuBackend checked(runtime, image, [&](auto opcode, auto resource, auto bytes) {
    if (opcode == Opcode::Draw) {
      webgpu::DrawPacket packet; std::memcpy(&packet, bytes.data(), sizeof(packet));
      check(packet.index32 != 2); sawFallback = true;
    }
    return webgpu::Result{webgpu::Status::Ok, ++sequence, resource, {}};
  }, limits);
  check(checked.Init(4, 4));
  state.vs_handle = 1; state.base_vertex = 0;
  state.vs = std::make_shared<gpu::VertexShader>();
  state.vs->program_type = 0x2078;
  state.vs->inputs[0] = {0, 0, 0x42}; state.vs->inputs[3] = {0, 2, 0x40};
  state.vs->program = {0, (1u << 21) | (96u << 13) | 0x1b, (2u << 26) | 0x360000 | (3u << 11) | 0x6c, (2u << 28) | (15u << 12) | (1u << 11) | 1};
  checked.Draw(state, 5, 0, 3, nullptr); check(sawFallback); checked.Retire();
  std::puts("resident-backend-ok");
}
