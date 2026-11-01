#include "browser/menu-backend.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>

namespace {
const auto check = [](bool valid, const char* reason) {
  if (!valid) { throw std::runtime_error(reason); }
};
const auto rejects = [](auto operation, const char* diagnostic) {
  try { operation(); } catch (const std::exception& error) {
    check(std::string(error.what()).find(diagnostic) != std::string::npos, error.what());
    return;
  }
  throw std::runtime_error(std::string("missing expected diagnostic: ") + diagnostic);
};
struct Command {
  browser::webgpu::Opcode opcode;
  uint32_t resource;
  std::vector<uint8_t> bytes;
};
struct Service {
  uint32_t sequence = 0;
  std::vector<Command> commands;
  std::map<uint32_t, std::vector<uint8_t>> pixels;
  std::set<uint32_t> live;
  bool fail = false;
  browser::webgpu::Result Execute(browser::webgpu::Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes) {
    check(!browser::Memory::HasBorrow(), "no borrowed guest pointer during service wait");
    commands.push_back({opcode, resource, {bytes.begin(), bytes.end()}});
    if (fail) { return {browser::webgpu::Status::Lost, ++sequence, resource, {}}; }
    if (opcode == browser::webgpu::Opcode::CreateTexture || opcode == browser::webgpu::Opcode::CreatePipeline) {
      check(live.insert(resource).second, "GPU resource IDs cannot alias across backends sharing a service");
    }
    if (opcode == browser::webgpu::Opcode::Destroy) { check(live.erase(resource) == 1, "destroy actual live GPU resource"); }
    if (opcode == browser::webgpu::Opcode::CreateTexture) {
      browser::webgpu::TexturePacket packet;
      std::memcpy(&packet, bytes.data(), sizeof(packet));
      pixels[resource] = std::vector<uint8_t>(packet.width * packet.height * 4, 0);
    }
    if (opcode == browser::webgpu::Opcode::Readback) {
      return {browser::webgpu::Status::Ok, ++sequence, resource, pixels.at(resource)};
    }
    return {browser::webgpu::Status::Ok, ++sequence, resource, {}};
  }
  size_t Count(browser::webgpu::Opcode opcode) const {
    return std::count_if(commands.begin(), commands.end(), [&](const auto& command) { return command.opcode == opcode; });
  }
};
}

