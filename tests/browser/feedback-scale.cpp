#include "browser/menu-backend.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>

int main() {
  using namespace browser;
  using webgpu::Opcode;
  const auto check = [](bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } };
  Runtime runtime({}, {});
  PreparedImage image;
  image.memory = std::make_unique<Memory>(); image.boundRuntime = &runtime;
  image.memory->Ensure(0x10000, 0x10000); image.memory->Ensure(0x10012000, 4096);
  std::set<uint32_t> live;
  uint32_t sequence = 0, copies = 0;
  const auto exchange = [&](auto opcode, auto resource, auto) {
    if (opcode == Opcode::CreateTexture || opcode == Opcode::CreatePipeline) { live.insert(resource); }
    if (opcode == Opcode::Destroy) { live.erase(resource); }
    if (opcode == Opcode::Copy) { ++copies; }
    return webgpu::Result{webgpu::Status::Ok, ++sequence, resource, {}};
  };
  MenuBackendLimits limits;
  limits.maxResourceBytes = 256;
  limits.maxResidentBytes = 65536;
  MenuBackend backend(runtime, image, exchange, limits);
  backend.SetOutputSource([] { return MenuBackend::Output{12, 12, true, 3.0f}; });
  check(backend.Init(4, 4), "init failed");
  gpu::State state;
  state.vs_handle = 0x44; state.streams[0] = {0x10000, 20};
  state.rt = {0x10100, 0x10012000, 0x00001200, 3 | (3 << 12)};
  state.viewport.w = state.viewport.h = 4;
  state.simple[gpu::kMColorWriteEnable / 4] = 0x01010101;
  state.tss[0][gpu::kTssColorOp] = 1;
  const uint32_t vertexHeader[] = {1, 0x80011000, 0};
  const uint32_t targetHeader[] = {0x50001, state.rt.data, 0, state.rt.format, state.rt.size};
  const uint32_t viewHeader[] = {0x40001, state.rt.data, 0, state.rt.format | 8, state.rt.size};
  const float vertices[] = {1, 0, 0, 1, 1, 4, 0, 0, 1, 1, 0, 4, 0, 1, 1};
  image.memory->Write({}, 0x10000, vertexHeader, sizeof(vertexHeader));
  image.memory->Write({}, state.rt.header, targetHeader, sizeof(targetHeader));
  image.memory->Write({}, 0x10200, viewHeader, sizeof(viewHeader));
  image.memory->Write({}, vertexHeader[1], vertices, sizeof(vertices));
  backend.Clear(state, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
  backend.Present(state.rt, false);
  state.textures[0] = 0x10200;
  state.tss[0][gpu::kTssAddressU] = state.tss[0][gpu::kTssAddressV] = 3;
  state.tss[0][gpu::kTssMagFilter] = state.tss[0][gpu::kTssMinFilter] = 2;
  state.tss[0][gpu::kTssMipFilter] = 1;
  backend.Draw(state, 5, 0, 3, nullptr);
  backend.Flush();
  check(copies == 1, "scaled feedback copy was not made");
  check(backend.Snapshot().residentBytes <= limits.maxResidentBytes, "scaled feedback copy exceeded the resident budget");
  backend.Retire();
  check(live.empty(), "scaled feedback copy leaked");
  std::puts("feedback-scale-ok");
}
