#pragma once
#include "browser/image.h"
#include "browser/webgpu-transport.h"
#include "gpu/backend.h"
#include <functional>

namespace browser {

struct MenuBackendLimits {
  uint32_t maxResources = 256;
  uint32_t maxResourceBytes = 16 * 1024 * 1024;
  uint32_t maxResidentBytes = 64 * 1024 * 1024;
  uint32_t maxVertices = 8192;
  uint32_t maxPipelines = 256;
};

class MenuBackend final : public gpu::Backend {
public:
  using Execute = std::function<webgpu::Result(webgpu::Opcode, uint32_t, std::span<const uint8_t>)>;
  MenuBackend(Runtime& runtime, PreparedImage& image, std::shared_ptr<webgpu::Transport> transport, MenuBackendLimits limits = {});
  MenuBackend(Runtime& runtime, PreparedImage& image, Execute execute, MenuBackendLimits limits = {});
  ~MenuBackend() override;
  struct SnapshotData {
    uint32_t pipelineCount = 0;
    uint32_t resourceCount = 0;
    uint64_t residentBytes = 0;
    uint64_t submittedCommands = 0;
    uint64_t completedCommands = 0;
    uint64_t presentedFrames = 0;
  };
  SnapshotData Snapshot() const;
  bool Init(uint32_t width, uint32_t height) override;
  void Clear(const gpu::State& state, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override;
  void Draw(const gpu::State& state, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
  void Copy(const gpu::Surface& source, const gpu::Surface& destination, uint32_t count, uint32_t sourceRects, uint32_t destinationPoints) override;
  void Present(const gpu::Surface& front, bool pillarbox) override;
  void Sync(uint32_t address, uint32_t size) override;
  void SyncLate(uint32_t address) override;
  bool DeliverLate(uint32_t address) override;
  void RecordLate(uint32_t address) override;
  void Flush() override;
  void SetGamma(const uint8_t* ramp) override;
  void Occlusion(const gpu::State&, const float*) override;
  void FilterShadows(const gpu::State&, const float*) override;
  void ApplyShadows(const gpu::State&) override;
  uint32_t Fence() override;
  bool FencePending(uint32_t fence) override;
  void WaitFence(uint32_t fence) override;
  bool Busy(uint32_t resource) override;
  void WaitResource(uint32_t resource) override;
  void WaitGpuFrame(uint64_t frame) override;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
