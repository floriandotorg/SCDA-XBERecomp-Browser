#pragma once
#include "browser/contiguous-memory.h"
#include "browser/sdk-provider.h"
#include "gpu/backend.h"

namespace browser {

enum class GraphicsMode { CpuDevice, Checkpoint };

struct GraphicsCheckpoint : Fault {
  explicit GraphicsCheckpoint(const X86Context& ctx) : Fault(ctx, 0, 0, "diagnostic graphics initialization requested; no device completion or rendered frame") {}
};

class DiagnosticGraphics final : public SdkGraphics {
public:
  enum class Request : uint32_t {
    Init, Resource, Commands, Clear, Draw, Copy, Present, ReadUnavailable, Flush, Gamma,
    Occlusion, FilterShadows, ApplyShadows, Fence, WaitUnavailable, Unsupported, CpuClear, CpuSwap
  };
  struct Event {
    uint64_t sequence = 0;
    Request request = Request::Init;
    std::array<uint32_t, 6> arguments{};
  };
  struct Resource {
    uint32_t header = 0;
    uint32_t data = 0;
    uint32_t bytes = 0;
    uint32_t format = 0;
    uint32_t size = 0;
    bool unavailable = false;
  };
  struct SnapshotData {
    bool initialized = false;
    uint64_t total = 0;
    size_t count = 0;
    std::array<Event, 64> events{};
    std::vector<Resource> resources;
    uint32_t commandWords = 0;
    std::vector<uint32_t> commands;
  };
  DiagnosticGraphics(Runtime& runtime, PreparedImage& image, ContiguousMemoryAccess allocations, GraphicsMode mode = GraphicsMode::CpuDevice);
  ~DiagnosticGraphics() override;
  bool BelongsTo(Runtime& runtime, PreparedImage& image) const override;
  bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) override;
  void CreateDevice(X86Context& ctx, uint8_t* base);
  SnapshotData Snapshot() const;
  gpu::Backend& Backend();
  gpu::State State() const;
private:
  class Recorder;
  Runtime& runtime_;
  PreparedImage& image_;
  Memory* memory_;
  ContiguousMemoryAccess allocations_;
  GraphicsMode mode_;
  mutable std::recursive_mutex lock_;
  std::unique_ptr<Recorder> backend_;
  gpu::State state_;
  std::vector<Resource> resources_;
  std::vector<uint32_t> allocationsOwned_;
  std::array<Event, 64> events_{};
  uint64_t total_ = 0;
  uint32_t push_ = 0;
  uint32_t pushBytes_ = 0;
  uint32_t capturedWords_ = 0;
  std::vector<uint32_t> commands_;
  bool initialized_ = false;
  bool creating_ = false;
  uint32_t fence_ = 0;
  void Require(const X86Context& ctx, uint8_t* base) const;
  void CheckRecorder() const;
  void Record(Request request, std::array<uint32_t, 6> arguments = {});
  void Invalidate(uint32_t header = 0);
  void Capture(const X86Context& ctx);
  uint32_t Allocate(const X86Context& ctx, uint32_t bytes);
  gpu::Surface NewSurface(const X86Context& ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t bpp);
  [[noreturn]] void Unavailable(Request request, uint32_t address, const char* reason);
};

}
