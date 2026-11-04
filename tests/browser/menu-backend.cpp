#include "browser/menu-backend.h"
#include "browser/menu-shaders.h"
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
  uint32_t completedSequence = 0;
  std::function<void()> beforePresentCompletion;
  browser::webgpu::Status presentStatus = browser::webgpu::Status::Ok;
  std::vector<Command> commands;
  std::map<uint32_t, std::vector<uint8_t>> pixels;
  std::set<uint32_t> live;
  bool fail = false;
  bool failPipeline = false;
  browser::webgpu::Result Execute(browser::webgpu::Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes) {
    check(!browser::Memory::HasBorrow(), "no borrowed guest pointer during service wait");
    commands.push_back({opcode, resource, {bytes.begin(), bytes.end()}});
    if (fail) { return {browser::webgpu::Status::Lost, ++sequence, resource, {}}; }
    if (failPipeline && opcode == browser::webgpu::Opcode::CreatePipeline) {
      const std::string reason = "fixture WGSL compilation failed";
      return {browser::webgpu::Status::Error, ++sequence, resource, {reason.begin(), reason.end()}};
    }
    if (opcode == browser::webgpu::Opcode::CreateTexture || opcode == browser::webgpu::Opcode::CreatePipeline) {
      check(live.insert(resource).second, "GPU resource IDs cannot alias across backends sharing a service");
    }
    if (opcode == browser::webgpu::Opcode::Destroy) { check(live.erase(resource) == 1, "destroy actual live GPU resource"); }
    if (opcode == browser::webgpu::Opcode::CreateTexture) {
      browser::webgpu::TexturePacket packet;
      std::memcpy(&packet, bytes.data(), sizeof(packet));
      pixels[resource] = std::vector<uint8_t>(packet.width * packet.height * 4, 0);
    }
    if (opcode == browser::webgpu::Opcode::Copy) {
      browser::webgpu::CopyPacket packet;
      std::memcpy(&packet, bytes.data(), sizeof(packet));
      check(packet.source != resource, "snapshot copy cannot alias its source");
      pixels.at(resource) = pixels.at(packet.source);
    }
    const auto command = ++sequence;
    if (opcode == browser::webgpu::Opcode::Present) {
      check(live.contains(resource), "present requires a live source texture");
      if (beforePresentCompletion) { beforePresentCompletion(); }
      if (presentStatus != browser::webgpu::Status::Ok) { return {presentStatus, command, resource, {}}; }
      completedSequence = command;
    }
    if (opcode == browser::webgpu::Opcode::Flush || opcode == browser::webgpu::Opcode::Readback) {
      completedSequence = command;
    }
    if (opcode == browser::webgpu::Opcode::Readback) {
      return {browser::webgpu::Status::Ok, command, resource, pixels.at(resource)};
    }
    return {browser::webgpu::Status::Ok, command, resource, {}};
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
    state.rs[gpu::kRsStencilEnable] = state.rs[gpu::kRsZEnable] = 1;
    backend.Draw(state, 5, 0, 3, nullptr);
    webgpu::DrawPacket dormantTests{};
    std::memcpy(&dormantTests, service.commands.back().bytes.data(), sizeof(dormantTests));
    check(!dormantTests.depth_target, "absent depth/stencil target manufactured an attachment");
    state.rs[gpu::kRsStencilEnable] = state.rs[gpu::kRsZEnable] = 0;
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
    const auto samplerBaseline = service.commands.back().bytes;
    const auto samplerPipelines = service.Count(Opcode::CreatePipeline);
    state.tss[0][gpu::kTssAddressU] = state.tss[0][gpu::kTssAddressV] = 3;
    state.tss[0][gpu::kTssMipLodBias] = 0xbf000000;
    state.tss[0][gpu::kTssMaxAnisotropy] = 1;
    backend.Draw(state, 5, 0, 3, nullptr);
    webgpu::DrawPacket capturedSampler;
    std::memcpy(&capturedSampler, service.commands.back().bytes.data(), sizeof(capturedSampler));
    check(capturedSampler.sampler[0][0] == 1 && capturedSampler.sampler[0][1] == 1 &&
      capturedSampler.sampler[0][2] == 2 && capturedSampler.sampler[0][3] == 2 && capturedSampler.sampler[0][4] == 1,
      "actual one-level menu sampler retains clamp and linear filtering with finite negative bias");
    state.tss[0][gpu::kTssAddressU] = state.tss[0][gpu::kTssAddressV] = 1;
    for (uint32_t bias : {0xbf000000u, 0x3f000000u, 0x80000000u}) {
      state.tss[0][gpu::kTssMipLodBias] = bias;
      backend.Draw(state, 5, 0, 3, nullptr);
      check(service.commands.back().bytes == samplerBaseline, "one-level equal min/mag filtering is exactly invariant under finite LOD bias");
    }
    state.tss[0][gpu::kTssMipFilter] = 0;
    state.tss[0][gpu::kTssMaxMipLevel] = 7;
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.commands.back().bytes == samplerBaseline, "one-level equal min/mag filtering is invariant under disabled mip and minimum LOD clamp");
    state.tss[0][gpu::kTssMipFilter] = 1;
    state.tss[0][gpu::kTssMaxMipLevel] = 0;
    state.tss[0][gpu::kTssMipLodBias] = 0;
    state.tss[0][gpu::kTssMaxAnisotropy] = UINT32_MAX;
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.commands.back().bytes == samplerBaseline, "dormant Xbox anisotropy word does not enable anisotropic filtering");
    state.tss[0][gpu::kTssMaxAnisotropy] = 1;
    for (uint32_t bias : {0x7fc00000u, 0x7f800000u, 0xff800000u}) {
      state.tss[0][gpu::kTssMipLodBias] = bias;
      const auto beforeInvalid = service.Count(Opcode::Draw);
      rejects([&] { backend.Draw(state, 5, 0, 3, nullptr); }, "sampler");
      check(service.Count(Opcode::Draw) == beforeInvalid, "nonfinite sampler bias cannot submit a draw");
    }
    state.tss[0][gpu::kTssMipLodBias] = 0xbf000000;
    state.tss[0][gpu::kTssMinFilter] = 1;
    backend.Draw(state, 5, 0, 3, nullptr);
    webgpu::DrawPacket lodDraw{};
    std::memcpy(&lodDraw, service.commands.back().bytes.data(), sizeof(lodDraw));
    menu::ShaderConstants lodConstants{};
    std::memcpy(&lodConstants, service.commands.back().bytes.data() + sizeof(lodDraw) + lodDraw.vertex_bytes, sizeof(lodConstants));
    check(lodConstants.lod_bias[0] == -0.5f && lodConstants.texsize[0][2] == 0 && lodConstants.texsize[0][3] == 0,
      "sampler LOD bias changed UV coordinates or was ignored with unequal filters");
    state.tss[0][gpu::kTssMipLodBias] = 0;
    state.tss[0][gpu::kTssMaxMipLevel] = 1;
    backend.Draw(state, 5, 0, 3, nullptr);
    state.tss[0][gpu::kTssMaxMipLevel] = 0;
    state.tss[0][gpu::kTssMinFilter] = 3;
    rejects([&] { backend.Draw(state, 5, 0, 3, nullptr); }, "sampler filter");
    state.tss[0][gpu::kTssMinFilter] = 2;
    const uint32_t mipChain[] = {0x40001, 0x10013000, 0, 0x02220620, 0};
    image.memory->Write({}, 0x10200, mipChain, sizeof(mipChain));
    rejects([&] { backend.Draw(state, 5, 0, 3, nullptr); }, "incompatible storage GPU resource view");
    image.memory->Write({}, 0x10200, texture, sizeof(texture));
    check(service.Count(Opcode::CreatePipeline) == samplerPipelines, "one-level sampler equivalents never create new pipelines");
    const auto textureCreates = service.Count(Opcode::CreateTexture);
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.Count(Opcode::CreateTexture) == textureCreates, "unchanged guest texture reused");
    const uint32_t pixel = 0xff123456;
    image.memory->Write({}, 0x10013000, &pixel, 4);
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.Count(Opcode::CreateTexture) == textureCreates + 1 && service.Count(Opcode::Destroy) == 1, "guest texture content version changes actual GPU resource");
    state.tss[0][gpu::kTssAddressU] = 5;
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
    const std::array<int32_t, 4> copyRect{1, 1, 9, 9};
    const std::array<int32_t, 2> copyPoint{0, 2};
    image.memory->Write({}, 0x10f00, copyRect.data(), sizeof(copyRect));
    image.memory->Write({}, 0x10f20, copyPoint.data(), sizeof(copyPoint));
    backend.Copy(state.rt, destination, 1, 0x10f00, 0x10f20);
    std::array<uint32_t, 8> region{};
    check(service.commands.back().bytes.size() == sizeof(region), "partial copy must own a complete region packet");
    std::memcpy(region.data(), service.commands.back().bytes.data(), sizeof(region));
    check(region[1] == 3 && region[2] == 2 && region[3] == 1 && region[4] == 1 && region[5] == 0 && region[6] == 2,
      "partial copy failed to clip both source and destination bounds");

    backend.RecordLate(state.rt.data);
    const uint32_t incompatible = state.rt.format ^ 0x100;
    image.memory->Write({}, state.rt.header + 12, &incompatible, 4);
    image.memory->Write({}, 0x1040c, &incompatible, 4);
    rejects([&] { backend.DeliverLate(state.rt.data); }, "stale");
    image.memory->Write({}, state.rt.header, surface, sizeof(surface));
    image.memory->Write({}, 0x10400, alias, sizeof(alias));
    check(backend.DeliverLate(state.rt.data), "owned late snapshot survives header restoration");
    std::vector<MenuBackend::FrameMetrics> frameMetrics;
    backend.SetMetricsSink([&](const auto& metrics) { frameMetrics.push_back(metrics); });
    backend.Present(state.rt, true);
    webgpu::PresentPacket aspect;
    std::memcpy(&aspect, service.commands.back().bytes.data(), sizeof(aspect));
    check(aspect.pillarbox == 1, "actual aspect compositor receives pillarbox contract");
    backend.Present(state.rt, false);
    check(service.Count(Opcode::Present) == 2, "present actual target");
    check(frameMetrics.size() == 2 && frameMetrics.back().frames == 2 && frameMetrics.back().resources &&
      frameMetrics.back().commandWaitMicroseconds >= frameMetrics.back().readbackWaitMicroseconds &&
      frameMetrics.back().commandWaitMicroseconds >= frameMetrics.back().drawWaitMicroseconds,
      "completed-frame metrics omitted actual resources or separated wait counters");
    backend.SetMetricsSink({});
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
    {
      const std::array<int32_t, 8> rectangles{-1, 1, 2, 3, 3, 0, 8, 1};
      backend.Clear(state, 2, reinterpret_cast<const uint8_t*>(rectangles.data()), gpu::kClearTarget | gpu::kClearZ | gpu::kClearStencil, 0xff123456, 0.25f, 0x123);
      webgpu::ClearPacket clear{};
      const auto& command = service.commands.back();
      std::memcpy(&clear, command.bytes.data(), sizeof(clear));
      check(clear.rect_count == 2 && clear.stencil == 0x23 && command.bytes.size() == sizeof(clear) + sizeof(rectangles) &&
        std::memcmp(command.bytes.data() + sizeof(clear), rectangles.data(), sizeof(rectangles)) == 0, "partial clear lost rectangles or stencil truncation");
      auto viewportClear = state;
      viewportClear.viewport = {1, 1, 2, 2, 0, 1};
      backend.Clear(viewportClear, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      std::memcpy(&clear, service.commands.back().bytes.data(), sizeof(clear));
      const std::array<int32_t, 4> expected{1, 1, 3, 3};
      check(clear.rect_count == 1 && std::memcmp(service.commands.back().bytes.data() + sizeof(clear), expected.data(), sizeof(expected)) == 0,
        "implicit clear ignored the current viewport");
    }
    state.rs[gpu::kRsZEnable] = 1;
    state.simple[gpu::kMZFunc / 4] = 0x203;
    state.simple[gpu::kMZWriteEnable / 4] = 1;
    backend.Draw(state, 5, 0, 3, nullptr);
    rejects([&] { backend.Sync(state.zs.data, 4); }, "format");
    {
      Service stencilService;
      MenuBackend stencilBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) {
        return stencilService.Execute(opcode, resource, bytes);
      });
      stencilBackend.Init(4, 4);
      auto stencilState = state;
      stencilState.rs[gpu::kRsZEnable] = 0;
      stencilState.rs[gpu::kRsStencilEnable] = 1;
      stencilState.rs[gpu::kRsStencilFail] = 0x1e00;
      stencilState.simple[gpu::kMStencilZFail / 4] = 0x1e00;
      stencilState.simple[gpu::kMStencilPass / 4] = 0x1e01;
      stencilState.simple[gpu::kMStencilFunc / 4] = 0x207;
      stencilState.simple[gpu::kMStencilRef / 4] = 16;
      stencilState.simple[gpu::kMStencilMask / 4] = 16;
      stencilState.simple[gpu::kMStencilWriteMask / 4] = 16;
      stencilState.simple[gpu::kMColorWriteEnable / 4] = 0;
      stencilBackend.Draw(stencilState, 5, 0, 3, nullptr);
      webgpu::DrawPacket stencilDraw{};
      std::memcpy(&stencilDraw, stencilService.commands.back().bytes.data(), sizeof(stencilDraw));
      const auto pipelineCommand = std::find_if(stencilService.commands.begin(), stencilService.commands.end(), [&](const auto& command) {
        return command.opcode == Opcode::CreatePipeline && command.resource == stencilDraw.pipeline;
      });
      check(pipelineCommand != stencilService.commands.end(), "stencil draw omitted its pipeline");
      webgpu::PipelinePacket stencilPipeline{};
      std::memcpy(&stencilPipeline, pipelineCommand->bytes.data(), sizeof(stencilPipeline));
      check(stencilPipeline.depth && !stencilPipeline.depth_write && stencilPipeline.depth_compare == 8 &&
        stencilPipeline.stencil && stencilPipeline.stencil_compare == 8 && stencilPipeline.stencil_fail == 1 &&
        stencilPipeline.stencil_pass == 3 && stencilPipeline.stencil_read_mask == 16 &&
        stencilPipeline.stencil_write_mask == 16 && !stencilPipeline.write_mask && stencilDraw.stencil_reference == 16,
        "stencil-only draw changed its comparison, operations, masks or reference");
      const auto pipelineCount = stencilService.Count(Opcode::CreatePipeline);
      stencilState.simple[gpu::kMStencilRef / 4] = 4;
      stencilBackend.Draw(stencilState, 5, 0, 3, nullptr);
      std::memcpy(&stencilDraw, stencilService.commands.back().bytes.data(), sizeof(stencilDraw));
      check(stencilDraw.stencil_reference == 4 && stencilService.Count(Opcode::CreatePipeline) == pipelineCount,
        "dynamic stencil reference created a pipeline or reused stale draw state");
    }
    {
      Service batchService;
      MenuBackendLimits batchLimits;
      batchLimits.maxVertices = 24576;
      MenuBackend batchBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) {
        return batchService.Execute(opcode, resource, bytes);
      }, batchLimits);
      batchBackend.Init(4, 4);
      std::vector<uint16_t> batchIndices(18000);
      auto batchState = state;
      batchState.index_buffer = batchState.index_data = 0;
      batchState.base_vertex = 0;
      batchBackend.Draw(batchState, 5, 0, uint32_t(batchIndices.size()), reinterpret_cast<const uint8_t*>(batchIndices.data()));
      uint32_t vertices = 0;
      for (const auto& command : batchService.commands) {
        if (command.opcode != Opcode::Draw) { continue; }
        check(command.bytes.size() <= webgpu::MaxBytes, "geometry batch exceeded the mailbox");
        webgpu::DrawPacket draw{};
        std::memcpy(&draw, command.bytes.data(), sizeof(draw));
        check(draw.vertex_count % 3 == 0, "geometry batch split a triangle");
        vertices += draw.vertex_count;
      }
      check(vertices == batchIndices.size() && batchService.Count(Opcode::Draw) > 1, "geometry batches lost or duplicated vertices");
    }
    {
      Service mipService;
      MenuBackend mipBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return mipService.Execute(opcode, resource, bytes); });
      mipBackend.Init(4, 4);
      const std::array<uint32_t, 5> mipHeader{0x40001, 0x10017000, 0, (1u << 24) | (1u << 20) | (2u << 16) | 0x629, 0};
      const std::array<uint32_t, 5> mipPixels{0xffff0000, 0xffff0000, 0xffff0000, 0xffff0000, 0xff0000ff};
      image.memory->Write({}, 0x10600, mipHeader.data(), sizeof(mipHeader));
      image.memory->Map(0x10017000, 4096);
      image.memory->Commit(0x10017000, 4096);
      image.memory->Write({}, 0x10017000, mipPixels.data(), sizeof(mipPixels));
      auto mipState = state;
      mipState.textures[0] = 0x10600;
      mipState.tss[0][gpu::kTssAddressU] = mipState.tss[0][gpu::kTssAddressV] = 1;
      mipState.tss[0][gpu::kTssMagFilter] = mipState.tss[0][gpu::kTssMinFilter] = 1;
      mipState.tss[0][gpu::kTssMipFilter] = 1;
      mipState.tss[0][gpu::kTssMipLodBias] = mipState.tss[0][gpu::kTssMaxMipLevel] = 0;
      mipBackend.Draw(mipState, 5, 0, 3, nullptr);
      bool uploaded = false;
      for (const auto& command : mipService.commands) {
        if (command.opcode != Opcode::CreateTexture) { continue; }
        webgpu::TexturePacket texture{};
        std::memcpy(&texture, command.bytes.data(), sizeof(texture));
        if (texture.levels != 2) { continue; }
        check(command.bytes.size() == sizeof(texture) + 20 && command.bytes[sizeof(texture)] == 255 &&
          command.bytes[sizeof(texture) + 16 + 2] == 255, "mip levels were omitted, reordered or changed channels");
        uploaded = true;
      }
      check(uploaded, "texture mip chain was not uploaded");
    }
    {
      Service swizzleService;
      MenuBackend swizzleBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return swizzleService.Execute(opcode, resource, bytes); });
      swizzleBackend.Init(4, 4);
      image.memory->Map(0x10018000, 4096);
      image.memory->Commit(0x10018000, 4096);
      const std::array<uint32_t, 5> header{0x50001, 0x10018000, 0, 0x0223062d, 0};
      std::array<uint8_t, 128> storage{};
      std::fill(storage.begin() + 64, storage.end(), 123);
      image.memory->Write({}, header[1], storage.data(), storage.size());
      image.memory->Write({}, 0x10700, header.data(), sizeof(header));
      auto swizzleState = state;
      swizzleState.rt = {0x10700, header[1], header[3], 0};
      swizzleBackend.Clear(swizzleState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      auto& pixels = swizzleService.pixels.at(swizzleService.commands.back().resource);
      for (uint32_t n = 0; n < 16; ++n) { pixels[n * 4] = uint8_t(n); pixels[n * 4 + 1] = uint8_t(n + 1); pixels[n * 4 + 2] = uint8_t(n + 2); pixels[n * 4 + 3] = 255; }
      swizzleBackend.Sync(header[1], 64);
      const std::array<uint32_t, 16> order{0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15};
      std::array<uint8_t, 64> actual{};
      image.memory->Read({}, header[1], actual.data(), actual.size());
      for (uint32_t n = 0; n < 16; ++n) {
        check(actual[order[n] * 4] == n + 2 && actual[order[n] * 4 + 2] == n && actual[order[n] * 4 + 3] == 255,
          "swizzled render target readback lost coordinates or channels");
      }
      image.memory->Read({}, header[1], storage.data(), storage.size());
      check(std::all_of(storage.begin() + 64, storage.end(), [](uint8_t value) { return value == 123; }), "surface view overwrote another face or mip");
      image.memory->Map(0x10019000, 4096);
      image.memory->Commit(0x10019000, 4096);
      const std::array<uint32_t, 5> destination{0x50001, 0x10019000, 0, 0x1200, 0x3003};
      image.memory->Write({}, 0x10800, destination.data(), sizeof(destination));
      swizzleBackend.Copy(swizzleState.rt, {0x10800, destination[1], destination[3], destination[4]}, 0, 0, 0);
      check(swizzleService.commands.back().opcode == Opcode::Copy, "compatible linear/swizzled copy did not use GPU contents");
    }
    {
      Service compressedService;
      MenuBackend compressedBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return compressedService.Execute(opcode, resource, bytes); });
      compressedBackend.Init(4, 4);
      image.memory->Map(0x1001b000, 8192);
      image.memory->Commit(0x1001b000, 8192);
      const std::array<uint32_t, 5> source{0x50001, 0x1001b000, 0, 0x02280c29, 0};
      const std::array<uint32_t, 5> destination{0x50001, 0x1001c000, 0, 0x02280c29, 0};
      image.memory->Write({}, 0x10a00, source.data(), sizeof(source));
      image.memory->Write({}, 0x10b00, destination.data(), sizeof(destination));
      const std::array<uint8_t, 16> blocks{1, 2, 3, 4, 5, 6, 7, 8, 99, 99, 99, 99, 99, 99, 99, 99};
      std::array<uint8_t, 16> actual{};
      actual.fill(77);
      image.memory->Write({}, source[1], blocks.data(), blocks.size());
      image.memory->Write({}, destination[1], actual.data(), actual.size());
      compressedBackend.Copy({0x10a00, source[1], source[3], 0}, {0x10b00, destination[1], destination[3], 0}, 0, 0, 0);
      image.memory->Read({}, destination[1], actual.data(), actual.size());
      check(std::equal(actual.begin(), actual.begin() + 8, blocks.begin()) &&
        std::all_of(actual.begin() + 8, actual.end(), [](uint8_t value) { return value == 77; }) &&
        compressedService.Count(Opcode::Copy) == 0 && !compressedBackend.HasGpuContents(destination[1]),
        "compressed surface copy changed block bytes, neighboring mip data or GPU authority");
      for (uint32_t n = 0; n < 3; ++n) {
        auto expandedSource = source, expandedDestination = destination;
        const uint32_t format = n == 0 ? 0x0c : n == 1 ? 0x0e : 0x0f;
        expandedSource[3] = expandedDestination[3] = 0x03380029 | (format << 8);
        image.memory->Write({}, 0x10a00, expandedSource.data(), sizeof(expandedSource));
        image.memory->Write({}, 0x10b00, expandedDestination.data(), sizeof(expandedDestination));
        std::array<uint8_t, 80> sourceBytes{}, destinationBytes{};
        for (uint32_t n = 0; n < sourceBytes.size(); ++n) { sourceBytes[n] = uint8_t(n + 1); }
        destinationBytes.fill(77);
        image.memory->Write({}, source[1], sourceBytes.data(), sourceBytes.size());
        image.memory->Write({}, destination[1], destinationBytes.data(), destinationBytes.size());
        const std::array<int32_t, 4> rectangle{4, 0, 8, 4};
        const std::array<int32_t, 2> point{0, 4};
        image.memory->Write({}, 0x10f00, rectangle.data(), sizeof(rectangle));
        image.memory->Write({}, 0x10f20, point.data(), sizeof(point));
        compressedBackend.Copy({0x10a00, source[1], expandedSource[3], 0}, {0x10b00, destination[1], expandedDestination[3], 0}, 1, 0x10f00, 0x10f20);
        image.memory->Read({}, destination[1], destinationBytes.data(), destinationBytes.size());
        const uint32_t blockBytes = n == 0 ? 8 : 16;
        for (uint32_t n = 0; n < destinationBytes.size(); ++n) {
          check(destinationBytes[n] == (n >= blockBytes * 2 && n < blockBytes * 3 ? sourceBytes[n - blockBytes] : 77),
            "compressed region copy changed block rows or neighboring storage");
        }
        check(!compressedBackend.HasGpuContents(destination[1]), "CPU block region copy fabricated GPU authority");
      }

    }
    {
      Service cubeService;
      MenuBackend cubeBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return cubeService.Execute(opcode, resource, bytes); });
      cubeBackend.Init(4, 4);
      image.memory->Map(0x1001d000, 4096);
      image.memory->Commit(0x1001d000, 4096);
      const std::array<uint32_t, 5> header{0x40001, 0x1001d000, 0, 0x0111062d, 0};
      std::array<uint8_t, 768> pixels{};
      for (uint32_t n = 0; n < 6; ++n) { pixels[n * 128 + 2] = uint8_t(n + 10); pixels[n * 128 + 3] = 255; }
      image.memory->Write({}, header[1], pixels.data(), pixels.size());
      image.memory->Write({}, 0x10a00, header.data(), sizeof(header));
      auto cubeState = state;
      cubeState.textures[0] = 0x10a00;
      cubeBackend.Draw(cubeState, 5, 0, 3, nullptr);
      webgpu::DrawPacket draw{};
      std::memcpy(&draw, cubeService.commands.back().bytes.data(), sizeof(draw));
      const auto upload = std::find_if(cubeService.commands.begin(), cubeService.commands.end(), [&](const auto& command) { return command.opcode == Opcode::CreateTexture && command.resource == draw.texture[0]; });
      check(upload != cubeService.commands.end(), "cube upload missing");
      webgpu::TexturePacket texture{};
      std::memcpy(&texture, upload->bytes.data(), sizeof(texture));
      check(texture.depth == 2 && upload->bytes.size() == sizeof(texture) + 96, "cube faces were not packed");
      for (uint32_t n = 0; n < 6; ++n) { check(upload->bytes[sizeof(texture) + n * 16] == n + 10, "cube face order or padding changed"); }
      const auto pipeline = std::find_if(cubeService.commands.begin(), cubeService.commands.end(), [](const auto& command) { return command.opcode == Opcode::CreatePipeline; });
      webgpu::PipelinePacket spec{};
      std::memcpy(&spec, pipeline->bytes.data(), sizeof(spec));
      check(spec.cube_mask == 1, "cube binding dimension missing from pipeline");
    }
    {
      Service borderService;
      MenuBackend borderBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return borderService.Execute(opcode, resource, bytes); });
      borderBackend.Init(4, 4);
      auto borderState = state;
      borderState.textures[0] = 0x10200;
      borderState.tss[0][gpu::kTssAddressU] = borderState.tss[0][gpu::kTssAddressV] = 4;
      borderState.tss[0][gpu::kTssMagFilter] = borderState.tss[0][gpu::kTssMinFilter] = 2;
      borderBackend.Draw(borderState, 5, 0, 3, nullptr);
      webgpu::DrawPacket draw{};
      std::memcpy(&draw, borderService.commands.back().bytes.data(), sizeof(draw));
      check(draw.sampler[0][0] == 1 && draw.sampler[0][1] == 1, "border emulation requires bounded hardware taps");
      const auto pipeline = std::find_if(borderService.commands.begin(), borderService.commands.end(), [](const auto& command) { return command.opcode == Opcode::CreatePipeline; });
      const std::string shaders(pipeline->bytes.begin() + sizeof(webgpu::PipelinePacket), pipeline->bytes.end());
      check(shaders.find("border_sample0(") != std::string::npos, "border mode silently became edge clamp");
    }
    {
      Service shadowService;
      MenuBackend shadowBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return shadowService.Execute(opcode, resource, bytes); });
      shadowBackend.Init(4, 4);
      image.memory->Map(0x1001e000, 4096);
      image.memory->Commit(0x1001e000, 4096);
      const std::array<uint32_t, 5> color{0x50001, 0x1001e040, 0, 0x1200, surface[4]};
      const std::array<uint32_t, 5> depth{0x50001, 0x1001e000, 0, 0x2e00, surface[4]};
      image.memory->Write({}, 0x10e00, color.data(), sizeof(color));
      image.memory->Write({}, 0x10e20, depth.data(), sizeof(depth));
      auto shadowState = state;
      shadowState.rt = {0x10e00, color[1], color[3], color[4]};
      shadowState.zs = {0x10e20, depth[1], depth[3], depth[4]};
      shadowBackend.Clear(shadowState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      const auto colorId = shadowService.commands.back().resource;
      shadowBackend.Clear(shadowState, 0, nullptr, gpu::kClearZ | gpu::kClearStencil, 0, 1, 0);
      auto& pixels = shadowService.pixels.at(colorId);
      std::fill(pixels.begin(), pixels.end(), 17);
      shadowBackend.Sync(color[1], 256);
      rejects([&] { shadowBackend.Sync(depth[1], 4); }, "depth readback");
      rejects([&] { shadowBackend.Sync(color[1] + 4, 4); }, "ambiguous");
      shadowState.rt = {0x10e40, 0, 0x1200, surface[4]};
      shadowState.simple[gpu::kMColorWriteEnable / 4] = 0;
      shadowState.rs[gpu::kRsZEnable] = 1;
      shadowState.simple[gpu::kMZFunc / 4] = 0x207;
      shadowBackend.Draw(shadowState, 5, 0, 3, nullptr);
      const auto& draw = shadowService.commands.back();
      webgpu::DrawPacket packet{};
      std::memcpy(&packet, draw.bytes.data(), sizeof(packet));
      check(draw.resource == 0 && packet.depth_target, "depth-only draw manufactured a color resource");
      const auto pipeline = std::find_if(shadowService.commands.begin(), shadowService.commands.end(), [](const auto& command) { return command.opcode == Opcode::CreatePipeline; });
      webgpu::PipelinePacket spec{};
      std::memcpy(&spec, pipeline->bytes.data(), sizeof(spec));
      check(!spec.color_target, "depth-only pipeline retained a color target");
      shadowState.zscale = 65535;
      shadowState.simple[gpu::kMSolidOffsetEnable / 4] = 1;
      shadowState.simple[gpu::kMDepthOffset / 4] = std::bit_cast<uint32_t>(4.0f);
      shadowState.simple[gpu::kMSlopeScale / 4] = std::bit_cast<uint32_t>(2.0f);
      shadowBackend.Draw(shadowState, 5, 0, 3, nullptr);
      const auto& biased = shadowService.commands.back();
      webgpu::DrawPacket biasedDraw{};
      std::memcpy(&biasedDraw, biased.bytes.data(), sizeof(biasedDraw));
      menu::ShaderConstants constants{};
      std::memcpy(&constants, biased.bytes.data() + sizeof(biasedDraw) + biasedDraw.vertex_bytes, sizeof(constants));
      check(std::abs(constants.depth_bias[0] - 1024.0f / 16777215.0f) < 0.000000001f && constants.depth_bias[1] == 2,
        "depth bias did not scale D16 units to the native D24 convention");
      shadowState.rt = state.rt;
      shadowState.zs = {};
      auto sampledHeader = depth;
      sampledHeader[0] = 0x40001;
      image.memory->Write({}, 0x10e60, sampledHeader.data(), sizeof(sampledHeader));
      shadowState.textures[0] = 0x10e60;
      shadowState.rs[gpu::kRsShadowFunc] = 0x204;
      shadowState.simple[gpu::kMColorWriteEnable / 4] = 0x01010101;
      shadowState.tss[0][gpu::kTssColorOp] = 2;
      shadowState.tss[0][gpu::kTssColorArg1] = 2;
      shadowBackend.Draw(shadowState, 5, 0, 3, nullptr);
      webgpu::DrawPacket sampledDepth{};
      std::memcpy(&sampledDepth, shadowService.commands.back().bytes.data(), sizeof(sampledDepth));
      check(sampledDepth.texture[0] == packet.depth_target && !sampledDepth.depth_target,
        "sampling a completed depth target must retain its GPU contents");

    }
    {
      Service reuseService;
      MenuBackend reuseBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return reuseService.Execute(opcode, resource, bytes); });
      reuseBackend.Init(4, 4);
      image.memory->Map(0x1001f000, 4096);
      image.memory->Commit(0x1001f000, 4096);
      const std::array<uint32_t, 5> oldTexture{0x40001, 0x1001f000, 0, 0x02210629, 0};
      const std::array<uint32_t, 5> newSurface{0x50001, oldTexture[1], 0, 0x1e00, 0x3003};
      image.memory->Write({}, 0x10f60, oldTexture.data(), sizeof(oldTexture));
      image.memory->Write({}, 0x10f80, newSurface.data(), sizeof(newSurface));
      auto reuseState = state;
      reuseState.textures[0] = 0x10f60;
      reuseBackend.Draw(reuseState, 5, 0, 3, nullptr);
      reuseBackend.RecordLate(oldTexture[1]);
      reuseState.textures[0] = 0;
      reuseState.rt = {0x10f80, newSurface[1], newSurface[3], newSurface[4]};
      rejects([&] { reuseBackend.Clear(reuseState, 0, nullptr, gpu::kClearTarget, 0, 1, 0); }, "incompatible storage");
      check(reuseBackend.DeliverLate(oldTexture[1]), "owned old storage snapshot disappeared during a rejected replacement");
      const auto texturesBefore = reuseService.Count(Opcode::CreateTexture), retiredBefore = reuseService.Count(Opcode::Destroy);
      reuseBackend.Clear(reuseState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      check(reuseService.Count(Opcode::CreateTexture) == texturesBefore + 1 && reuseService.Count(Opcode::Destroy) == retiredBefore + 1,
        "new CPU-owned storage layout failed to replace exactly its old GPU cache entry");
      check(!reuseBackend.ReleaseResource(0x10f60, oldTexture[1]) && reuseBackend.Busy(0x10f80),
        "obsolete header retirement destroyed the replacement storage version");
      reuseBackend.Copy(state.rt, reuseState.rt, 0, 0, 0);
      std::array<uint32_t, 8> opaqueCopy{};
      check(reuseService.commands.back().bytes.size() == sizeof(opaqueCopy), "opaque surface copy must own its alpha conversion flag");
      std::memcpy(opaqueCopy.data(), reuseService.commands.back().bytes.data(), sizeof(opaqueCopy));
      check(opaqueCopy[7] == 1, "X8 destination copy failed to force opaque alpha");
      reuseState.rt = state.rt; reuseState.textures[0] = 0x10f60;
      rejects([&] { reuseBackend.Draw(reuseState, 5, 0, 3, nullptr); }, "incompatible storage");
    }
    {
      Service trackedService;
      MenuBackend trackedBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return trackedService.Execute(opcode, resource, bytes); });
      trackedBackend.Init(4, 4);
      const std::array<uint32_t, 5> texture{0x40001, 0x80030000, 0, 0x00010629, 0};
      image.memory->Write({}, 0x10fa0, texture.data(), sizeof(texture));
      uint32_t value = 0xff123456;
      image.memory->Write({}, texture[1], &value, 4);
      auto trackedState = state;
      trackedState.textures[0] = 0x10fa0;
      trackedBackend.Draw(trackedState, 5, 0, 3, nullptr);
      const auto uploads = trackedService.Count(Opcode::CreateTexture);
      trackedBackend.Draw(trackedState, 5, 0, 3, nullptr);
      check(trackedService.Count(Opcode::CreateTexture) == uploads, "unchanged physical texture cannot grow GPU storage");
      value = 0xffabcdef;
      image.memory->Write({}, 0xf0030000, &value, 4);
      trackedBackend.Draw(trackedState, 5, 0, 3, nullptr);
      check(trackedService.Count(Opcode::CreateTexture) == uploads + 1, "physical alias writes were hidden by the cached texture version");
      {
        auto lease = image.memory->Borrow();
        image.memory->Span({}, texture[1] + 1, 1)[0] = 17;
      }
      trackedBackend.Draw(trackedState, 5, 0, 3, nullptr);
      check(trackedService.Count(Opcode::CreateTexture) == uploads + 2, "mutable leased texture writes were hidden by the cached version");
      const auto updated = std::find_if(trackedService.commands.rbegin(), trackedService.commands.rend(), [](const auto& command) { return command.opcode == Opcode::CreateTexture; });
      const std::array<uint8_t, 4> expected{0xab, 17, 0xef, 255};
      check(std::equal(expected.begin(), expected.end(), updated->bytes.begin() + sizeof(webgpu::TexturePacket)),
        "cached physical texture upload lost changed channels");
    }
    {
      Service batchedService;
      MenuBackendLimits batchedLimits;
      batchedLimits.batchDraws = true;
      batchedLimits.maxVertices = 24576;
      MenuBackend batchedBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return batchedService.Execute(opcode, resource, bytes); }, batchedLimits);
      batchedBackend.Init(4, 4);
      batchedBackend.Draw(state, 5, 0, 3, nullptr);
      batchedBackend.Draw(state, 5, 0, 3, nullptr);
      check(batchedService.Count(Opcode::Draw) == 0 && batchedService.Count(Opcode::DrawBatch) == 0,
        "consecutive mission draws must remain owned and bounded until a submission boundary");
      const float changed = 9999;
      image.memory->Write({}, 0x10011000, &changed, 4);
      const auto fence = batchedBackend.Fence();
      check(batchedService.Count(Opcode::DrawBatch) == 1 && batchedBackend.FencePending(fence),
        "a fence must submit staged draws without inventing GPU completion");
      const auto batch = std::find_if(batchedService.commands.begin(), batchedService.commands.end(), [](const auto& command) { return command.opcode == Opcode::DrawBatch; });
      uint32_t draws = 0;
      std::memcpy(&draws, batch->bytes.data(), 4);
      float capturedPosition = 1;
      std::memcpy(&capturedPosition, batch->bytes.data() + 12 + sizeof(webgpu::DrawPacket), 4);
      check(draws == 2 && capturedPosition == 0, "staged draw payloads retained mutable guest geometry");
      const float restored = 0;
      image.memory->Write({}, 0x10011000, &restored, 4);
      batchedBackend.Present(state.rt, false);
      check(!batchedBackend.FencePending(fence), "actual Present completion failed to retire the submitted draw batch");
      const auto beforeLarge = batchedService.commands.size();
      std::vector<uint16_t> triangles(18000);
      auto largeState = state;
      largeState.index_buffer = largeState.index_data = 0;
      largeState.base_vertex = 0;
      batchedBackend.Draw(largeState, 5, 0, uint32_t(triangles.size()), reinterpret_cast<const uint8_t*>(triangles.data()));
      batchedBackend.Fence();
      uint32_t totalVertices = 0, largeBatches = 0;
      for (size_t n = beforeLarge; n < batchedService.commands.size(); ++n) {
        const auto& command = batchedService.commands[n];
        if (command.opcode != Opcode::DrawBatch) { continue; }
        ++largeBatches;
        check(command.bytes.size() <= webgpu::MaxBytes, "staged geometry exceeded the hard mailbox byte capacity");
        uint32_t count = 0;
        std::memcpy(&count, command.bytes.data(), 4);
        size_t offset = 4;
        for (uint32_t n = 0; n < count; ++n) {
          uint32_t size = 0;
          std::memcpy(&size, command.bytes.data() + offset + 4, 4);
          webgpu::DrawPacket draw{};
          std::memcpy(&draw, command.bytes.data() + offset + 8, sizeof(draw));
          check(draw.vertex_count % 3 == 0, "staged geometry split a complete primitive");
          totalVertices += draw.vertex_count;
          offset += 8 + size;
        }
        check(offset == command.bytes.size(), "staged geometry packet lengths lost their owned byte boundaries");
      }
      check(largeBatches == 2 && totalVertices == triangles.size(), "bounded draw staging lost or duplicated large geometry");
      batchedBackend.Present(state.rt, false);
      const auto beforeCapacity = batchedService.Count(Opcode::DrawBatch);
      for (uint32_t n = 0; n < 300; ++n) { batchedBackend.Draw(state, 5, 0, 3, nullptr); }
      check(batchedService.Count(Opcode::DrawBatch) == beforeCapacity + 1, "draw batches must flush at their bounded command capacity");
      check(batchedBackend.Busy(state.rt.header) && batchedService.Count(Opcode::DrawBatch) == beforeCapacity + 2,
        "resource busy checks must submit owned queued writes before inspecting pending GPU work");
      batchedBackend.Draw(state, 5, 0, 3, nullptr);
      const auto submissions = batchedService.Count(Opcode::DrawBatch);
      batchedBackend.Retire();
      check(batchedService.Count(Opcode::DrawBatch) == submissions && batchedService.live.empty(),
        "retirement must discard unsubmitted CPU draws and destroy all acknowledged GPU resources");
    }
    state.streams[0].stride = 0;
    shader->stream_stride[0] = 16;
    backend.Draw(state, 5, 0, 3, nullptr);
    const auto fallbackLayout = service.Count(Opcode::CreatePipeline);
    shader->stream_stride[0] = 20;
    backend.Draw(state, 5, 0, 3, nullptr);
    check(service.Count(Opcode::CreatePipeline) == fallbackLayout + 1, "fallback declaration stride is deterministic pipeline layout state");
    const auto dynamicPipelines = service.Count(Opcode::CreatePipeline);
    const auto shaderTranslations = backend.Snapshot().shaderTranslations;
    for (uint32_t n = 0; n < 32; ++n) {
      state.rs[gpu::kRsTextureFactor] = 0xff000000 | n;
      state.simple[gpu::kMAlphaRef / 4] = n;
      state.vsc[0][0] = float(n);
      backend.Draw(state, 5, 0, 3, nullptr);
    }
    check(service.Count(Opcode::CreatePipeline) == dynamicPipelines, "idle dynamic uniform changes never grow pipeline cache");
    check(backend.Snapshot().shaderTranslations == shaderTranslations, "uniform updates must reuse owned translated shader text");
    const auto originalProgram = shader->program;
    shader->program = {0, 0, 0, 0};
    rejects([&] { backend.Draw(state, 5, 0, 3, nullptr); }, "unsupported menu shader");
    shader->program = originalProgram;
    const auto translationsAfterFailure = backend.Snapshot().shaderTranslations;
    backend.Draw(state, 5, 0, 3, nullptr);
    check(backend.Snapshot().shaderTranslations == translationsAfterFailure, "changed shader bytes cannot poison an older cached translation");
    state.rs[gpu::kRsFogEnable] = 1;
    rejects([&] { backend.Draw(state, 5, 0, 3, nullptr); }, "fog");
    state.rs[gpu::kRsFogEnable] = 0;
    const auto alphaEnabled = state.simple[gpu::kMAlphaTestEnable / 4], alphaFunction = state.simple[gpu::kMAlphaFunc / 4];
    state.simple[gpu::kMAlphaTestEnable / 4] = 1;
    state.simple[gpu::kMAlphaFunc / 4] = 0x207;
    backend.Draw(state, 5, 0, 3, nullptr);
    state.simple[gpu::kMAlphaFunc / 4] = 0;
    rejects([&] { backend.Draw(state, 5, 0, 3, nullptr); }, "alpha comparison");
    state.simple[gpu::kMAlphaTestEnable / 4] = alphaEnabled;
    state.simple[gpu::kMAlphaFunc / 4] = alphaFunction;
    {
      Service cacheService;
      MenuBackendLimits cacheLimits;
      cacheLimits.maxPipelines = 512;
      MenuBackend cacheBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return cacheService.Execute(opcode, resource, bytes); }, cacheLimits);
      cacheBackend.Init(4, 4);
      auto cacheState = state;
      cacheState.textures[0] = 0x10200;
      cacheState.tss[0][gpu::kTssAddressU] = 4;
      for (uint32_t n = 0; n < 300; ++n) {
        cacheState.tss[0][gpu::kTssBorderColor] = n;
        cacheBackend.Draw(cacheState, 5, 0, 3, nullptr);
        const auto cached = cacheBackend.Snapshot();
        check(cached.shaderCacheEntries <= 256 && cached.shaderCacheBytes <= (16u << 20), "shader text cache exceeded its entry or byte bound");
      }
      cacheBackend.Retire();
      check(!cacheBackend.Snapshot().shaderCacheEntries && !cacheBackend.Snapshot().shaderCacheBytes,
        "retired shader translations retained their owned cache storage");
    }
    const auto stats = backend.Snapshot();
    check(stats.resourceCount >= 4 && stats.pipelineCount >= 5 && stats.presentedFrames == 3 && stats.submittedCommands >= stats.completedCommands, "owned backend statistics reflect actual commands");
    {
      Service compactService;
      MenuBackendLimits compactLimits;
      compactLimits.compactVertices = true;
      MenuBackend compactBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return compactService.Execute(opcode, resource, bytes); }, compactLimits);
      compactBackend.Init(4, 4);
      auto compactState = state;
      compactState.vs = std::make_shared<gpu::VertexShader>(*shader);
      compactState.vs->inputs[3].offset = 16;
      compactState.streams[0].stride = 20;
      compactBackend.Draw(compactState, 5, 0, 3, nullptr);
      webgpu::DrawPacket compactDraw{};
      std::memcpy(&compactDraw, compactService.commands.back().bytes.data(), sizeof(compactDraw));
      check(compactDraw.vertex_bytes == 3 * 32, "compact vertices retained unused register storage");
      const auto pipeline = std::find_if(compactService.commands.begin(), compactService.commands.end(), [](const auto& command) { return command.opcode == Opcode::CreatePipeline; });
      webgpu::PipelinePacket compactPipeline{};
      std::memcpy(&compactPipeline, pipeline->bytes.data(), sizeof(compactPipeline));
      check(compactPipeline.vertex_mask == 9, "compact vertices changed sparse shader locations");
      float color[4];
      std::memcpy(color, compactService.commands.back().bytes.data() + sizeof(compactDraw) + 16, sizeof(color));
      check(color[0] == 128 / 255.0f && color[3] == 63 / 255.0f, "compact vertex registers changed normalized component values");
      compactBackend.Retire();
      check(compactService.live.empty(), "compact vertex shutdown leaked resources");
    }
    {
      Service indexedService;
      MenuBackendLimits indexedLimits;
      indexedLimits.compactVertices = true;
      indexedLimits.indexedVertices = true;
      MenuBackend indexedBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return indexedService.Execute(opcode, resource, bytes); }, indexedLimits);
      indexedBackend.Init(4, 4);
      auto indexedState = state;
      indexedState.vs = std::make_shared<gpu::VertexShader>(*shader);
      indexedState.vs->inputs[3].offset = 16;
      indexedState.streams[0].stride = 20;
      const std::array<uint16_t, 6> repeated{2, 0, 2, 1, 0, 1};
      indexedBackend.Draw(indexedState, 5, 0, repeated.size(), reinterpret_cast<const uint8_t*>(repeated.data()));
      const auto& bytes = indexedService.commands.back().bytes;
      webgpu::DrawPacket indexedDraw{};
      std::memcpy(&indexedDraw, bytes.data(), sizeof(indexedDraw));
      check(indexedDraw.vertex_count == 3 && indexedDraw.index_count == 6 && !indexedDraw.index32 &&
        indexedDraw.vertex_bytes == 96 && indexedDraw.index_bytes == 12, "indexed draws failed to reuse converted vertices");
      std::array<uint16_t, 6> ownedOrder;
      std::memcpy(ownedOrder.data(), bytes.data() + sizeof(indexedDraw) + indexedDraw.vertex_bytes, sizeof(ownedOrder));
      check(ownedOrder == std::array<uint16_t, 6>{0, 1, 0, 2, 1, 2}, "indexed uploads changed primitive order or index identity");
      float firstY = 0;
      std::memcpy(&firstY, bytes.data() + sizeof(indexedDraw) + 4, 4);
      check(firstY == 4, "indexed upload reused a different source vertex");
      const auto owned = bytes;
      const uint32_t changed = 0xffffffff;
      image.memory->Write({}, 0x10011000, &changed, 4);
      check(indexedService.commands.back().bytes == owned, "indexed upload retained mutable guest geometry");
      image.memory->Write({}, 0x10011000, vertices, sizeof(vertices));
      indexedBackend.Retire();
      check(indexedService.live.empty(), "indexed upload shutdown leaked resources");
    }
    {
      Service diagnosticService;
      MenuBackend diagnosticBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return diagnosticService.Execute(opcode, resource, bytes); });
      diagnosticBackend.Init(4, 4);
      diagnosticBackend.Clear(state, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      const uint32_t mismatchedView[] = {0x50001, surface[1], 0, 0x1e00, 1 | (3 << 12)};
      image.memory->Write({}, 0x10d00, mismatchedView, sizeof(mismatchedView));
      const gpu::Surface mismatched{0x10d00, mismatchedView[1], mismatchedView[3], mismatchedView[4]};
      const auto before = diagnosticBackend.Snapshot();
      const auto commandsBefore = diagnosticService.commands.size();
      const std::string expected = "menu backend unsupported incompatible storage GPU resource view requested{header=0x10d00 common=0x50001 data=0x10012000 format=0x1e00 size=0x3001 width=2 height=4 pitch=64 levels=1 cube=0 linear=1 depth=0 bytes=256} existing{header=0x10100 common=0x50001 data=0x10012000 format=0x1200 size=0x3003 width=4 height=4 pitch=64 levels=1 cube=0 linear=1 depth=0 bytes=256 views=1 version=2 dirty=1}";
      rejects([&] { diagnosticBackend.Present(mismatched, false); }, expected.c_str());
      const auto after = diagnosticBackend.Snapshot();
      check(after.storageViewFailure == expected && before.storageViewFailure.empty(), "bounded owned incompatible descriptor survives guest fault formatting truncation");
      check(diagnosticService.commands.size() == commandsBefore && before.resourceCount == after.resourceCount && before.residentBytes == after.residentBytes,
        "incompatible storage diagnostic cannot submit commands, observe a new alias or mutate resident storage");
      diagnosticBackend.Present(state.rt, false);
      check(diagnosticBackend.ReleaseResource(state.rt.header, state.rt.data), "rejected alias cannot retain physical storage lifetime");
    }
    {
      Service dormantService;
      MenuBackend dormantBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return dormantService.Execute(opcode, resource, bytes); });
      dormantBackend.Init(4, 4);
      auto dormantState = state;
      dormantState.vs_handle = 0x44;
      dormantState.vs.reset();
      dormantState.streams[0].stride = 20;
      dormantState.zs = {};
      dormantState.rs[gpu::kRsZEnable] = 0;
      dormantState.textures[0] = 0x10200;
      dormantBackend.Draw(dormantState, 5, 0, 3, nullptr);
      webgpu::DrawPacket initial;
      std::memcpy(&initial, dormantService.commands.back().bytes.data(), sizeof(initial));
      const auto before = dormantBackend.Snapshot();
      const auto initialTarget = dormantService.commands.back().resource;
      const uint32_t dormantDepth[] = {0x50001, texture[1], 0, 0x2e00, texture[4]};
      image.memory->Write({}, 0x10d00, dormantDepth, sizeof(dormantDepth));
      dormantState.zs = {0x10d00, dormantDepth[1], dormantDepth[3], dormantDepth[4]};
      dormantBackend.Draw(dormantState, 5, 0, 3, nullptr);
      webgpu::DrawPacket actual;
      std::memcpy(&actual, dormantService.commands.back().bytes.data(), sizeof(actual));
      check(!actual.depth_target && actual.texture[0] == initial.texture[0] && dormantService.commands.back().resource == initialTarget,
        "disabled depth testing does not bind or reinterpret a dormant depth header over live color texture storage");
      for (uint32_t n = 0; n < 128; ++n) {
        dormantState.zs = n % 2 ? gpu::Surface{0xdead0000, 0xdead1000, 0x3000, 0} : gpu::Surface{};
        dormantBackend.Draw(dormantState, 5, 0, 3, nullptr);
      }
      const auto after = dormantBackend.Snapshot();
      check(after.resourceCount == before.resourceCount && after.residentBytes == before.residentBytes && after.pipelineCount == before.pipelineCount,
        "unconsumed depth headers do not read guest storage or grow resources, resident bytes or pipelines");
      auto& gpuPixels = dormantService.pixels.at(initial.texture[0]);
      gpuPixels[0] = 0x12; gpuPixels[1] = 0x34; gpuPixels[2] = 0x56; gpuPixels[3] = 0x78;
      dormantBackend.Sync(texture[1], 4);
      uint32_t readPixel = 0;
      image.memory->Read({}, texture[1], &readPixel, 4);
      check(readPixel == 0x78123456, "dormant depth binding preserves honest color texture mapping and GPU readback");
      check(dormantBackend.ReleaseResource(0x10200, texture[1]), "dormant depth header and rejected active depth view cannot acquire physical alias lifetime");
    }
    {
      Service aliasService;
      MenuBackend aliases(runtime, image, [&](auto opcode, auto resource, auto bytes) { return aliasService.Execute(opcode, resource, bytes); });
      aliases.Init(4, 4);
      auto aliasState = state;
      aliasState.vs_handle = 0x44;
      aliasState.vs.reset();
      aliasState.streams[0].stride = 20;
      aliasState.textures[0] = 0;
      const uint32_t depthAlias[] = {0x50001, state.rt.data, 0, 0x2e00, state.rt.size};
      image.memory->Write({}, 0x10d00, depthAlias, sizeof(depthAlias));
      aliasState.zs = {0x10d00, depthAlias[1], depthAlias[3], depthAlias[4]};
      aliases.Clear(aliasState, 0, nullptr, gpu::kClearTarget | gpu::kClearZ | gpu::kClearStencil, 0, 1, 0);
      webgpu::ClearPacket clear;
      std::memcpy(&clear, aliasService.commands.back().bytes.data(), sizeof(clear));
      const uint32_t color = aliasService.commands.back().resource;
      check(color && clear.depth_target && color != clear.depth_target, "same-address color and depth targets did not retain distinct typed resources");
      aliasState.rs[gpu::kRsZEnable] = 1;
      aliases.Draw(aliasState, 5, 0, 3, nullptr);
      webgpu::DrawPacket draw;
      std::memcpy(&draw, aliasService.commands.back().bytes.data(), sizeof(draw));
      check(draw.depth_target == clear.depth_target && aliasService.commands.back().resource == color, "aliased depth draw changed the color resource identity");
      auto& pixels = aliasService.pixels.at(color);
      pixels[0] = 0x12; pixels[1] = 0x34; pixels[2] = 0x56; pixels[3] = 0x78;
      const uint32_t reusedHeaderFormat = state.rt.format ^ 0x100;
      image.memory->Write({}, state.rt.header + 12, &reusedHeaderFormat, 4);
      aliases.Sync(state.rt.data, 4);
      uint32_t readPixel = 0;
      image.memory->Read({}, state.rt.data, &readPixel, 4);
      check(readPixel == 0x78123456, "raw color readback selected an aliased depth target");
      image.memory->Write({}, state.rt.header + 12, &state.rt.format, 4);
      aliases.Draw(aliasState, 5, 0, 3, nullptr);
      const auto before = aliases.Snapshot();
      const uint32_t aliasedCpuPixel = 0xff765432;
      image.memory->Write({}, state.rt.data, &aliasedCpuPixel, 4);
      const auto depthCreates = aliasService.Count(Opcode::CreateTexture);
      aliases.Clear(aliasState, 0, nullptr, gpu::kClearZ | gpu::kClearStencil, 0, 0.5f, 3);
      webgpu::ClearPacket repeatedDepth;
      std::memcpy(&repeatedDepth, aliasService.commands.back().bytes.data(), sizeof(repeatedDepth));
      check(repeatedDepth.depth_target == clear.depth_target && aliasService.Count(Opcode::CreateTexture) == depthCreates,
        "CPU color alias writes replaced independent GPU depth contents");
      check(!aliases.ReleaseResource(aliasState.zs.header, aliasState.zs.data), "depth release retired still-live color storage");
      check(aliases.Snapshot().resourceCount + 1 == before.resourceCount, "depth alias release retained its resource");
      check(aliases.ReleaseResource(state.rt.header, state.rt.data), "final color release did not retire shared-address storage");
    }
    {
      Service resourceService;
      MenuBackend resources(runtime, image, [&](auto opcode, auto resource, auto bytes) { return resourceService.Execute(opcode, resource, bytes); });
      resources.Init(4, 4);
      auto resourceState = state;
      resourceState.zs = {};
      resourceState.rs[gpu::kRsZEnable] = 0;
      resourceState.vs_handle = 0x44;
      resourceState.vs.reset();
      resourceState.streams[0].stride = 20;
      resources.Clear(resourceState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      const auto initialTarget = resourceService.commands.back().resource;
      image.memory->Write({}, resourceState.rt.data, &pixel, 4);
      resources.Draw(resourceState, 5, 0, 3, nullptr);
      check(resourceService.commands.back().resource != initialTarget, "CPU writes to a GPU-dirty target replace stale GPU content");
      const auto targetUpload = std::find_if(resourceService.commands.rbegin(), resourceService.commands.rend(), [&](const auto& command) {
        return command.opcode == Opcode::CreateTexture && command.resource == resourceService.commands.back().resource;
      });
      check(targetUpload != resourceService.commands.rend() && targetUpload->bytes[sizeof(webgpu::TexturePacket)] == 0x12 &&
        targetUpload->bytes[sizeof(webgpu::TexturePacket) + 2] == 0x56, "dirty target replacement uploads actual decoded CPU pixels");
      const uint32_t textureAlias[] = {0x40001, texture[1], 0, texture[3], texture[4]};
      image.memory->Write({}, 0x10600, textureAlias, sizeof(textureAlias));
      resourceState.textures[0] = 0x10200;
      resourceState.textures[1] = 0x10600;
      resourceState.tss[1] = resourceState.tss[0];
      resources.Draw(resourceState, 5, 0, 3, nullptr);
      webgpu::DrawPacket aliasedTextures;
      std::memcpy(&aliasedTextures, resourceService.commands.back().bytes.data(), sizeof(aliasedTextures));
      check(aliasedTextures.texture[0] == aliasedTextures.texture[1], "distinct texture headers share one physical GPU storage snapshot");
      resourceState.textures = {};
      auto sharedLayout = std::make_shared<gpu::VertexShader>();
      sharedLayout->inputs[0] = {0, 0, 0x32};
      sharedLayout->inputs[3] = {1, 0, 0x40};
      resourceState.vs = sharedLayout;
      resourceState.vs_handle = 1;
      resourceState.streams[0].stride = 12;
      resourceState.streams[1] = {resourceState.streams[0].vb, 32};
      resources.Draw(resourceState, 5, 0, 3, nullptr);
      check(resourceService.commands.back().opcode == Opcode::Draw, "shared vertex buffer snapshots cover every bound stream stride");
      check(!resources.Busy(resourceState.streams[0].vb), "deep-owned consumed vertex buffers do not remain busy or become unknown resources");
      resources.WaitResource(resourceState.streams[0].vb);
      const uint32_t padding = 0x76543210;
      image.memory->Write({}, resourceState.rt.data + 20, &padding, 4);
      const auto drawnTarget = resourceService.commands.back().resource;
      auto& targetPixels = resourceService.pixels.at(drawnTarget);
      for (uint32_t n = 0; n < targetPixels.size(); ++n) { targetPixels[n] = uint8_t(n + 1); }
      image.memory->Write({}, resourceState.rt.data, &pixel, 4);
      resources.Sync(resourceState.rt.data + 12, 56);
      uint32_t partialPixel = 0;
      image.memory->Read({}, resourceState.rt.data + 12, &partialPixel, 4);
      check(partialPixel == 0x100d0e0f, "partial readback converts actual RGBA bytes at requested row suffix");
      image.memory->Read({}, resourceState.rt.data + 64, &partialPixel, 4);
      check(partialPixel == 0x14111213, "partial readback crosses padded rows into the next requested pixel");
      image.memory->Read({}, resourceState.rt.data, &partialPixel, 4);
      check(partialPixel == pixel, "partial readback preserves active pixels outside the requested range");
      resources.Sync(resourceState.rt.data, 0);
      uint32_t syncedPadding = 0;
      image.memory->Read({}, resourceState.rt.data + 20, &syncedPadding, 4);
      check(syncedPadding == padding, "whole target readback preserves guest row padding");
      resources.RecordLate(resourceState.rt.data);
      const uint32_t latePadding = 0x10293847;
      image.memory->Write({}, resourceState.rt.data + 20, &latePadding, 4);
      check(resources.DeliverLate(resourceState.rt.data), "late target readback delivered");
      image.memory->Read({}, resourceState.rt.data + 20, &syncedPadding, 4);
      check(syncedPadding == latePadding, "late readback preserves padding written after capture");
      image.memory->Commit(0x80020000, 4096);
      const uint32_t physicalSurface[] = {0x50001, 0x20000, 0, surface[3], surface[4]};
      image.memory->Write({}, 0x10700, physicalSurface, sizeof(physicalSurface));
      resourceState.rt = {0x10700, 0x80020000, surface[3], surface[4]};
      resources.Clear(resourceState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      const auto physicalId = resourceService.commands.back().resource;
      const uint32_t combined = 0xf0020000;
      image.memory->Write({}, resourceState.rt.header + 4, &combined, 4);
      resourceState.rt.data = combined;
      resources.Present(resourceState.rt, false);
      check(resourceService.commands.back().resource == physicalId, "write-combined alias preserves actual physical GPU storage");
      resources.Sync(combined, 0);
      const uint32_t physicalIndices[] = {0x10001, 0x21000, 0};
      image.memory->Commit(0x80021000, 4096);
      image.memory->Write({}, 0x80021000, triangleOrder, sizeof(triangleOrder));
      image.memory->Write({}, 0x10800, physicalIndices, sizeof(physicalIndices));
      resourceState.index_buffer = 0x10800;
      resourceState.index_data = physicalIndices[1];
      resources.Draw(resourceState, 5, 0, 3, reinterpret_cast<const uint8_t*>(triangleOrder));
      check(resourceService.commands.back().opcode == Opcode::Draw, "native raw physical index binding matches owned virtual payload");
      resourceState.index_data = 0xf0021000;
      resources.Draw(resourceState, 5, 0, 3, reinterpret_cast<const uint8_t*>(triangleOrder));
      check(resourceService.commands.back().opcode == Opcode::Draw, "write-combined index binding matches owned canonical payload");
      resourceState.index_buffer = 0;
      resourceState.index_data = physicalIndices[1];
      resources.Draw(resourceState, 5, 0, 3, reinterpret_cast<const uint8_t*>(triangleOrder));
      check(resourceService.commands.back().opcode == Opcode::Draw, "raw physical index data without a buffer header is captured through its mapped alias");
      resourceState.zs = state.zs;
      const uint32_t initialDepth = ~pixel;
      image.memory->Write({}, resourceState.zs.data, &initialDepth, 4);
      const uint32_t secondDepthRow = 0x123456a5, depthPadding = 0x4567896d;
      image.memory->Write({}, resourceState.zs.data + 64, &secondDepthRow, 4);
      image.memory->Write({}, resourceState.zs.data + 16, &depthPadding, 4);
      resourceState.rs[gpu::kRsZEnable] = 1;
      const auto initializedDraws = resourceService.Count(Opcode::Draw);
      resources.Draw(resourceState, 5, 0, 3, nullptr);
      check(resourceService.Count(Opcode::Draw) == initializedDraws + 1 && resourceService.Count(Opcode::UploadDepth) == 1,
        "nonuniform guest depth was not imported before its first draw");
      const auto upload = std::find_if(resourceService.commands.begin(), resourceService.commands.end(), [](const auto& command) {
        return command.opcode == Opcode::UploadDepth;
      });
      webgpu::DepthUploadPacket importedSource{};
      std::memcpy(&importedSource, upload->bytes.data(), sizeof(importedSource));
      std::array<uint32_t, 16> importedPixels{}, expectedPixels{};
      expectedPixels[0] = initialDepth;
      expectedPixels[4] = secondDepthRow;
      std::memcpy(importedPixels.data(), upload->bytes.data() + sizeof(importedSource), sizeof(importedPixels));
      check(importedSource.width == 4 && importedSource.height == 4 && importedSource.depth_bits == 24 &&
        upload->bytes.size() == sizeof(importedSource) + 64 && importedPixels == expectedPixels &&
        (importedSource.stencil_mask[(initialDepth & 255) / 32] & (1u << (initialDepth & 31))),
        "pitched guest depth import changed pixel bits, dimensions or stencil selection");
      resources.Clear(resourceState, 0, nullptr, gpu::kClearZ, 0, 1, 0);
      resources.Draw(resourceState, 5, 0, 3, nullptr);
      check(resourceService.Count(Opcode::Draw) == initializedDraws + 2, "genuine full GPU depth clear authorizes depth-tested draws");
      image.memory->Write({}, resourceState.zs.data, &pixel, 4);
      rejects([&] { resources.Draw(resourceState, 5, 0, 3, nullptr); }, "CPU depth surface upload");
      std::array<uint32_t, 64> uniformDepth;
      uniformDepth.fill(0x8000007f);
      image.memory->Write({}, resourceState.zs.data, uniformDepth.data(), sizeof(uniformDepth));
      Service uniformService;
      MenuBackend uniformBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) {
        return uniformService.Execute(opcode, resource, bytes);
      });
      uniformBackend.Init(4, 4);
      uniformBackend.Draw(resourceState, 5, 0, 3, nullptr);
      const auto imported = std::find_if(uniformService.commands.begin(), uniformService.commands.end(), [](const auto& command) {
        return command.opcode == Opcode::Clear;
      });
      check(imported != uniformService.commands.end(), "uniform guest depth bytes were not imported before drawing");
      webgpu::ClearPacket importedDepth{};
      std::memcpy(&importedDepth, imported->bytes.data(), sizeof(importedDepth));
      check(importedDepth.flags == 6 && importedDepth.depth_target && importedDepth.stencil == 127 &&
        std::abs(importedDepth.depth - 0.5f) < 0.000001f,
        "uniform D24S8 import changed real guest depth or stencil contents");
    }
    {
      Service feedbackService;
      const auto exercise = [&](MenuBackendLimits feedbackLimits) {
        MenuBackend feedbackBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return feedbackService.Execute(opcode, resource, bytes); }, feedbackLimits);
        feedbackBackend.Init(4, 4);
        gpu::State feedbackState;
        feedbackState.vs_handle = 0x44;
        feedbackState.streams[0] = {0x10000, 20};
        feedbackState.rt = {0x10100, surface[1], surface[3], surface[4]};
        feedbackState.viewport.w = feedbackState.viewport.h = 4;
        feedbackState.simple[gpu::kMColorWriteEnable / 4] = 0x01010101;
        feedbackState.tss[0][gpu::kTssColorOp] = 1;
        feedbackBackend.Clear(feedbackState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
        const auto sourceId = feedbackService.commands.back().resource;
        auto& sourcePixels = feedbackService.pixels.at(sourceId);
        for (uint32_t n = 0; n < sourcePixels.size(); ++n) { sourcePixels[n] = uint8_t(n + 7); }
        const uint32_t feedbackHeader[] = {0x40001, surface[1], 0, surface[3] | 8, surface[4]};
        image.memory->Write({}, 0x10a00, feedbackHeader, sizeof(feedbackHeader));
        image.memory->Write({}, 0x10b00, feedbackHeader, sizeof(feedbackHeader));
        for (uint32_t n = 0; n < 2; ++n) {
          feedbackState.textures[n] = 0x10a00 + n * 0x100;
          feedbackState.tss[n][gpu::kTssAddressU] = feedbackState.tss[n][gpu::kTssAddressV] = 3;
          feedbackState.tss[n][gpu::kTssMagFilter] = feedbackState.tss[n][gpu::kTssMinFilter] = 2;
          feedbackState.tss[n][gpu::kTssMipFilter] = 1;
        }
        feedbackBackend.Draw(feedbackState, 5, 0, 3, nullptr);
        webgpu::DrawPacket feedbackPacket;
        std::memcpy(&feedbackPacket, feedbackService.commands.back().bytes.data(), sizeof(feedbackPacket));
        check(feedbackPacket.texture[0] != sourceId && feedbackPacket.texture[0] == feedbackPacket.texture[1],
          "feedback aliases bind one actual scratch texture rather than the active render target");
        check(feedbackService.Count(Opcode::Copy) == 1 && feedbackService.pixels.at(feedbackPacket.texture[0]) == sourcePixels,
          "first feedback draw copies current GPU contents exactly once per physical source");
        const auto copy = std::find_if(feedbackService.commands.begin(), feedbackService.commands.end(), [](const auto& command) { return command.opcode == Opcode::Copy; });
        webgpu::CopyPacket feedbackCopy;
        std::memcpy(&feedbackCopy, copy->bytes.data(), sizeof(feedbackCopy));
        check(feedbackCopy.source == sourceId && copy->resource == feedbackPacket.texture[0] && copy < feedbackService.commands.end() - 1,
          "GPU feedback copy precedes draw and names the current target storage as its source");
        const auto feedbackStats = feedbackBackend.Snapshot();
        const auto liveFeedback = feedbackService.live.size();
        const auto feedbackCreates = feedbackService.Count(Opcode::CreateTexture);
        for (uint32_t n = 0; n < 32; ++n) {
          sourcePixels[0] = uint8_t(n + 91);
          feedbackBackend.Draw(feedbackState, 5, 0, 3, nullptr);
          check(feedbackService.pixels.at(feedbackPacket.texture[0])[0] == sourcePixels[0], "every feedback draw snapshots latest GPU writes instead of guest CPU bytes or stale scratch");
        }
        const auto repeatedStats = feedbackBackend.Snapshot();
        check(feedbackService.Count(Opcode::Copy) == 33 && feedbackService.Count(Opcode::CreateTexture) == feedbackCreates &&
          feedbackService.live.size() == liveFeedback && repeatedStats.resourceCount == feedbackStats.resourceCount &&
          repeatedStats.residentBytes == feedbackStats.residentBytes && repeatedStats.pipelineCount == feedbackStats.pipelineCount,
          "repeated feedback draws reuse bounded scratch storage without live resource, byte or pipeline growth");
        check(feedbackStats.resourceCount == 3 && feedbackStats.residentBytes == 392, "feedback scratch participates in resource and resident byte statistics");
        image.memory->Write({}, surface[1], &pixel, 4);
        feedbackBackend.Draw(feedbackState, 5, 0, 3, nullptr);
        check(!feedbackService.live.contains(sourceId) && !feedbackService.live.contains(feedbackPacket.texture[0]),
          "CPU replacement retires both prior target and its feedback scratch");
        std::memcpy(&feedbackPacket, feedbackService.commands.back().bytes.data(), sizeof(feedbackPacket));
        const auto replacementId = feedbackService.commands.back().resource;
        feedbackBackend.RecordLate(surface[1]);
        check(!feedbackBackend.ReleaseResource(0x10a00, surface[1]) && !feedbackBackend.ReleaseResource(surface[0], surface[1]),
          "unrelated final header does not retire live storage aliases");
        check(!feedbackBackend.ReleaseResource(0x10100, surface[1]) && feedbackService.live.contains(replacementId),
          "final surface header release preserves still-live texture alias storage");
        check(feedbackBackend.ReleaseResource(0x10b00, surface[1]), "last certified final header release retires physical GPU storage");
        check(!feedbackService.live.contains(replacementId) && !feedbackService.live.contains(feedbackPacket.texture[0]) &&
          feedbackBackend.Snapshot().resourceCount == 1 && feedbackBackend.Snapshot().residentBytes == 8,
          "last alias retirement destroys target and feedback scratch and debits owned and delayed snapshot bytes");
        check(!feedbackBackend.ReleaseResource(0x10b00, surface[1]), "repeated final release cannot destroy a GPU resource twice");
        check(!feedbackBackend.ReleaseResource(0x10000, vb[1]), "consumed upload metadata retirement is not a physical GPU retirement");
        rejects([&] { feedbackBackend.Busy(0x10000); }, "unknown resource");
        auto resizedState = feedbackState;
        resizedState.textures = {};
        resizedState.rt.size = 1 | (1 << 12);
        const uint32_t resizedSurface[] = {0x50001, surface[1], 0, surface[3], resizedState.rt.size};
        image.memory->Write({}, resizedState.rt.header, resizedSurface, sizeof(resizedSurface));
        feedbackBackend.Clear(resizedState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
        check(feedbackBackend.ReleaseResource(resizedState.rt.header, surface[1]), "certified retired physical allocation can be reused with a different resource layout");
        image.memory->Write({}, 0x10100, surface, sizeof(surface));
      };
      exercise({});
      check(feedbackService.live.empty(), "backend teardown destroys feedback scratch with its owner");
    }
    {
      Service swapService;
      MenuBackend swapBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return swapService.Execute(opcode, resource, bytes); });
      swapBackend.Init(4, 4);
      uint32_t firstView[] = {0x50001, 0x10013000, 0, surface[3], surface[4]};
      uint32_t secondView[] = {0x50001, 0x10014000, 0, surface[3], surface[4]};
      image.memory->Write({}, 0x10e00, firstView, sizeof(firstView));
      image.memory->Write({}, 0x10f00, secondView, sizeof(secondView));
      gpu::State swapState;
      swapState.rt = {0x10e00, firstView[1], firstView[3], firstView[4]};
      swapBackend.Clear(swapState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      swapState.rt = {0x10f00, secondView[1], secondView[3], secondView[4]};
      swapBackend.Clear(swapState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      std::swap(firstView[1], secondView[1]);
      image.memory->Write({}, 0x10e00, firstView, sizeof(firstView));
      image.memory->Write({}, 0x10f00, secondView, sizeof(secondView));
      swapState.rt = {0x10e00, firstView[1], firstView[3], firstView[4]};
      swapBackend.Clear(swapState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      check(swapService.Count(Opcode::Destroy) == 0 && swapBackend.Snapshot().resourceCount == 2,
        "moving header view never destroys temporarily unobserved backbuffer storage");
      swapState.rt = {0x10f00, secondView[1], secondView[3], secondView[4]};
      swapBackend.Clear(swapState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      check(swapBackend.ReleaseResource(0x10e00, firstView[1]) && swapBackend.Snapshot().resourceCount == 1,
        "swapped header final release retires only its current physical owner without stale alias membership");
      check(swapBackend.ReleaseResource(0x10f00, secondView[1]) && swapBackend.Snapshot().resourceCount == 0 && swapBackend.Snapshot().residentBytes == 0,
        "both swapped final releases retire both physical owners and restore resident budget");
    }
    image.memory->Map(0x10200000, 65536);
    image.memory->Commit(0x10200000, 65536);
    const uint32_t feedbackBudgetSurface[] = {0x50001, 0x10200000, 0, 0x1200, 127 | (127 << 12) | (7 << 24)};
    const uint32_t feedbackBudgetTexture[] = {0x40001, 0x10200000, 0, 0x1200, feedbackBudgetSurface[4]};
    image.memory->Write({}, 0x10c00, feedbackBudgetSurface, sizeof(feedbackBudgetSurface));
    image.memory->Write({}, 0x10d00, feedbackBudgetTexture, sizeof(feedbackBudgetTexture));
    for (uint32_t n = 0; n < 2; ++n) {
      Service feedbackBudgetService;
      MenuBackendLimits feedbackLimits;
      if (n == 0) { feedbackLimits.maxResources = 2; }
      else { feedbackLimits.maxResidentBytes = 196615; }
      MenuBackend feedbackBounded(runtime, image, [&](auto opcode, auto resource, auto bytes) { return feedbackBudgetService.Execute(opcode, resource, bytes); }, feedbackLimits);
      feedbackBounded.Init(4, 4);
      gpu::State feedbackState;
      feedbackState.vs_handle = 0x44;
      feedbackState.streams[0] = {0x10000, 20};
      feedbackState.rt = {0x10c00, feedbackBudgetSurface[1], feedbackBudgetSurface[3], feedbackBudgetSurface[4]};
      feedbackState.viewport.w = feedbackState.viewport.h = 128;
      feedbackState.tss[0][gpu::kTssColorOp] = 1;
      feedbackBounded.Clear(feedbackState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      feedbackBounded.Draw(feedbackState, 5, 0, 3, nullptr);
      feedbackState.textures[0] = 0x10d00;
      rejects([&] { feedbackBounded.Draw(feedbackState, 5, 0, 3, nullptr); }, "feedback");
      check(feedbackBudgetService.Count(Opcode::Copy) == 0 && feedbackBudgetService.Count(Opcode::Draw) == 1 &&
        feedbackBounded.Snapshot().resourceCount == 2 && feedbackBounded.Snapshot().residentBytes == 131080,
        "feedback budgets refuse before creating scratch, copying or drawing");
    }
    {
      Service viewportService;
      MenuBackend viewportBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return viewportService.Execute(opcode, resource, bytes); });
      viewportBackend.Init(640, 480);
      image.memory->Map(0x10020000, 0x12c000);
      image.memory->Commit(0x10020000, 0x12c000);
      const uint32_t viewportTarget[] = {0x50001, 0x10020000, 0, 0x1200, 639 | (479 << 12) | (39 << 24)};
      image.memory->Write({}, 0x10900, viewportTarget, sizeof(viewportTarget));
      gpu::State viewportState;
      viewportState.vs_handle = 0x44;
      viewportState.streams[0] = {0x10000, 20};
      viewportState.rt = {0x10900, viewportTarget[1], viewportTarget[3], viewportTarget[4]};
      viewportState.viewport = {160, 120, 320, 240, 0.25f, 0.75f};
      viewportState.tss[0][gpu::kTssColorOp] = 1;
      viewportState.simple[gpu::kMColorWriteEnable / 4] = 0x01010101;
      viewportService.failPipeline = true;
      std::string pipelineFailure;
      try { viewportBackend.Draw(viewportState, 5, 0, 3, nullptr); }
      catch (const std::exception& error) { pipelineFailure = error.what(); }
      check(pipelineFailure.find("fixture WGSL compilation failed") != std::string::npos &&
        pipelineFailure.find("state: ") != std::string::npos && pipelineFailure.find("vertex WGSL:\n") != std::string::npos &&
        pipelineFailure.find("fragment WGSL:\n") != std::string::npos && pipelineFailure.find("vs_main") != std::string::npos &&
        pipelineFailure.find("fs_main") != std::string::npos,
        "failed pipeline preserves actual service error with concise shader state and both generated WGSL sources");
      check(viewportBackend.Snapshot().pipelineCount == 0 && viewportService.Count(Opcode::Draw) == 0,
        "failed pipeline is never cached or submitted as a successful draw");
      viewportService.failPipeline = false;
      viewportBackend.Draw(viewportState, 5, 0, 3, nullptr);
      check(viewportBackend.Snapshot().pipelineCount == 1 && viewportService.Count(Opcode::CreatePipeline) == 2,
        "retry after failed compilation creates and caches a genuine new pipeline");
      webgpu::DrawPacket viewportPacket;
      std::memcpy(&viewportPacket, viewportService.commands.back().bytes.data(), sizeof(viewportPacket));
      check(viewportPacket.viewport[0] == 0 && viewportPacket.viewport[1] == 0 &&
        viewportPacket.viewport[2] == 640 && viewportPacket.viewport[3] == 480 &&
        viewportPacket.viewport[4] == 0 && viewportPacket.viewport[5] == 1,
        "guest viewport already embedded in shader is not applied twice by hardware viewport");
      check(viewportPacket.scissor[0] == 160 && viewportPacket.scissor[1] == 120 &&
        viewportPacket.scissor[2] == 320 && viewportPacket.scissor[3] == 240,
        "guest viewport clips full-target hardware viewport through scissor");
      viewportState.scissor_count = 1;
      viewportState.scissors = {100, 200, 400, 400};
      viewportBackend.Draw(viewportState, 5, 0, 3, nullptr);
      std::memcpy(&viewportPacket, viewportService.commands.back().bytes.data(), sizeof(viewportPacket));
      check(viewportPacket.scissor[0] == 160 && viewportPacket.scissor[1] == 200 &&
        viewportPacket.scissor[2] == 240 && viewportPacket.scissor[3] == 160,
        "guest scissor intersects rather than replaces guest viewport clip");
      viewportState.scissors = {500, 400, 600, 450};
      viewportBackend.Draw(viewportState, 5, 0, 3, nullptr);
      std::memcpy(&viewportPacket, viewportService.commands.back().bytes.data(), sizeof(viewportPacket));
      check(viewportPacket.scissor[2] == 0 && viewportPacket.scissor[3] == 0,
        "disjoint viewport and scissor produce an empty clip without unsigned underflow");
    }
    {
      Service completionService;
      MenuBackend completionBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) { return completionService.Execute(opcode, resource, bytes); });
      completionBackend.Init(4, 4);
      gpu::State completionState;
      completionState.rt = {0x10100, surface[1], surface[3], surface[4]};
      completionBackend.Clear(completionState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      const auto firstFence = completionBackend.Fence();
      const auto sameCommandFence = completionBackend.Fence();
      completionState.rt = destination;
      completionBackend.Clear(completionState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      const auto secondFence = completionBackend.Fence();
      const auto outstanding = completionBackend.Snapshot();
      const auto checkPending = [&] {
        check(completionBackend.FencePending(firstFence) && completionBackend.FencePending(sameCommandFence) &&
          completionBackend.FencePending(secondFence), "accepted clear fences remain pending before actual queue completion");
        check(completionBackend.Busy(surface[1]) && completionBackend.Busy(0x10100) &&
          completionBackend.Busy(destination.data) && completionBackend.Busy(destination.header),
          "both targets remain busy by header and payload before actual queue completion");
        check(completionBackend.Snapshot().completedCommands == outstanding.completedCommands && completionService.completedSequence == 0,
          "neither backend nor service invents completion before queue work is done");
      };
      checkPending();
      completionService.beforePresentCompletion = checkPending;
      completionBackend.Present(completionState.rt, false);
      completionService.beforePresentCompletion = {};
      check(completionService.completedSequence == completionService.sequence,
        "successful empty Present reply follows actual service queue completion contract");
      check(!completionBackend.FencePending(firstFence) && !completionBackend.FencePending(sameCommandFence) &&
        !completionBackend.FencePending(secondFence), "successful Present retires all prior queue fences without explicit Flush or Readback");
      check(!completionBackend.Busy(surface[1]) && !completionBackend.Busy(0x10100) &&
        !completionBackend.Busy(destination.data) && !completionBackend.Busy(destination.header),
        "successful Present retires both its source and other earlier queue targets");
      auto completed = completionBackend.Snapshot();
      check(completed.completedCommands == completed.submittedCommands && completed.presentedFrames == 1,
        "Present completion advances checked command accounting and successful frame count");
      auto commandsBeforeWait = completionService.commands.size();
      completionBackend.WaitGpuFrame(0);
      check(completionService.commands.size() == commandsBeforeWait, "completed Present frame never needs a redundant Flush");
      completionBackend.Present(completionState.rt, true);
      completionBackend.Clear(completionState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      const auto laterFence = completionBackend.Fence();
      completed = completionBackend.Snapshot();
      check(completionBackend.FencePending(laterFence) && completionBackend.Busy(destination.header) &&
        completed.completedCommands < completed.submittedCommands, "commands after Present remain genuinely outstanding");
      commandsBeforeWait = completionService.commands.size();
      completionBackend.WaitGpuFrame(0);
      completionBackend.WaitGpuFrame(1);
      check(completionService.commands.size() == commandsBeforeWait && completionBackend.FencePending(laterFence),
        "waiting completed frames neither flushes nor retires later queue work");
      for (auto status : {webgpu::Status::Error, webgpu::Status::Lost}) {
        completionService.presentStatus = status;
        const auto serviceCompleted = completionService.completedSequence;
        rejects([&] { completionBackend.Present(completionState.rt, false); }, "service");
        const auto failed = completionBackend.Snapshot();
        check(failed.completedCommands == completed.completedCommands && failed.submittedCommands == completed.submittedCommands &&
          failed.presentedFrames == completed.presentedFrames && completionService.completedSequence == serviceCompleted,
          "failed Present does not invent completed commands or successful frames");
        check(completionBackend.FencePending(laterFence) && completionBackend.Busy(destination.header),
          "failed Present leaves later fence and target genuinely pending");
        rejects([&] { completionBackend.WaitGpuFrame(2); }, "unsubmitted frame");
      }
      completionService.presentStatus = webgpu::Status::Ok;
      completionBackend.Present(completionState.rt, false);
      check(!completionBackend.FencePending(laterFence) && !completionBackend.Busy(destination.header),
        "successful Present retry retires the actual outstanding queue despite failed command ID gaps");
      commandsBeforeWait = completionService.commands.size();
      completionBackend.WaitGpuFrame(2);
      check(completionService.commands.size() == commandsBeforeWait && completionService.Count(Opcode::Flush) == 0 &&
        completionService.Count(Opcode::Readback) == 0, "Present completion and frame waits require no explicit completion commands");
      completionBackend.Clear(completionState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      const auto flushFence = completionBackend.Fence();
      check(completionBackend.FencePending(flushFence) && completionBackend.Busy(destination.header), "new clear remains pending until explicit Flush");
      completionBackend.Flush();
      check(!completionBackend.FencePending(flushFence) && !completionBackend.Busy(destination.header) &&
        completionBackend.Snapshot().completedCommands == completionBackend.Snapshot().submittedCommands,
        "explicit Flush still acknowledges genuine queue completion");
      completionBackend.Clear(completionState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      const auto readbackFence = completionBackend.Fence();
      check(completionBackend.FencePending(readbackFence) && completionBackend.Busy(destination.header), "new clear remains pending until explicit Readback");
      completionBackend.Sync(destination.data, 0);
      check(!completionBackend.FencePending(readbackFence) && !completionBackend.Busy(destination.header) &&
        completionBackend.Snapshot().completedCommands == completionBackend.Snapshot().submittedCommands,
        "explicit Readback still acknowledges genuine queue completion");
      commandsBeforeWait = completionService.commands.size();
      for (uint32_t n = 0; n < 8192; ++n) {
        const auto completedFence = completionBackend.Fence();
        check(!completionBackend.FencePending(completedFence), "fences for already completed queue work are immediately complete");
        completionBackend.WaitFence(completedFence);
      }
      check(completionService.commands.size() == commandsBeforeWait,
        "completed fence tokens never exhaust pending storage or require extra GPU commands");
      completionBackend.Clear(completionState, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
      uint32_t lastPendingFence = 0;
      for (uint32_t n = 0; n < 4096; ++n) {
        lastPendingFence = completionBackend.Fence();
        check(completionBackend.FencePending(lastPendingFence), "actual outstanding fences retain the bounded pending budget");
      }
      rejects([&] { completionBackend.Fence(); }, "pending fence budget");
      completionBackend.Flush();
      check(!completionBackend.FencePending(lastPendingFence), "real completion retires a full pending fence budget");
      check(completionBackend.Fence() == lastPendingFence + 1,
        "rejected fence allocation does not consume or publish a fence identity");
    }
    service.fail = true;
    rejects([&] { backend.Flush(); }, "service");
    Service retiringService;
    MenuBackend retiringBackend(runtime, image, [&](auto opcode, auto resource, auto bytes) {
      return retiringService.Execute(opcode, resource, bytes);
    });
    retiringBackend.Init(4, 4);
    retiringBackend.Clear(state, 0, nullptr, gpu::kClearTarget, 0, 1, 0);
    check(!retiringService.live.empty(), "retirement fixture owns real GPU resources");
    runtime.Cancel();
    retiringBackend.Retire();
    retiringBackend.Retire();
    check(retiringService.live.empty(), "cancelled runtime retained GPU resources across replacement");
    rejects([&] { retiringBackend.Init(4, 4); }, "retired");
    std::puts("menu-backend-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
