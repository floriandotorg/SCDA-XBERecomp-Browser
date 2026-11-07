#pragma once
#include <functional>
#include "browser/contiguous-memory.h"
#include "browser/hook-graphics.h"
#include "browser/sdk-provider.h"
#include "gpu/backend.h"
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace browser {

enum class GraphicsMode { CpuDevice, Checkpoint, StateCapture, Rendering };

struct GraphicsCheckpoint : Fault {
  explicit GraphicsCheckpoint(const X86Context& ctx) : Fault(ctx, 0, 0, "diagnostic graphics initialization requested; no device completion or rendered frame") {}
};

struct GraphicsPresentCheckpoint : Fault {
  uint32_t completedPresents;
  GraphicsPresentCheckpoint(const X86Context& ctx, uint32_t count)
    : Fault(ctx, 0, 0, "diagnostic Present-count checkpoint reached after completed backend Present; not menu acceptance"), completedPresents(count) {}
};

class DiagnosticGraphics final : public SdkGraphics {
public:
  enum class Request : uint32_t {
    Init, Resource, Commands, Clear, Draw, Copy, Present, ReadUnavailable, Flush, Gamma,
    Occlusion, FilterShadows, ApplyShadows, Fence, WaitUnavailable, Unsupported, CpuClear, CpuSwap, PersistDisplay
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
  struct PersistedDisplay {
    uint32_t common = 0;
    uint32_t format = 0;
    uint32_t size = 0;
    std::vector<uint8_t> pixels;
  };
  struct SnapshotData {
    bool initialized = false;
    uint64_t total = 0;
    size_t count = 0;
    std::array<Event, 64> events{};
    std::vector<Resource> resources;
    uint32_t completedPresents = 0;
    uint32_t commandWords = 0;
    std::vector<uint32_t> commands;
    std::array<uint32_t, 4> persistedDisplay{};
  };
  DiagnosticGraphics(Runtime& runtime, PreparedImage& image, ContiguousMemoryAccess allocations, GraphicsMode mode = GraphicsMode::CpuDevice, std::unique_ptr<gpu::Backend> backend = {});
  ~DiagnosticGraphics() override;
  void Shutdown();
  void ConfigurePresentCheckpoint(uint32_t frames);
  bool BelongsTo(Runtime& runtime, PreparedImage& image) const override;
  bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) override;
  void CreateDevice(X86Context& ctx, uint8_t* base);
  SnapshotData Snapshot() const;
  std::optional<PersistedDisplay> PersistedDisplaySnapshot() const;
  gpu::Backend& Backend();
  gpu::State State() const;
  gpu::State SnapshotState() const;
  HookGraphics& Hooks() { return hooks_; }
  // after each presented Swap, as native's app::Pace after D3DDevice_Swap (the frame cap)
  void SetPacer(std::function<void()> pace) { pace_ = std::move(pace); }
private:
  std::function<void()> pace_;
  class Recorder;
  Runtime& runtime_;
  PreparedImage& image_;
  Memory* memory_;
  ContiguousMemoryAccess allocations_;
  GraphicsMode mode_;
  mutable std::recursive_mutex lock_;
  std::unique_ptr<Recorder> backend_;
  std::unique_ptr<gpu::Backend> renderer_;
  gpu::State state_;
  HookGraphics hooks_{state_, lock_};
  std::vector<Resource> resources_;
  std::vector<uint32_t> allocationsOwned_;
  std::unordered_set<uint32_t> gpuWritten_;
  std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> vertexShaders_;
  std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> pixelShaders_;
  std::array<Event, 64> events_{};
  uint64_t total_ = 0;
  uint32_t push_ = 0;
  uint32_t pushBytes_ = 0;
  uint32_t capturedWords_ = 0;
  uint32_t decodedWords_ = 0;
  std::vector<uint32_t> commands_;
  std::optional<PersistedDisplay> persistedDisplay_;
  uint32_t userStream_ = 0;  // the vertex buffer header a user pointer draw binds as stream 0
  bool released_ = false;    // the guest released its device; its next CreateDevice gets it back
  bool initialized_ = false;
  bool creating_ = false;
  bool unsubmitted_ = false;
  bool failed_ = false;
  bool typedViewport_ = false;
  bool typedVertexShader_ = false;
  uint32_t fence_ = 0;
  uint32_t presentCheckpoint_ = 0;
  uint32_t completedPresents_ = 0;
  void Require(const X86Context& ctx, uint8_t* base) const;
  void CheckRecorder() const;
  void Record(Request request, std::array<uint32_t, 6> arguments = {});
  void Invalidate(uint32_t header = 0);
  void Capture(const X86Context& ctx);
  void CaptureState(const X86Context& ctx, bool forceViewport = false);
  bool TypedState(X86Context& ctx, uint8_t* base, uint32_t id);
  void UpdateViewportConstants();
  void Reference(X86Context& ctx, uint8_t* base, uint32_t resource, bool add);
  void ApplyCommands(const X86Context& ctx);
  uint32_t Allocate(const X86Context& ctx, uint32_t bytes);
  gpu::Surface NewSurface(const X86Context& ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t bpp);
  [[noreturn]] void Unavailable(Request request, uint32_t address, const char* reason, std::array<uint32_t, 6> arguments = {});
};

}
