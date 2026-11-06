#pragma once
#include "browser/hook-graphics.h"
#include "browser/image.h"
#include "browser/shader-inventory.h"
#include "browser/webgpu-transport.h"
#include "gpu/backend.h"
#include <functional>

namespace browser {

struct MenuBackendLimits {
  uint32_t maxResources = 256;
  uint32_t maxResourceBytes = 16 * 1024 * 1024;
  uint32_t maxResidentBytes = 64 * 1024 * 1024;
  uint32_t maxVertices = 8192;
  uint32_t maxExpandedVertices = 0;
  uint32_t maxPipelines = 256;
  bool batchDraws = false;
  bool batchCommands = false;
  bool compactVertices = false;
  bool indexedVertices = false;
  bool postCommands = false;
  bool residentVertices = false;
};

class MenuBackend final : public gpu::Backend {
public:
  using Execute = std::function<webgpu::Result(webgpu::Opcode, uint32_t, std::span<const uint8_t>)>;
  MenuBackend(Runtime& runtime, PreparedImage& image, std::shared_ptr<webgpu::Transport> transport, MenuBackendLimits limits = {});
  MenuBackend(Runtime& runtime, PreparedImage& image, Execute execute, MenuBackendLimits limits = {});
  ~MenuBackend() override;
  void Retire();
  struct FrameMetrics {
    uint64_t frames, commandWaitMicroseconds, readbackWaitMicroseconds, uploadWaitMicroseconds, drawWaitMicroseconds;
    uint32_t resources, pipelines, residentBytes;
    ShaderInventory inventory;
  };
  void SetMetricsSink(std::function<void(const FrameMetrics&)> sink);
  // the page's output for the next Present, asked once a frame: the canvas size in pixels and the upscaling filter.
  // A changed size is configured before that Present's packet, in order with it.
  // scale: screen sized render targets are drawn at this many times the guest's size (1 the console's)
  struct Output {
    uint32_t width = 0, height = 0;
    bool linear = false;
    float scale = 1;
  };
  void SetOutputSource(std::function<Output()> source);
  struct SnapshotData {
    uint32_t pipelineCount = 0;
    uint32_t resourceCount = 0;
    uint64_t residentBytes = 0;
    uint64_t submittedCommands = 0;
    uint64_t completedCommands = 0;
    uint64_t presentedFrames = 0;
    std::string storageViewFailure;
    uint64_t shaderTranslations = 0;
    uint32_t shaderCacheBytes = 0;
    uint32_t shaderCacheEntries = 0;
    uint32_t uploadCacheEntries = 0;
  };
  SnapshotData Snapshot() const;
  bool Init(uint32_t width, uint32_t height) override;
  void Clear(const gpu::State& state, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override;
  void Draw(const gpu::State& state, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
  void IndexSource(uint32_t address) override;
  void Copy(const gpu::Surface& source, const gpu::Surface& destination, uint32_t count, uint32_t sourceRects, uint32_t destinationPoints) override;
  bool HasGpuContents(uint32_t address) const override;
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
  // the optional effects game hooks ask for through d3d::; none is drawn yet, so a hook's request is counted instead
  bool Supports(GraphicsEffect) const { return false; }
  uint32_t Fence() override;
  bool FencePending(uint32_t fence) override;
  void WaitFence(uint32_t fence) override;
  bool Busy(uint32_t resource) override;
  void WaitResource(uint32_t resource) override;
  void WaitGpuFrame(uint64_t frame) override;
  bool ReleaseResource(uint32_t header, uint32_t data) override;
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}
