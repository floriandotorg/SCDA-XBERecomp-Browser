#pragma once
#include "browser/cpu_runtime.h"
#include "gpu/state.h"
#include <optional>
#include <span>

namespace browser {

enum class RenderResourceKind : uint32_t { VertexBuffer = 1, IndexBuffer, Texture, Surface, IndexData };

struct RenderResourceRequest {
  uint64_t id = 0;
  uint64_t version = 0;
  RenderResourceKind kind = RenderResourceKind::VertexBuffer;
  uint32_t headerAddress = 0;
  uint32_t payloadAddress = 0;
  uint32_t payloadBytes = 0;
};

struct RenderResourceSnapshot {
  uint64_t id = 0;
  uint64_t version = 0;
  RenderResourceKind kind = RenderResourceKind::VertexBuffer;
  uint32_t headerAddress = 0;
  uint32_t payloadAddress = 0;
  std::vector<uint8_t> header;
  std::vector<uint8_t> payload;
};

struct RenderSnapshotLimits {
  uint32_t maxResources = 64;
  uint32_t maxPayloadBytes = 16 * 1024 * 1024;
  uint32_t maxTotalBytes = 32 * 1024 * 1024;
  uint32_t maxShaderWords = 65536;
  uint32_t maxShaderConstants = 192;
};

struct RenderSnapshot {
  gpu::State state;
  std::optional<gpu::VertexShader> vertexShader;
  std::optional<gpu::PixelShader> pixelShader;
  std::vector<RenderResourceSnapshot> resources;
};

RenderSnapshot CaptureRenderSnapshot(const X86Context& context, Memory& memory, const gpu::State& state,
  std::span<const RenderResourceRequest> resources, const RenderSnapshotLimits& limits = {});
std::vector<uint8_t> SerializeRenderSnapshot(const RenderSnapshot& snapshot, const RenderSnapshotLimits& limits = {});
RenderSnapshot DeserializeRenderSnapshot(std::span<const uint8_t> bytes, const RenderSnapshotLimits& limits = {});

}
