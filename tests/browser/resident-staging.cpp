#include "browser/menu-backend.h"
#include <cstring>
#include <map>
#include <set>
#include <cstdio>

int main() {
  using namespace browser;
  using webgpu::Opcode;
  const auto check = [](bool value) { if (!value) { throw std::runtime_error("resident staging changed queued geometry"); } };
  Runtime runtime({}, {});
  PreparedImage image;
  image.memory = std::make_unique<Memory>(); image.boundRuntime = &runtime;
  image.memory->Ensure(0x10000, 0x10000); image.memory->Ensure(0x10012000, 4096);
  std::map<uint32_t, std::vector<uint8_t>> buffers;
  std::set<uint32_t> live;
  std::vector<float> positions;
  uint32_t sequence = 0, batches = 0;
  MenuBackendLimits limits;
  limits.residentVertices = true; limits.batchDraws = true;
  limits.maxResources = 4; limits.maxResourceBytes = 1u << 20; limits.maxResidentBytes = 65536;
  const auto applyUpload = [&](uint32_t id, std::span<const uint8_t> bytes) {
      webgpu::BufferUpload upload;
      std::memcpy(&upload, bytes.data(), sizeof(upload));
      if (!buffers.contains(id)) { check(live.insert(id).second); }
      auto& buffer = buffers[id]; buffer.resize(upload.capacity);
      check(upload.offset + bytes.size() - sizeof(upload) <= buffer.size());
      std::memcpy(buffer.data() + upload.offset, bytes.data() + sizeof(upload), bytes.size() - sizeof(upload));

  };
  const auto exchange = [&](auto opcode, auto resource, auto bytes) {
    check(!Memory::HasBorrow());
    if (opcode == Opcode::CreateTexture || opcode == Opcode::CreatePipeline) { check(live.insert(resource).second); }
    if (opcode == Opcode::UploadBuffer) { applyUpload(resource, bytes); }
    if (opcode == Opcode::UploadDrawBatch) {
      webgpu::UploadDrawPacket batch;
      std::memcpy(&batch, bytes.data(), sizeof(batch)); check(!batch.mixed);
      size_t offset = sizeof(batch);
      for (uint32_t n = 0; n < batch.upload_count; ++n) {
        uint32_t row[2]; std::memcpy(row, bytes.data() + offset, 8); offset += 8;
        applyUpload(row[0], bytes.subspan(offset, row[1])); offset += row[1];
      }
      check(offset == sizeof(batch) + batch.upload_bytes);
      bytes = bytes.subspan(offset, batch.draw_bytes);
    }
    if (opcode == Opcode::DrawBatch || opcode == Opcode::UploadDrawBatch) {
      uint32_t count;
      std::memcpy(&count, bytes.data(), 4);
      if (count) { ++batches; }
      size_t offset = 4;
      for (uint32_t n = 0; n < count; ++n) {
        uint32_t length;
        std::memcpy(&length, bytes.data() + offset + 4, 4);
        webgpu::ResidentDraw draw;
        std::memcpy(&draw, bytes.data() + offset + 8 + sizeof(webgpu::DrawPacket), sizeof(draw));
        float value;
        std::memcpy(&value, buffers.at(draw.vertices[0]).data() + draw.offsets[0], 4);
        positions.push_back(value); offset += 8 + length;
      }
    }
    if (opcode == Opcode::Destroy) { check(live.erase(resource) == 1); buffers.erase(resource); }
    return webgpu::Result{webgpu::Status::Ok, ++sequence, resource, {}};
  };
  MenuBackend backend(runtime, image, exchange, limits);
  check(backend.Init(4, 4));
  gpu::State state;
  state.vs_handle = 0x44; state.streams[0] = {0x10000, 20};
  state.rt = {0x10100, 0x10012000, 0x00001200, 3 | (3 << 12)};
  state.viewport.w = state.viewport.h = 4;
  state.simple[gpu::kMColorWriteEnable / 4] = 0x01010101;
  state.tss[0][gpu::kTssColorOp] = 1;
  const uint32_t vertexHeader[] = {1, 0x80011000, 0};
  const uint32_t targetHeader[] = {0x50001, state.rt.data, 0, state.rt.format, state.rt.size};
  const float vertices[] = {1, 0, 0, 1, 1, 4, 0, 0, 1, 1, 0, 4, 0, 1, 1, 4, 4, 0, 1, 1};
  image.memory->Write({}, 0x10000, vertexHeader, sizeof(vertexHeader));
  image.memory->Write({}, state.rt.header, targetHeader, sizeof(targetHeader));
  image.memory->Write({}, vertexHeader[1], vertices, sizeof(vertices));
  backend.Draw(state, 5, 0, 3, nullptr);
  backend.Draw(state, 5, 1, 3, nullptr);
  check(batches == 0 && buffers.empty());
  image.memory->Store<float>({}, vertexHeader[1], 2);
  backend.Draw(state, 5, 0, 3, nullptr);
  check(batches == 1 && positions == std::vector<float>({1, 4}));
  backend.Flush();
  check(batches == 2 && positions == std::vector<float>({1, 4, 2}));
  std::vector<float> grid(205 * 5, 0);
  for (uint32_t n = 0; n < 205; ++n) { grid[n * 5] = float(n + 10); grid[n * 5 + 3] = 1; }
  image.memory->Write({}, vertexHeader[1], grid.data(), grid.size() * 4);
  std::vector<float> expected{1, 4, 2};
  for (uint32_t n = 0; n < 200; ++n) {
    backend.Draw(state, 5, n, 3, nullptr); expected.push_back(float(n + 10));
    check(backend.Snapshot().resourceCount <= limits.maxResources);
  }
  backend.Flush(); check(positions == expected && buffers.size() == 1);
  backend.Retire(); check(buffers.empty() && live.empty());
  MenuBackend cancelled(runtime, image, exchange, limits);
  check(cancelled.Init(4, 4));
  const auto before = batches;
  cancelled.Draw(state, 5, 0, 3, nullptr); cancelled.Retire();
  check(batches == before && buffers.empty() && live.empty());
  std::puts("resident-backend-ok");
}