int main() {
  try {
    using namespace browser;
    using webgpu::Opcode;
    Runtime runtime({}, {});
    PreparedImage image;
    image.memory = std::make_unique<Memory>();
    image.boundRuntime = &runtime;
    image.memory->Map(0x10000, 0x10000);
    image.memory->Commit(0x10000, 0x10000);
    Service service;
    MenuBackend backend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return service.Execute(opcode, resource, bytes); });
    check(backend.Init(4, 4), "device initialized");
    gpu::State state;
    state.vs_handle = 0x44;
    state.streams[0] = {0x10000, 20};
    state.rt = {0x10100, 0x10012000, 0x00001200, 3 | (3 << 12)};
    state.viewport.w = state.viewport.h = 4;
    state.simple[gpu::kMColorWriteEnable / 4] = 0x01010101;
    state.tss[0][gpu::kTssColorOp] = 1;
    image.memory->Map(0x10011000, 8192);
    image.memory->Commit(0x10011000, 8192);
    const uint32_t vb[] = {1, 0x10011000, 0};
    const uint32_t surface[] = {0x50001, state.rt.data, 0, state.rt.format, state.rt.size};
    image.memory->Write({}, 0x10000, vb, sizeof(vb));
    image.memory->Write({}, state.rt.header, surface, sizeof(surface));
    const float vertices[] = {0, 0, 0, 1, 1, 4, 0, 0, 1, 1, 0, 4, 0, 1, 1};
    image.memory->Write({}, 0x10011000, vertices, sizeof(vertices));
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.Count(Opcode::Draw) == 1, "actual draw submitted");
    const auto creates = service.Count(Opcode::CreateTexture);
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.Count(Opcode::CreateTexture) == creates && service.Count(Opcode::CreatePipeline) == 1, "same resources and compatible pipeline reused");
    backend.Draw(state, 2, 0, 2, nullptr);
    check(service.Count(Opcode::CreatePipeline) == 2, "topology is pipeline state");
    const uint16_t triangleOrder[] = {2, 0, 1};
    image.memory->Write({}, 0x10011800, triangleOrder, sizeof(triangleOrder));
    const uint8_t* guestIndices;
    {
      auto lease = image.memory->Borrow();
      guestIndices = image.memory->Span({}, 0x10011800, sizeof(triangleOrder));
    }
    state.index_data = 0x10011800;
    backend.Draw(state, 5, 0, 3, guestIndices);
    float indexedY = 0;
    std::memcpy(&indexedY, service.commands.back().bytes.data() + sizeof(webgpu::DrawPacket) + 4, 4);
    check(indexedY == 4, "checked owned guest indices reorder actual vertices");
    state.index_data = 0;
    std::vector<uint16_t> hostIndices{2, 0, 1};
    backend.Draw(state, 5, 0, 3, reinterpret_cast<const uint8_t*>(hostIndices.data()));
    float hostIndexedY = 0;
    std::memcpy(&hostIndexedY, service.commands.back().bytes.data() + sizeof(webgpu::DrawPacket) + 4, 4);
    hostIndices.assign({0, 0, 0});
    check(hostIndexedY == 4, "frontend owned host index contract produces deep-owned actual vertex staging");
    const auto drawCommand = std::find_if(service.commands.begin(), service.commands.end(), [](const auto& command) { return command.opcode == Opcode::Draw; });
    webgpu::DrawPacket drawPacket;
    std::memcpy(&drawPacket, drawCommand->bytes.data(), sizeof(drawPacket));
    check(drawPacket.vertex_count == 3 && drawPacket.vertex_bytes == 3 * 256, "canonical sixteen float4 register ABI");
    float fourthRegister;
    std::memcpy(&fourthRegister, drawCommand->bytes.data() + sizeof(drawPacket) + 3 * 16, 4);
    check(fourthRegister == 128 / 255.0f, "D3DCOLOR normalized to canonical register");
    const auto owned = service.commands.back().bytes;
    image.memory->Decommit(0x10011000, 4096);
    check(service.commands.back().bytes == owned, "submitted vertex bytes owned");
    rejects([&] { backend.Draw(state, 5, 0, 3, nullptr); }, "memory");
    image.memory->Commit(0x10011000, 4096);
    image.memory->Write({}, 0x10011000, vertices, sizeof(vertices));
    state.rs[gpu::kRsStencilEnable] = 1;
    rejects([&] { backend.Draw(state, 5, 0, 3, nullptr); }, "stencil");
    state.rs[gpu::kRsStencilEnable] = 0;
    rejects([&] { backend.Draw(state, 42, 0, 3, nullptr); }, "topology");
    image.memory->Map(0x10013000, 0x3000);
    image.memory->Commit(0x10013000, 0x3000);
    const uint32_t texture[] = {0x40001, 0x10013000, 0, state.rt.format, state.rt.size};
    image.memory->Write({}, 0x10200, texture, sizeof(texture));
    state.textures[0] = 0x10200;
    state.tss[0][gpu::kTssAddressU] = state.tss[0][gpu::kTssAddressV] = 1;
    state.tss[0][gpu::kTssMagFilter] = state.tss[0][gpu::kTssMinFilter] = 2;
    state.tss[0][gpu::kTssMipFilter] = 1;
    backend.Draw(state, 5, 0, 3, nullptr);
    const auto textureCreates = service.Count(Opcode::CreateTexture);
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.Count(Opcode::CreateTexture) == textureCreates, "unchanged guest texture reused");
    const uint32_t pixel = 0xff123456;
    image.memory->Write({}, 0x10013000, &pixel, 4);
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.Count(Opcode::CreateTexture) == textureCreates + 1 && service.Count(Opcode::Destroy) == 1, "guest texture content version changes actual GPU resource");
    state.tss[0][gpu::kTssAddressU] = 4;
    rejects([&] { backend.Draw(state, 5, 0, 3, nullptr); }, "sampler");
    state.tss[0][gpu::kTssAddressU] = 1;
    const uint32_t badFormat = 0x7f00;
    image.memory->Write({}, 0x1020c, &badFormat, 4);
    rejects([&] { backend.Draw(state, 5, 0, 3, nullptr); }, "format");
    image.memory->Write({}, 0x10200, texture, sizeof(texture));
    state.textures[0] = 0;
    backend.Clear(state, 0, nullptr, gpu::kClearTarget, 0xff804020, 1, 0);
    check(service.Count(Opcode::Clear) == 1, "clear submitted");
    backend.RecordLate(state.rt.data);
    check(backend.DeliverLate(state.rt.data), "completed late readback delivered");
    check(!backend.DeliverLate(state.rt.data), "late readback consumed only once");
    image.memory->Write({}, state.rt.data, &pixel, 4);
    backend.SyncLate(state.rt.data);
    uint32_t firstLate = pixel;
    image.memory->Read({}, state.rt.data, &firstLate, 4);
    check(firstLate == 0, "first late lock publishes real GPU bytes before returning");
    check(service.Count(Opcode::Readback) == 3, "first late lock synchronizes absent history before recording next snapshot");
    const auto readbackBaseline = service.Count(Opcode::Readback);
    backend.Sync(state.rt.data, 64);
    check(service.Count(Opcode::Readback) == readbackBaseline + 1, "sync reads actual target");
    backend.Sync(state.rt.data, 0);
    check(service.Count(Opcode::Readback) == readbackBaseline + 2, "native BlockOnResource zero-size contract reads entire actual target");
    image.memory->Write({}, state.rt.data + 60, &pixel, 4);
    backend.Sync(state.rt.data, 4);
    uint32_t preserved = 0;
    image.memory->Read({}, state.rt.data + 60, &preserved, 4);
    check(preserved == pixel, "partial Sync never overwrites guest data outside requested range");
    const uint32_t secondSurface[] = {0x50001, 0x10014000, 0, state.rt.format, state.rt.size};
    image.memory->Write({}, 0x10300, secondSurface, sizeof(secondSurface));
    const gpu::Surface destination{0x10300, secondSurface[1], secondSurface[3], secondSurface[4]};
    backend.Copy(state.rt, destination, 0, 0, 0);
    check(service.Count(Opcode::Copy) == 1, "actual surface copy submitted");
    const uint32_t alias[] = {0x40001, state.rt.data, 0, state.rt.format, state.rt.size};
    image.memory->Write({}, 0x10400, alias, sizeof(alias));
    auto aliasState = state;
    aliasState.rt = destination;
    aliasState.textures[0] = 0x10400;
    const auto beforeAlias = service.Count(Opcode::CreateTexture);
    backend.Draw(aliasState, 5, 0, 3, nullptr);
    webgpu::DrawPacket aliasDraw;
    std::memcpy(&aliasDraw, service.commands.back().bytes.data(), sizeof(aliasDraw));
    const auto copyCommand = std::find_if(service.commands.begin(), service.commands.end(), [](const auto& command) { return command.opcode == Opcode::Copy; });
    const uint32_t frontId = copyCommand->resource;
    webgpu::CopyPacket copied;
    std::memcpy(&copied, copyCommand->bytes.data(), sizeof(copied));
    check(service.Count(Opcode::CreateTexture) == beforeAlias && aliasDraw.texture[0] == copied.source, "compatible known physical payload view reuses actual rendered GPU storage");
    rejects([&] { backend.Copy(state.rt, destination, 1, 0, 0); }, "partial copy");
    backend.RecordLate(state.rt.data);
    const uint32_t incompatible = state.rt.format ^ 0x100;
    image.memory->Write({}, state.rt.header + 12, &incompatible, 4);
    image.memory->Write({}, 0x1040c, &incompatible, 4);
    rejects([&] { backend.DeliverLate(state.rt.data); }, "stale");
    image.memory->Write({}, state.rt.header, surface, sizeof(surface));
    image.memory->Write({}, 0x10400, alias, sizeof(alias));
    check(backend.DeliverLate(state.rt.data), "owned late snapshot survives header restoration");
    backend.Present(state.rt, true);
    webgpu::PresentPacket aspect;
    std::memcpy(&aspect, service.commands.back().bytes.data(), sizeof(aspect));
    check(aspect.pillarbox == 1, "actual aspect compositor receives pillarbox contract");
    backend.Present(state.rt, false);
    check(service.Count(Opcode::Present) == 2, "present actual target");
    const uint32_t backId = service.commands.back().resource;
    image.memory->Write({}, state.rt.header + 4, &destination.data, 4);
    image.memory->Write({}, destination.header + 4, &state.rt.data, 4);
    const auto beforeSwap = service.Count(Opcode::CreateTexture);
    const gpu::Surface swappedFront{state.rt.header, destination.data, state.rt.format, state.rt.size};
    backend.Present(swappedFront, false);
    check(service.commands.back().resource == frontId, "front/back rotation follows physical payload not stable header");
    auto swappedState = state;
    swappedState.rt = {destination.header, state.rt.data, state.rt.format, state.rt.size};
    backend.Clear(swappedState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
    check(service.commands.back().resource == backId && service.Count(Opcode::CreateTexture) == beforeSwap, "rotating header payloads preserve actual GPU IDs without stale guest reupload");
    image.memory->Write({}, state.rt.header, surface, sizeof(surface));
    image.memory->Write({}, destination.header, secondSurface, sizeof(secondSurface));
    const auto fence = backend.Fence();
    check(backend.FencePending(fence), "queue acceptance is not GPU completion");
    backend.WaitFence(fence);
    check(!backend.FencePending(fence), "completed service fence honest");
    rejects([&] { backend.Sync(0xdead0000, 4); }, "unknown");
    rejects([&] { backend.Occlusion(state, nullptr); }, "occlusion");
    rejects([&] { backend.SetGamma(nullptr); }, "gamma");
    std::array<uint8_t, 768> ramp;
    for (uint32_t n = 0; n < ramp.size(); ++n) { ramp[n] = uint8_t(255 - n % 256); }
    backend.SetGamma(ramp.data());
    check(service.commands.back().opcode == Opcode::SetGamma && service.commands.back().bytes.size() == 768 && service.commands.back().bytes[0] == 255, "actual arbitrary gamma LUT submitted");
    ramp.fill(0);
    check(service.commands.back().bytes[0] == 255, "gamma command owns copied bytes");
    MenuBackendLimits limits;
    limits.maxVertices = 2;
    MenuBackend bounded(runtime, image, [&](auto opcode, auto resource, auto bytes) { return service.Execute(opcode, resource, bytes); }, limits);
    bounded.Init(4, 4);
    rejects([&] { bounded.Draw(state, 5, 0, 3, nullptr); }, "budget");
    MenuBackend shared(runtime, image, [&](auto opcode, auto resource, auto bytes) { return service.Execute(opcode, resource, bytes); });
    shared.Init(4, 4);
    shared.Clear(state, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
    MenuBackendLimits tiny;
    tiny.maxResidentBytes = 16;
    MenuBackend residentBounded(runtime, image, [&](auto opcode, auto resource, auto bytes) { return service.Execute(opcode, resource, bytes); }, tiny);
    residentBounded.Init(4, 4);
    rejects([&] { residentBounded.Clear(state, 0, nullptr, gpu::kClearTarget, 0, 1, 0); }, "resident");
    auto shader = std::make_shared<gpu::VertexShader>();
    shader->inputs[0] = {0, 0, 0x32};
    shader->inputs[3] = {0, 12, 0x40};
    state.vs = shader;
    state.vs_handle = 1;
    state.streams[0].stride = 16;
    const auto beforeLayout = service.Count(Opcode::CreatePipeline);
    backend.Draw(state, 5, 0, 3, nullptr);
    shader->inputs[3].offset = 8;
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.Count(Opcode::CreatePipeline) == beforeLayout + 2, "incompatible guest vertex layouts cannot reuse pipeline");
    const uint32_t depthHeader[] = {0x50001, 0x10015000, 0, 0x2e00, state.rt.size};
    image.memory->Write({}, 0x10500, depthHeader, sizeof(depthHeader));
    state.zs = {0x10500, depthHeader[1], depthHeader[3], depthHeader[4]};
    backend.Clear(state, 0, nullptr, gpu::kClearTarget | gpu::kClearZ | gpu::kClearStencil, 0xff123456, 0.5f, 7);
    state.rs[gpu::kRsZEnable] = 1;
    state.simple[gpu::kMZFunc / 4] = 0x203;
    state.simple[gpu::kMZWriteEnable / 4] = 1;
    backend.Draw(state, 5, 0, 3, nullptr);
    rejects([&] { backend.Sync(state.zs.data, 4); }, "format");
    state.streams[0].stride = 0;
    shader->stream_stride[0] = 16;
    backend.Draw(state, 5, 0, 3, nullptr);
    const auto fallbackLayout = service.Count(Opcode::CreatePipeline);
    shader->stream_stride[0] = 20;
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.Count(Opcode::CreatePipeline) == fallbackLayout + 1, "fallback declaration stride is deterministic pipeline layout state");
    const auto dynamicPipelines = service.Count(Opcode::CreatePipeline);
    for (uint32_t n = 0; n < 32; ++n) {
      state.rs[gpu::kRsTextureFactor] = 0xff000000 | n;
      state.simple[gpu::kMAlphaRef / 4] = n;
      state.vsc[0][0] = float(n);
      backend.Draw(state, 5, 0, 3, nullptr);
    }
    check(service.Count(Opcode::CreatePipeline) == dynamicPipelines, "idle dynamic uniform changes never grow pipeline cache");
    const auto stats = backend.Snapshot();
    check(stats.resourceCount >= 4 && stats.pipelineCount >= 5 && stats.presentedFrames == 3 && stats.submittedCommands >= stats.completedCommands, "owned backend statistics reflect actual commands");
    service.fail = true;
    rejects([&] { backend.Flush(); }, "service");
    std::puts("menu-backend-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
