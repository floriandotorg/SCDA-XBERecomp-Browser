#include "browser/diagnostic-graphics.h"
#include "browser/diagnostic-shaders.h"
#include "browser/push-state.h"
#include "cpu/x86_ops.h"
#include "image_symbols.h"
#include <algorithm>
#include <bit>
#include <sstream>

#define DG_CAT_(left, right) left##right
#define DG_CAT(left, right) DG_CAT_(left, right)
#define DG_ADDRESS(name) DG_CAT(0x, SYM_##name)
#define DG_GUEST(name) DG_CAT(sub_, SYM_##name)
#define DG_ORIGINAL(name) DG_CAT(__imp__sub_, SYM_##name)
#define DG_BOUNDARY(name, id) \
  X86_FUNC(DG_ORIGINAL(name)); \
  X86_FUNC(DG_GUEST(name)) { \
    if (browser::Runtime::ObserveSdkEntry(ctx, base, id)) { DG_ORIGINAL(name)(ctx, base); } \
  }

namespace {
struct ResourceReleaseFrame {
  X86Context* context = nullptr;
  uint8_t* base = nullptr;
  browser::DiagnosticGraphics* owner = nullptr;
  std::unique_lock<std::recursive_mutex> lock;
  uint32_t stack = 0;
  uint32_t header = 0;
  std::array<uint32_t, 2> words{};
  uint32_t parent = 0;
  std::array<uint32_t, 2> parentWords{};
  bool internal = false;
  bool completed = false;
};
thread_local std::array<ResourceReleaseFrame, 32> resourceReleases;
thread_local size_t resourceReleaseDepth = 0;
class ResourceReleaseScope {
public:
  ResourceReleaseFrame& frame;
  ResourceReleaseScope(X86Context& ctx, uint8_t* base, bool internal)
    : frame(resourceReleases[CheckedIndex(ctx)]) {
    frame = {};
    frame.context = &ctx;
    frame.base = base;
    frame.stack = ctx.esp;
    frame.internal = internal;
    ++resourceReleaseDepth;
  }
  ~ResourceReleaseScope() { frame = {}; --resourceReleaseDepth; }
  ResourceReleaseScope(const ResourceReleaseScope&) = delete;
  ResourceReleaseScope& operator=(const ResourceReleaseScope&) = delete;
private:
  static size_t CheckedIndex(const X86Context& ctx) {
    if (resourceReleaseDepth == resourceReleases.size()) { throw browser::Fault(ctx, ctx.esp, 0, "resource release nesting limit exceeded"); }
    return resourceReleaseDepth;
  }
};
void ObserveResourceRelease(X86Context& ctx, uint8_t* base, bool internal, X86Func original) {
  ResourceReleaseScope scope(ctx, base, internal);
  browser::Runtime::ObserveSdkEntry(ctx, base, 66);
  original(ctx, base);
  scope.frame.completed = true;
  browser::Runtime::ObserveSdkEntry(ctx, base, 67);
}
}
#ifdef SYM_D3DResource_Release
X86_FUNC(DG_ORIGINAL(D3DResource_Release));
X86_FUNC(DG_GUEST(D3DResource_Release)) { ObserveResourceRelease(ctx, base, false, DG_ORIGINAL(D3DResource_Release)); }
#endif
#ifdef SYM_D3D_InternalRelease
X86_FUNC(DG_ORIGINAL(D3D_InternalRelease));
X86_FUNC(DG_GUEST(D3D_InternalRelease)) { ObserveResourceRelease(ctx, base, true, DG_ORIGINAL(D3D_InternalRelease)); }
#endif

#ifdef SYM_D3DDevice_MakeSpace
DG_BOUNDARY(D3DDevice_MakeSpace, 4)
#endif
#ifdef SYM_D3D_MakeRequestedSpace
DG_BOUNDARY(D3D_MakeRequestedSpace, 5)
#endif
#ifdef SYM_CDevice_KickOff
DG_BOUNDARY(CDevice_KickOff, 6)
#endif
#ifdef SYM_D3D_KickOffAndWaitForIdle
DG_BOUNDARY(D3D_KickOffAndWaitForIdle, 7)
#endif
#ifdef SYM_D3D_BlockOnResource
DG_BOUNDARY(D3D_BlockOnResource, 8)
#endif
#ifdef SYM_Lock2DSurface
DG_BOUNDARY(Lock2DSurface, 9)
#endif
#ifdef SYM_D3DResource_BlockUntilNotBusy
DG_BOUNDARY(D3DResource_BlockUntilNotBusy, 10)
#endif
#ifdef SYM_D3DResource_IsBusy
DG_BOUNDARY(D3DResource_IsBusy, 11)
#endif
#ifdef SYM_D3DDevice_InsertFence
DG_BOUNDARY(D3DDevice_InsertFence, 12)
#endif
#ifdef SYM_D3DDevice_BlockOnFence
DG_BOUNDARY(D3DDevice_BlockOnFence, 13)
#endif
#ifdef SYM_D3DDevice_IsFencePending
DG_BOUNDARY(D3DDevice_IsFencePending, 14)
#endif
#ifdef SYM_D3DDevice_PersistDisplay
DG_BOUNDARY(D3DDevice_PersistDisplay, 15)
#endif
#ifdef SYM_D3DDevice_Swap
DG_BOUNDARY(D3DDevice_Swap, 16)
#endif
#ifdef SYM_D3DDevice_Clear
DG_BOUNDARY(D3DDevice_Clear, 17)
#endif
#ifdef SYM_D3DDevice_SetRenderStateNotInline
DG_BOUNDARY(D3DDevice_SetRenderStateNotInline, 18)
#endif
#ifdef SYM_D3DDevice_DrawVertices
DG_BOUNDARY(D3DDevice_DrawVertices, 19)
#endif
#ifdef SYM_D3DDevice_DrawIndexedVertices
DG_BOUNDARY(D3DDevice_DrawIndexedVertices, 20)
#endif
#ifdef SYM_D3DDevice_CopyRects
DG_BOUNDARY(D3DDevice_CopyRects, 21)
#endif
#ifdef SYM_D3DDevice_Begin
DG_BOUNDARY(D3DDevice_Begin, 22)
#endif
#ifdef SYM_D3DDevice_End
DG_BOUNDARY(D3DDevice_End, 23)
#endif
#ifdef SYM_D3D_SetFence
DG_BOUNDARY(D3D_SetFence, 24)
#endif
#ifdef SYM_D3D_BlockOnTime
DG_BOUNDARY(D3D_BlockOnTime, 25)
#endif
#ifdef SYM_D3DDevice_BlockUntilVerticalBlank
DG_BOUNDARY(D3DDevice_BlockUntilVerticalBlank, 26)
#endif
#ifdef SYM_D3DDevice_Release
DG_BOUNDARY(D3DDevice_Release, 27)
#endif
#ifdef SYM_D3DDevice_SetGammaRamp
DG_BOUNDARY(D3DDevice_SetGammaRamp, 28)
#endif
#ifdef SYM_D3DDevice_EnableOverlay
DG_BOUNDARY(D3DDevice_EnableOverlay, 68)
#endif

#ifdef SYM_D3DDevice_SetRenderState_Simple
DG_BOUNDARY(D3DDevice_SetRenderState_Simple, 29)
#endif
#ifdef SYM_D3DDevice_SetVertexShaderConstant1Fast
DG_BOUNDARY(D3DDevice_SetVertexShaderConstant1Fast, 30)
#endif
#ifdef SYM_D3DDevice_SetVertexShaderConstantNotInlineFast
DG_BOUNDARY(D3DDevice_SetVertexShaderConstantNotInlineFast, 31)
#endif
#ifdef SYM_D3DDevice_SetPixelShader
DG_BOUNDARY(D3DDevice_SetPixelShader, 32)
#endif
#ifdef SYM_D3DDevice_SetPixelShaderConstant
DG_BOUNDARY(D3DDevice_SetPixelShaderConstant, 33)
#endif
#ifdef SYM_D3DDevice_SetViewport
DG_BOUNDARY(D3DDevice_SetViewport, 34)
#endif
#ifdef SYM_D3DDevice_SetScissors
DG_BOUNDARY(D3DDevice_SetScissors, 35)
#endif
#ifdef SYM_D3DDevice_SetScreenSpaceOffset
DG_BOUNDARY(D3DDevice_SetScreenSpaceOffset, 36)
#endif
#ifdef SYM_D3DDevice_SetTransform
DG_BOUNDARY(D3DDevice_SetTransform, 37)
#endif
#ifdef SYM_D3DDevice_SetMaterial
DG_BOUNDARY(D3DDevice_SetMaterial, 38)
#endif
#ifdef SYM_D3DDevice_SetVertexShader
DG_BOUNDARY(D3DDevice_SetVertexShader, 39)
#endif
#ifdef SYM_D3DDevice_SetRenderTarget
DG_BOUNDARY(D3DDevice_SetRenderTarget, 45)
#endif
#ifdef SYM_D3DDevice_SetTexture
DG_BOUNDARY(D3DDevice_SetTexture, 46)
#endif
#ifdef SYM_D3DDevice_SetStreamSource
DG_BOUNDARY(D3DDevice_SetStreamSource, 47)
#endif
#ifdef SYM_D3DDevice_SetIndices
DG_BOUNDARY(D3DDevice_SetIndices, 48)
#endif
#ifdef SYM_D3DDevice_CreateVertexShader
DG_BOUNDARY(D3DDevice_CreateVertexShader, 49)
#endif
#ifdef SYM_D3DDevice_DeleteVertexShader
DG_BOUNDARY(D3DDevice_DeleteVertexShader, 50)
#endif
#ifdef SYM_D3DDevice_CreatePixelShader
DG_BOUNDARY(D3DDevice_CreatePixelShader, 51)
#endif
#ifdef SYM_D3DDevice_DeletePixelShader
DG_BOUNDARY(D3DDevice_DeletePixelShader, 52)
#endif
#ifdef SYM_D3DDevice_SwitchTexture
DG_BOUNDARY(D3DDevice_SwitchTexture, 53)
#endif
#ifdef SYM_D3DDevice_SetVertexShaderConstant1
DG_BOUNDARY(D3DDevice_SetVertexShaderConstant1, 54)
#endif
#ifdef SYM_D3DDevice_SetVertexShaderConstant4
DG_BOUNDARY(D3DDevice_SetVertexShaderConstant4, 55)
#endif
#ifdef SYM_D3DDevice_SetTextureState_BumpEnv
DG_BOUNDARY(D3DDevice_SetTextureState_BumpEnv, 56)
#endif
#ifdef SYM_D3DDevice_SetTextureState_BorderColor
DG_BOUNDARY(D3DDevice_SetTextureState_BorderColor, 57)
#endif
#ifdef SYM_D3DDevice_SetTextureState_ColorKeyColor
DG_BOUNDARY(D3DDevice_SetTextureState_ColorKeyColor, 58)
#endif
#ifdef SYM_D3DDevice_SetTextureState_TexCoordIndex
DG_BOUNDARY(D3DDevice_SetTextureState_TexCoordIndex, 59)
#endif
#ifdef SYM_D3DDevice_SetTextureStageStateNotInline
DG_BOUNDARY(D3DDevice_SetTextureStageStateNotInline, 60)
#endif
#ifdef SYM_D3DDevice_SetLight
DG_BOUNDARY(D3DDevice_SetLight, 61)
#endif
#ifdef SYM_D3DDevice_LightEnable
DG_BOUNDARY(D3DDevice_LightEnable, 62)
#endif
#ifdef SYM_D3DDevice_SetShaderConstantMode
DG_BOUNDARY(D3DDevice_SetShaderConstantMode, 63)
#endif

namespace browser {
namespace {
constexpr uint32_t device = DG_ADDRESS(D3D_Device);
constexpr uint32_t devicePointer = DG_ADDRESS(D3D_pDevice);
constexpr uint32_t deviceBytes = 0x2800;
constexpr uint32_t captureBytes = 0x10000;
#ifdef SYM_D3D_DirtyFlags
constexpr uint32_t rollbackBase = DG_ADDRESS(D3D_DirtyFlags);
#else
constexpr uint32_t rollbackBase = device;
#endif
constexpr uint32_t rollbackBytes = device + deviceBytes - rollbackBase;
}

class DiagnosticGraphics::Recorder final : public gpu::Backend {
  DiagnosticGraphics& owner_;
  bool ready_ = false;
public:
  explicit Recorder(DiagnosticGraphics& owner) : owner_(owner) {}
  void Reset() noexcept { ready_ = false; }
  bool Init(uint32_t width, uint32_t height) override {
    std::lock_guard lock(owner_.lock_);
    owner_.CheckRecorder();
    if (ready_ || !width || !height) { return false; }
    owner_.Record(Request::Init, {width, height});
    ready_ = true;
    return true;
  }
  void Clear(const gpu::State& state, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil) override {
    std::lock_guard lock(owner_.lock_);
    if (owner_.mode_ != GraphicsMode::CpuDevice) { owner_.Unavailable(Request::Unsupported, flags, "state capture cannot clear GPU targets"); }
    if (count || rects) { owner_.Unavailable(Request::Unsupported, count, "diagnostic graphics clear rectangles are not recorded"); }
    owner_.Record(Request::Clear, {count, flags, color, std::bit_cast<uint32_t>(z), stencil, state.rt.header});
    if (flags == gpu::kClearTarget) {
      auto resource = std::find_if(owner_.resources_.begin(), owner_.resources_.end(), [&](const auto& entry) { return entry.header == state.rt.header; });
      if (resource == owner_.resources_.end() || ((resource->format >> 8) & 255) != 18) {
        owner_.Unavailable(Request::Unsupported, state.rt.header, "diagnostic CPU clear requires an owned linear A8R8G8B8 target");
      }
      std::array<uint32_t, 5> header;
      owner_.memory_->Read({}, resource->header, header.data(), sizeof(header));
      if (header[1] != (resource->data & 0xfffffff) || header[3] != resource->format || header[4] != resource->size) {
        owner_.Unavailable(Request::Unsupported, resource->header, "diagnostic CPU clear storage metadata changed");
      }
      const uint32_t width = (resource->size & 0xfff) + 1;
      const uint32_t height = ((resource->size >> 12) & 0xfff) + 1;
      const uint32_t pitch = ((resource->size >> 24) + 1) * 64;
      if (state.viewport.x || state.viewport.y || state.viewport.w != width || state.viewport.h != height) {
        owner_.Unavailable(Request::Unsupported, resource->header, "diagnostic CPU clear requires the full initial viewport");
      }
      owner_.memory_->CheckRange({}, resource->data, resource->bytes);
      const std::vector<uint32_t> row(width, color);
      for (uint32_t n = 0; n < height; ++n) { owner_.memory_->Write({}, resource->data + n * pitch, row.data(), width * 4); }
      resource->unavailable = false;
      owner_.Record(Request::CpuClear, {resource->header, resource->data, width, height, pitch, color});
      return;
    }
    owner_.Unavailable(Request::Unsupported, flags, "diagnostic CPU depth or stencil clear is not implemented");
  }
  void Draw(const gpu::State& state, uint32_t primitive, uint32_t start, uint32_t count, const uint8_t* indices) override {
    std::lock_guard lock(owner_.lock_);
    if (owner_.mode_ != GraphicsMode::CpuDevice) { owner_.Unavailable(Request::Unsupported, count, "state capture cannot draw GPU targets"); }
    if (indices) { owner_.Unavailable(Request::Unsupported, count, "diagnostic graphics indexed draws are not recorded"); }
    owner_.Record(Request::Draw, {primitive, start, count, state.rt.header, state.zs.header});
    owner_.Invalidate(state.rt.header);
    if (state.zs.header) { owner_.Invalidate(state.zs.header); }
  }
  void Copy(const gpu::Surface& source, const gpu::Surface& destination, uint32_t count, uint32_t rects, uint32_t points) override {
    std::lock_guard lock(owner_.lock_);
    if (owner_.mode_ != GraphicsMode::CpuDevice) { owner_.Unavailable(Request::Unsupported, destination.header, "state capture cannot copy GPU targets"); }
    owner_.Record(Request::Copy, {source.header, destination.header, count, rects, points});
    owner_.Invalidate(destination.header);
  }
  void Present(const gpu::Surface& surface, bool pillarbox) override {
    std::lock_guard lock(owner_.lock_);
    if (owner_.mode_ != GraphicsMode::CpuDevice) { owner_.Unavailable(Request::Unsupported, surface.header, "state capture cannot present GPU targets"); }
    owner_.Record(Request::Present, {surface.header, uint32_t(pillarbox)});
  }
  void Sync(uint32_t address, uint32_t bytes) override {
    std::lock_guard lock(owner_.lock_);
    owner_.CheckRecorder();
    if (!((address >= 0x80000000 && address < 0x84000000) || (address >= 0xf0000000 && address < 0xf4000000))) {
      owner_.Unavailable(Request::ReadUnavailable, address, "diagnostic graphics read requires a backed CPU alias");
    }
    const uint64_t physical = address & 0x0fffffff;
    for (const auto& resource : owner_.resources_) {
      const uint64_t begin = resource.data & 0x0fffffff;
      const uint64_t end = begin + resource.bytes;
      const uint64_t requestedEnd = bytes ? physical + bytes : end;
      if (physical >= begin && physical < end && requestedEnd <= end && !resource.unavailable) { return; }
    }
    owner_.Unavailable(Request::ReadUnavailable, address, "diagnostic graphics GPU result is unavailable");
  }
  void SyncLate(uint32_t address) override { Sync(address, 0); }
  bool DeliverLate(uint32_t address) override { Sync(address, 0); return false; }
  void RecordLate(uint32_t address) override { Sync(address, 0); }
  void Flush() override {
    std::lock_guard lock(owner_.lock_);
    owner_.Unavailable(Request::Flush, 0, "diagnostic graphics cannot complete GPU commands");
  }
  void SetGamma(const uint8_t*) override {
    std::lock_guard lock(owner_.lock_);
    owner_.Unavailable(Request::Gamma, 0, "diagnostic graphics gamma application is unavailable");
  }
  void Occlusion(const gpu::State&, const float*) override {
    std::lock_guard lock(owner_.lock_);
    owner_.Record(Request::Occlusion);
    owner_.Invalidate();
  }
  void FilterShadows(const gpu::State&, const float*) override {
    std::lock_guard lock(owner_.lock_);
    owner_.Record(Request::FilterShadows);
    owner_.Invalidate();
  }
  void ApplyShadows(const gpu::State&) override {
    std::lock_guard lock(owner_.lock_);
    owner_.Record(Request::ApplyShadows);
    owner_.Invalidate();
  }
  uint32_t Fence() override {
    std::lock_guard lock(owner_.lock_);
    if (owner_.fence_ == UINT32_MAX) { owner_.Unavailable(Request::Fence, 0, "diagnostic graphics fence capacity exhausted"); }
    owner_.Record(Request::Fence, {++owner_.fence_});
    return owner_.fence_;
  }
  bool FencePending(uint32_t fence) override {
    std::lock_guard lock(owner_.lock_);
    owner_.CheckRecorder();
    if (!fence || fence > owner_.fence_) { owner_.Unavailable(Request::Fence, fence, "unknown diagnostic graphics fence"); }
    return true;
  }
  void WaitFence(uint32_t fence) override {
    std::lock_guard lock(owner_.lock_);
    owner_.Unavailable(Request::WaitUnavailable, fence, "diagnostic graphics fence completion is unavailable");
  }
  bool Busy(uint32_t resource) override {
    std::lock_guard lock(owner_.lock_);
    owner_.CheckRecorder();
    for (const auto& entry : owner_.resources_) {
      if (entry.header == resource) { return entry.unavailable; }
    }
    owner_.Unavailable(Request::WaitUnavailable, resource, "unknown diagnostic graphics resource");
  }
  void WaitResource(uint32_t resource) override {
    std::lock_guard lock(owner_.lock_);
    if (!Busy(resource)) { return; }
    owner_.Unavailable(Request::WaitUnavailable, resource, "diagnostic graphics resource completion is unavailable");
  }
  void WaitGpuFrame(uint64_t) override {
    std::lock_guard lock(owner_.lock_);
    owner_.Unavailable(Request::WaitUnavailable, 0, "diagnostic graphics frame completion is unavailable");
  }
};

DiagnosticGraphics::DiagnosticGraphics(Runtime& runtime, PreparedImage& image, ContiguousMemoryAccess allocations, GraphicsMode mode, std::unique_ptr<gpu::Backend> backend)
  : runtime_(runtime), image_(image), memory_(image.memory.get()), allocations_(std::move(allocations)), mode_(mode), backend_(std::make_unique<Recorder>(*this)), renderer_(std::move(backend)) {
  if ((mode_ == GraphicsMode::Rendering) != bool(renderer_)) {
    throw Fault({}, device, 0, "rendering mode requires an injected backend exclusively");
  }
  if (!memory_ || !allocations_.allocate || !allocations_.release || image.importsBound || image.boundRuntime || runtime.HasPublishedImage() || runtime.ActiveWorkers() || Memory::HasBorrow()) {
    throw Fault({}, device, 0, "diagnostic graphics requires its cold owned image and allocator");
  }
}
DiagnosticGraphics::~DiagnosticGraphics() = default;
void DiagnosticGraphics::ConfigurePresentCheckpoint(uint32_t frames) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  if (frames > 10000 || (frames && mode_ != GraphicsMode::Rendering)) {
    throw Fault({}, 0, 0, "Present checkpoint requires rendering and a frame limit from 0 through 10000");
  }
  if (!BelongsTo(runtime_, image_) || image_.importsBound || image_.boundRuntime || runtime_.HasPublishedImage() ||
      runtime_.ActiveWorkers() || Memory::HasBorrow() || initialized_ || creating_ || failed_) {
    throw Fault({}, 0, 0, "Present checkpoint configuration requires its cold owned image before guest work");
  }
  presentCheckpoint_ = frames;
}
void DiagnosticGraphics::Shutdown() {
  if (runtime_.OwnsCurrentWorker() || Memory::HasBorrow()) { throw Fault({}, 0, 0, "graphics shutdown requires its host owner without a borrowed lease"); }
  runtime_.Stop();
  std::lock_guard lock(lock_);
  renderer_.reset();
  persistedDisplay_.reset();
  initialized_ = false;
}

bool DiagnosticGraphics::BelongsTo(Runtime& runtime, PreparedImage& image) const {
  return &runtime == &runtime_ && &image == &image_ && image.memory.get() == memory_;
}
void DiagnosticGraphics::Require(const X86Context& ctx, uint8_t* base) const {
  if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ ||
      base != memory_->Base() || ctx.host != &runtime_ || !runtime_.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, ctx.esp, 0, "diagnostic graphics requires its bound current worker without a lease");
  }
  runtime_.Checkpoint(ctx);
}
void DiagnosticGraphics::CheckRecorder() const {
  if (!runtime_.OwnsCurrentWorker() || !BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ || Memory::HasBorrow()) {
    throw Fault({}, device, 0, "diagnostic recorder requires its owned image worker");
  }
  runtime_.Checkpoint({});
}
void DiagnosticGraphics::Record(Request request, std::array<uint32_t, 6> arguments) {
  CheckRecorder();
  Event event{++total_, request, arguments};
  events_[(total_ - 1) % events_.size()] = event;
}
void DiagnosticGraphics::Invalidate(uint32_t header) {
  uint32_t data = 0;
  if (header) {
    const auto resource = std::find_if(resources_.begin(), resources_.end(), [header](const auto& entry) { return entry.header == header; });
    if (resource == resources_.end()) { Unavailable(Request::Unsupported, header, "unknown diagnostic graphics target storage"); }
    data = resource->data;
  }
  for (auto& resource : resources_) {
    if (!header || resource.data == data) { resource.unavailable = true; }
  }
}
[[noreturn]] void DiagnosticGraphics::Unavailable(Request request, uint32_t address, const char* reason, std::array<uint32_t, 6> arguments) {
  Record(request, arguments == std::array<uint32_t, 6>{} ? std::array<uint32_t, 6>{address} : arguments);
  throw Fault({}, address, 0, reason);
}
uint32_t DiagnosticGraphics::Allocate(const X86Context& ctx, uint32_t bytes) {
  const uint32_t address = allocations_.allocate(ctx, bytes, 0x1000);
  if (!address) { throw Fault(ctx, device, bytes, "diagnostic graphics contiguous backing exhausted"); }
  allocationsOwned_.push_back(address);
  return address;
}
gpu::Surface DiagnosticGraphics::NewSurface(const X86Context& ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t bpp) {
  const uint32_t pitch = (width * bpp + 63) & ~63u;
  const uint32_t bytes = pitch * height;
  const uint32_t data = Allocate(ctx, bytes);
  const uint32_t header = Allocate(ctx, 24);
  const uint32_t encodedFormat = 0x10021 | (format << 8);
  const uint32_t encodedSize = (width - 1) | ((height - 1) << 12) | ((pitch / 64 - 1) << 24);
  const std::array<uint32_t, 6> words{0xd0001, data & 0xfffffff, 0, encodedFormat, encodedSize, 0};
  memory_->Write(ctx, header, words.data(), sizeof(words));
  resources_.push_back({header, data, bytes, encodedFormat, encodedSize, false});
  Record(Request::Resource, {header, data, bytes, encodedFormat, encodedSize});
  return {header, data & 0xfffffff, encodedFormat, encodedSize};
}
void DiagnosticGraphics::Capture(const X86Context& ctx) {
  if (!push_) { return; }
  uint32_t cursor;
  memory_->Read(ctx, device, &cursor, 4);
  if (cursor < push_ || uint64_t(cursor) > uint64_t(push_) + pushBytes_ || (cursor - push_) % 4 || (cursor - push_) / 4 < capturedWords_) {
    throw Fault(ctx, cursor, 0, "diagnostic graphics command capture cursor escaped owned storage");
  }
  const uint32_t words = (cursor - push_) / 4;
  if (unsubmitted_) { Unavailable(Request::ReadUnavailable, push_, "unsubmitted guest commands have no available GPU results"); }
  if (words == capturedWords_) { return; }
  const size_t oldSize = commands_.size();
  commands_.resize(words);
  memory_->Read(ctx, push_ + capturedWords_ * 4, commands_.data() + oldSize, (words - capturedWords_) * 4);
  Record(Request::Commands, {push_ + capturedWords_ * 4, words - capturedWords_});
  capturedWords_ = words;
  if (!creating_) {
    if (mode_ == GraphicsMode::Rendering || mode_ == GraphicsMode::StateCapture) {
      ApplyCommands(ctx);
    } else {
      Invalidate();
    }
  }
}
void DiagnosticGraphics::ApplyCommands(const X86Context& ctx) {
  const auto words = std::span<const uint32_t>(commands_).subspan(decodedWords_);
  const auto result = ApplyPushState(state_, words);
  if (result.status != PushStateStatus::StateOnly) {
    unsubmitted_ = true;
    Invalidate();
    Record(Request::Unsupported, {uint32_t(result.status), result.method, uint32_t(result.wordOffset) + decodedWords_, uint32_t(words.size())});
    throw Fault(ctx, result.method, uint32_t(result.wordOffset) + decodedWords_, "guest command stream is not verified frontend state only");
  }
  decodedWords_ = capturedWords_;
}
void DiagnosticGraphics::CaptureState(const X86Context& ctx, bool forceViewport) {
  auto lease = memory_->Borrow();
  const auto surface = [&](uint32_t offset) {
    gpu::Surface result;
    memory_->Read(ctx, device + offset, &result.header, 4);
    if (result.header) {
      std::array<uint32_t, 5> header;
      memory_->Read(ctx, result.header, header.data(), sizeof(header));
      result.data = header[1];
      result.format = header[3];
      result.size = header[4];
    }
    return result;
  };
  state_.rt = surface(0x1a04);
  state_.zs = surface(0x1a08);
#ifdef SYM_D3D_RenderState
  memory_->Read(ctx, DG_ADDRESS(D3D_RenderState), state_.rs.data(), state_.rs.size() * sizeof(uint32_t));
#endif
#ifdef SYM_D3D_DeferredTextureState
  for (uint32_t n = 0; n < state_.tss.size(); ++n) {
    memory_->Read(ctx, DG_ADDRESS(D3D_DeferredTextureState) + n * 128, state_.tss[n].data(), gpu::kTssBumpEnvMat00 * sizeof(uint32_t));
  }
#endif
  if (!typedVertexShader_) { memory_->Read(ctx, device + 0x798, &state_.vs_handle, 4); }
  memory_->Read(ctx, device + 0xf88, state_.textures.data(), sizeof(state_.textures));
  memory_->Read(ctx, device + 0x7a0, &state_.index_buffer, 4);
  memory_->Read(ctx, device + 0x1c, &state_.base_vertex, 4);
#ifdef SYM_D3D_IndexData
  memory_->Read(ctx, DG_ADDRESS(D3D_IndexData), &state_.index_data, 4);
#endif
#ifdef SYM_D3D_Streams
  for (uint32_t n = 0; n < state_.streams.size(); ++n) {
    std::array<uint32_t, 3> stream;
    memory_->Read(ctx, DG_ADDRESS(D3D_Streams) + n * 12, stream.data(), sizeof(stream));
    state_.streams[n] = {stream[2], stream[0]};
  }
#endif
  std::array<uint32_t, 6> viewport;
  memory_->Read(ctx, device + 0xee0, viewport.data(), sizeof(viewport));
  const auto previous = state_.viewport;
  if (!typedViewport_) {
    state_.viewport = {viewport[0], viewport[1], viewport[2], viewport[3], std::bit_cast<float>(viewport[4]), std::bit_cast<float>(viewport[5])};
  }
  const auto& current = state_.viewport;
  const bool changedViewport = previous.x != current.x || previous.y != current.y || previous.w != current.w || previous.h != current.h ||
    std::bit_cast<uint32_t>(previous.min_z) != std::bit_cast<uint32_t>(current.min_z) ||
    std::bit_cast<uint32_t>(previous.max_z) != std::bit_cast<uint32_t>(current.max_z);
  for (uint32_t n = 0; n < 10; ++n) {
    const uint32_t target = n < 2 ? n + 2 : n < 6 ? n + 14 : n + 250;
    memory_->Read(ctx, device + 0xc60 + n * 64, state_.transforms[target].data(), 64);
  }
  state_.zscale = state_.zs.header && (((state_.zs.format >> 8) & 255) == 44 || ((state_.zs.format >> 8) & 255) == 48) ? 65535.0f : 16777215.0f;
  if (forceViewport || changedViewport) { UpdateViewportConstants(); }
}
void DiagnosticGraphics::UpdateViewportConstants() {
  state_.zscale = state_.zs.header && (((state_.zs.format >> 8) & 255) == 44 || ((state_.zs.format >> 8) & 255) == 48) ? 65535.0f : 16777215.0f;
  const auto& v = state_.viewport;
  state_.vsc[58] = {v.w * 0.5f, -float(v.h) * 0.5f, (v.max_z - v.min_z) * state_.zscale, 0};
  state_.vsc[59] = {v.x + state_.ss_offset_x + v.w * 0.5f, v.y + state_.ss_offset_y + v.h * 0.5f, v.min_z * state_.zscale, 0};
}
void DiagnosticGraphics::Reference(X86Context& ctx, uint8_t* base, uint32_t resource, bool add) {
  if (!resource) { return; }
#if defined(SYM_D3D_InternalAddRef) && defined(SYM_D3D_InternalRelease)
  const uint32_t target = add ? DG_ADDRESS(D3D_InternalAddRef) : DG_ADDRESS(D3D_InternalRelease);
  if (!runtime_.Find(target)) { Unavailable(Request::Unsupported, target, "graphics retained resource reference helper is unavailable"); }
  const auto original = ctx;
  try { runtime_.Call(ctx, base, target, {resource}); }
  catch (...) { ctx = original; throw; }
#else
  Unavailable(Request::Unsupported, resource, "graphics retained resource reference symbols are unavailable");
#endif
}
bool DiagnosticGraphics::TypedState(X86Context& ctx, uint8_t* base, uint32_t id) {
  uint32_t arguments;
  switch (id) {
    case 29: case 30: case 54: case 55: arguments = 0; break;
    case 31: case 32: case 34: case 38: case 39: case 50: case 52: case 63: arguments = 1; break;
    case 33: case 35: case 47: case 56: case 60: arguments = 3; break;
    case 36: case 37: case 45: case 46: case 48: case 51: case 57: case 58: case 59: arguments = 2; break;
    case 49: arguments = 4; break;
    default: return false;
  }
  const uint32_t cleanup = (arguments + 1) * 4;
  if (ctx.esp > UINT32_MAX - cleanup) { throw Fault(ctx, ctx.esp, cleanup, "graphics typed state cleanup overflow"); }
  std::array<uint32_t, 5> frame{};
  memory_->Read(ctx, ctx.esp, frame.data(), cleanup);
  if (id == 29) {
    state_.simple[(ctx.ecx & 0x1ffc) >> 2] = ctx.edx;
  } else if (id == 30 || id == 31 || id == 54 || id == 55) {
    const uint32_t requested = id == 31 ? frame[1] / 4 : id == 55 ? 4 : 1;
    const uint32_t count = ctx.ecx < state_.vsc.size() ? std::min(requested, uint32_t(state_.vsc.size()) - ctx.ecx) : 0;
    std::array<std::array<float, 4>, 192> values{};
    if (count) {
      memory_->Read(ctx, ctx.edx, values.data(), count * 16);
      std::copy_n(values.begin(), count, state_.vsc.begin() + ctx.ecx);
    }
  } else if (id == 34) {
    std::array<uint32_t, 6> v;
    memory_->Read(ctx, frame[1], v.data(), sizeof(v));
    state_.viewport = {v[0], v[1], v[2], v[3], std::bit_cast<float>(v[4]), std::bit_cast<float>(v[5])};
    typedViewport_ = true;
    state_.scissor_count = 0;
    UpdateViewportConstants();
  } else if (id == 35) {
    const uint32_t count = std::min(frame[1], 8u);
    auto scissors = state_.scissors;
    if (count) { memory_->Read(ctx, frame[3], scissors.data(), count * 16); }
    state_.scissors = scissors;
    state_.scissor_count = count;
  } else if (id == 36) {
    state_.ss_offset_x = std::bit_cast<float>(frame[1]);
    state_.ss_offset_y = std::bit_cast<float>(frame[2]);
    UpdateViewportConstants();
  } else if (id == 37 && frame[1] < 10) {
    std::array<float, 16> matrix;
    memory_->Read(ctx, frame[2], matrix.data(), sizeof(matrix));
    memory_->Write(ctx, device + 0xc60 + frame[1] * 64, matrix.data(), sizeof(matrix));
    const uint32_t type = frame[1];
    state_.transforms[type < 2 ? type + 2 : type < 6 ? type + 14 : type + 250] = matrix;
  } else if (id == 38) {
    auto material = state_.material;
    memory_->Read(ctx, frame[1], material.data(), sizeof(material));
    state_.material = material;
  } else if (id == 39) {
    std::shared_ptr<gpu::VertexShader> shader;
    if (frame[1] & 1) {
      const auto entry = vertexShaders_.find(frame[1]);
      if (entry == vertexShaders_.end()) { Unavailable(Request::Unsupported, frame[1], "graphics vertex shader handle is not owned"); }
      shader = entry->second;
    }
    state_.vs_handle = frame[1];
    state_.vs = std::move(shader);
    if (state_.vs) {
      for (const auto& [reg, value] : state_.vs->constants) {
        if (reg < state_.vsc.size()) { state_.vsc[reg] = value; }
      }
    }
    typedVertexShader_ = true;
  } else if (id == 49 || id == 51) {
    const uint32_t output = id == 49 ? frame[3] : frame[2];
    memory_->CheckRange(ctx, output, 4);
    if (id == 49) {
      auto shader = std::make_shared<gpu::VertexShader>(ReadVertexShader(*memory_, ctx, 3, frame[1], frame[2]));
      const uint32_t record = Allocate(ctx, 16);
      shader->handle = record | 1;
      vertexShaders_.emplace(shader->handle, shader);
      memory_->Write(ctx, output, &shader->handle, 4);
    } else {
      auto shader = std::make_shared<gpu::PixelShader>(ReadPixelShader(*memory_, ctx, 2, frame[1]));
      shader->handle = Allocate(ctx, 16);
      pixelShaders_.emplace(shader->handle, shader);
      memory_->Write(ctx, output, &shader->handle, 4);
    }
  } else if (id == 50 || id == 52) {
    const uint32_t handle = frame[1];
    const bool owned = id == 50 ? vertexShaders_.contains(handle) : pixelShaders_.contains(handle);
    if (owned) {
      const uint32_t record = id == 50 ? handle & ~1u : handle;
      allocations_.release(ctx, record);
      std::erase(allocationsOwned_, record);
      if (id == 50) { vertexShaders_.erase(handle); }
      else { pixelShaders_.erase(handle); }
    }
  } else if (id == 32) {
    std::shared_ptr<gpu::PixelShader> shader;
    if (frame[1]) {
      const auto entry = pixelShaders_.find(frame[1]);
      if (entry == pixelShaders_.end()) { Unavailable(Request::Unsupported, frame[1], "graphics pixel shader handle is not owned"); }
      shader = entry->second;
#ifdef SYM_D3D_RenderState
      memory_->CheckRange(ctx, DG_ADDRESS(D3D_RenderState) + gpu::kRsPsTextureModes * 4, 4);
#else
      Unavailable(Request::Unsupported, id, "graphics render state storage is unavailable");
#endif
    }
    state_.ps = std::move(shader);
    if (state_.ps) {
      const auto* definition = state_.ps->def;
      const auto put = [&](uint32_t method, uint32_t value) { state_.simple[method >> 2] = value; };
      for (uint32_t n = 0; n < 8; ++n) {
        put(gpu::kMPsAlphaInputs + n * 4, definition[n]);
        put(gpu::kMPsConstant0 + n * 4, definition[10 + n]);
        put(gpu::kMPsConstant1 + n * 4, definition[18 + n]);
        put(gpu::kMPsAlphaOutputs + n * 4, definition[26 + n]);
        put(gpu::kMPsRgbInputs + n * 4, definition[34 + n]);
        put(gpu::kMPsRgbOutputs + n * 4, definition[45 + n]);
      }
      put(gpu::kMPsFinalAbcd, definition[8]);
      put(gpu::kMPsFinalEfgh, definition[9]);
      put(gpu::kMPsCompareMode, definition[42]);
      put(gpu::kMPsFinalConstant0, definition[43]);
      put(gpu::kMPsFinalConstant1, definition[44]);
      put(gpu::kMPsCombinerCount, definition[53]);
      put(gpu::kMPsDotMapping, definition[55]);
      put(gpu::kMPsInputTexture, definition[56]);
#ifdef SYM_D3D_RenderState
      memory_->Write(ctx, DG_ADDRESS(D3D_RenderState) + gpu::kRsPsTextureModes * 4, &definition[54], 4);
#endif
      state_.rs[gpu::kRsPsTextureModes] = definition[54];
    }
  } else if (id == 33) {
    const uint32_t count = frame[1] < state_.psc.size() ? std::min(frame[3], uint32_t(state_.psc.size()) - frame[1]) : 0;
    std::array<std::array<float, 4>, 16> values{};
    if (count) {
      memory_->Read(ctx, frame[2], values.data(), count * 16);
      std::copy_n(values.begin(), count, state_.psc.begin() + frame[1]);
    }
    ApplyDiagnosticPixelConstants(state_, frame[1], frame[3]);
  } else if (id == 45) {
    const auto surface = [&](uint32_t header) {
      gpu::Surface value{};
      value.header = header;
      if (header) {
        std::array<uint32_t, 5> words;
        memory_->Read(ctx, header, words.data(), sizeof(words));
        if ((words[0] & 0x70000) != 0x50000 && (words[0] & 0x70000) != 0x40000) {
          Unavailable(Request::Unsupported, header, "graphics render target requires a surface or texture header");
        }
        value.data = words[1]; value.format = words[3]; value.size = words[4];
      }
      return value;
    };
    std::array<uint32_t, 2> old;
    memory_->Read(ctx, device + 0x1a04, old.data(), sizeof(old));
    const auto rt = surface(frame[1] ? frame[1] : old[0]);
    const auto zs = surface(frame[2]);
    if (!rt.header) { Unavailable(Request::Unsupported, id, "graphics render target requires a non-null color target"); }
    memory_->CheckRange(ctx, device + 0x1a04, 8);
    Reference(ctx, base, rt.header, true);
    Reference(ctx, base, zs.header, true);
    Reference(ctx, base, old[0], false);
    Reference(ctx, base, old[1], false);
    const std::array<uint32_t, 2> targets{rt.header, zs.header};
    memory_->Write(ctx, device + 0x1a04, targets.data(), sizeof(targets));
    state_.rt = rt; state_.zs = zs;
    const uint32_t width = rt.size ? (rt.size & 0xfff) + 1 : 1u << ((rt.format >> 20) & 15);
    const uint32_t height = rt.size ? ((rt.size >> 12) & 0xfff) + 1 : 1u << ((rt.format >> 24) & 15);
    state_.viewport = {0, 0, width, height, 0, 1};
    typedViewport_ = true;
    UpdateViewportConstants();
  } else if (id == 46 || id == 47 || id == 48) {
    const uint32_t slot = frame[1] & (id == 47 ? 15 : 3);
    uint32_t address;
    if (id == 46) { address = device + 0xf88 + slot * 4; }
    else if (id == 48) { address = device + 0x7a0; }
    else {
#ifdef SYM_D3D_Streams
      address = DG_ADDRESS(D3D_Streams) + slot * 12 + 8;
#else
      Unavailable(Request::Unsupported, id, "graphics stream metadata is unavailable");
#endif
    }
    const uint32_t resource = id == 48 ? frame[1] : frame[2];
    uint32_t old;
    memory_->Read(ctx, address, &old, 4);
    uint32_t data = 0;
    if (resource) {
      std::array<uint32_t, 2> header;
      memory_->Read(ctx, resource, header.data(), sizeof(header));
      data = header[1];
    }
    memory_->CheckRange(ctx, address, 4);
    uint32_t dirty = 0;
    if (id == 46) {
#ifdef SYM_D3D_DirtyFlags
      memory_->Read(ctx, DG_ADDRESS(D3D_DirtyFlags), &dirty, 4);
#else
      Unavailable(Request::Unsupported, id, "graphics dirty flags are unavailable");
#endif
    }
    if (id == 47) { memory_->CheckRange(ctx, address - 8, 4); }
    if (id == 48) {
#ifdef SYM_D3D_IndexData
      memory_->CheckRange(ctx, DG_ADDRESS(D3D_IndexData), 4);
#else
      Unavailable(Request::Unsupported, id, "graphics index metadata is unavailable");
#endif
      memory_->CheckRange(ctx, device + 0x1c, 4);
    }
    if (resource != old) {
      Reference(ctx, base, resource, true);
      Reference(ctx, base, old, false);
    }
    memory_->Write(ctx, address, &resource, 4);
    if (id == 46) {
#ifdef SYM_D3D_DirtyFlags
      dirty |= 0x4800;
      memory_->Write(ctx, DG_ADDRESS(D3D_DirtyFlags), &dirty, 4);
#endif
      state_.textures[slot] = resource;
    } else if (id == 47) {
      memory_->Write(ctx, address - 8, &frame[3], 4);
      state_.streams[slot] = {resource, frame[3]};
    } else {
#ifdef SYM_D3D_IndexData
      memory_->Write(ctx, DG_ADDRESS(D3D_IndexData), &data, 4);
#endif
      memory_->Write(ctx, device + 0x1c, &frame[2], 4);
      state_.index_buffer = resource; state_.index_data = data; state_.base_vertex = frame[2];
    }
  } else if (id == 63) {
    const uint32_t mode = frame[1] & ~0x10u;
    if (!mode) { Unavailable(Request::Unsupported, frame[1], "graphics fixed-function constant mode requires unimplemented matrix and lighting initialization"); }
    uint32_t flags;
    memory_->Read(ctx, device + 8, &flags, 4);
    memory_->CheckRange(ctx, device + 0x1928, 4);
    flags = (flags & ~0x200u) | ((frame[1] & 0x10) ? 0x200u : 0u);
    memory_->Write(ctx, device + 8, &flags, 4);
    memory_->Write(ctx, device + 0x1928, &mode, 4);
  } else if (id >= 56 && id <= 60) {
    const uint32_t stage = frame[1] & 3;
    const uint32_t type = id == 56 || id == 60 ? frame[2] : id == 57 ? gpu::kTssBorderColor : id == 58 ? gpu::kTssColorKeyColor : gpu::kTssTexCoordIndex;
    const uint32_t value = id == 56 || id == 60 ? frame[3] : frame[2];
    const bool supported = id == 56 ? type >= gpu::kTssBumpEnvMat00 && type <= gpu::kTssBumpEnvLOffset : type < gpu::kTssCount;
    if (supported) {
      if (id == 59 || id == 60) {
#ifdef SYM_D3D_DeferredTextureState
        memory_->Write(ctx, DG_ADDRESS(D3D_DeferredTextureState) + (stage * 32 + type) * 4, &value, 4);
#else
        Unavailable(Request::Unsupported, id, "graphics texture state storage is unavailable");
#endif
      }
      state_.tss[stage][type] = value;
    }
  }
  ctx.eax = 0;
  ctx.esp += cleanup;
  return true;
}
void DiagnosticGraphics::CreateDevice(X86Context& ctx, uint8_t* base) {
  Require(ctx, base);
  std::lock_guard lock(lock_);
  if (failed_) { throw Fault(ctx, device, 0, "genuine renderer initialization failure is terminal"); }
  if (initialized_ || creating_) { throw Fault(ctx, device, 0, "diagnostic graphics device already exists"); }
  if (ctx.esp > UINT32_MAX - 28) { throw Fault(ctx, ctx.esp, 28, "diagnostic graphics stdcall cleanup overflow"); }
  std::array<uint32_t, 7> frame;
  std::array<uint32_t, 13> pp;
  uint32_t savedPointer = 0;
  {
    auto lease = memory_->Borrow();
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    memory_->Read(ctx, frame[5], pp.data(), sizeof(pp));
    memory_->CheckRange(ctx, frame[6], 4);
  }
  if (mode_ == GraphicsMode::Checkpoint) {
    if (!backend_->Init(pp[0], pp[1])) { Record(Request::Init, {pp[0], pp[1]}); }
    throw GraphicsCheckpoint(ctx);
  }
  std::vector<uint8_t> saved(rollbackBytes);
  {
    auto lease = memory_->Borrow();
    memory_->Read(ctx, rollbackBase, saved.data(), saved.size());
    memory_->Read(ctx, devicePointer, &savedPointer, 4);
  }
  const bool capture = mode_ == GraphicsMode::StateCapture || mode_ == GraphicsMode::Rendering;
  const bool presentation = capture
    ? frame[4] == 0x50 && pp == std::array<uint32_t, 13>{640, 480, 18, 1, 0, 1, 0, 0, 1, 46, 320, 0, 0x80000002}
    : frame[4] == 0x40 && pp[0] && pp[0] <= 2048 && pp[1] && pp[1] <= 2048 && pp[2] == 18 && pp[3] == 1 &&
      !pp[4] && !pp[5] && !pp[6] && !pp[7] && !pp[8] && !pp[9] && !pp[10] && !pp[11] && !pp[12];
  if (frame[1] != 0 || frame[2] != 1 || frame[3] != 0 || !frame[6] || !presentation) {
    std::ostringstream diagnostic;
    diagnostic << "diagnostic graphics unsupported CreateDevice presentation contract callers=" << std::hex;
    uint32_t cursor = ctx.ebp;
    for (uint32_t n = 0; cursor && n < 12; ++n) {
      try {
        std::array<uint32_t, 2> caller{};
        memory_->Read(ctx, cursor, caller.data(), sizeof(caller));
        diagnostic << caller[1] << ',';
        if (caller[0] <= cursor || caller[0] - cursor > 0x10000) { break; }
        cursor = caller[0];
      } catch (const Fault&) { break; }
    }
    throw Fault(ctx, frame[5], sizeof(pp), diagnostic.str().c_str());
  }
  const auto overlaps = [](uint32_t left, uint32_t leftBytes, uint32_t right, uint32_t rightBytes) {
    return uint64_t(left) < uint64_t(right) + rightBytes && uint64_t(right) < uint64_t(left) + leftBytes;
  };
  const auto inAllocationArena = [&](uint32_t address, uint32_t bytes) {
    return overlaps(address, bytes, 0x80100000, 0x3f00000) || overlaps(address, bytes, 0xf0100000, 0x3f00000);
  };
  if (ctx.esp < 12 || inAllocationArena(ctx.esp - 12, 40) || inAllocationArena(frame[5], 52) || inAllocationArena(frame[6], 4) ||
      ((ctx.esp | frame[5] | frame[6]) & 3) ||
      overlaps(ctx.esp, 28, rollbackBase, rollbackBytes) || overlaps(frame[5], 52, rollbackBase, rollbackBytes) ||
      overlaps(frame[6], 4, rollbackBase, rollbackBytes) || overlaps(frame[6], 4, ctx.esp, 28) ||
      overlaps(frame[6], 4, frame[5], 52) || overlaps(frame[5], 52, ctx.esp, 28)) {
    throw Fault(ctx, ctx.esp, 28, "diagnostic graphics aliased or unaligned CreateDevice frame");
  }
  memory_->CheckRange(ctx, ctx.esp - 12, 12);
  if (!runtime_.Find(DG_ADDRESS(D3D_InitDefaultState))) { throw Fault(ctx, device, 0, "retained graphics defaults are unavailable"); }
  const auto original = ctx;
  creating_ = true;
  try {
    push_ = Allocate(ctx, captureBytes);
    pushBytes_ = captureBytes;
    const auto front = NewSurface(ctx, pp[0], pp[1], 18, 4);
    const auto back = NewSurface(ctx, pp[0], pp[1], 18, 4);
    gpu::Surface depth{};
    if (pp[8]) { depth = NewSurface(ctx, pp[0], pp[1], 46, 4); }
    std::vector<uint8_t> metadata(deviceBytes);
    const auto put = [&](uint32_t offset, uint32_t value) { std::memcpy(metadata.data() + offset, &value, 4); };
    put(0, push_);
    put(4, push_ + captureBytes - 0x1000);
    put(0x1a04, back.header);
    put(0x1a08, depth.header);
    put(0x1a14, back.header);
    put(0x1a18, front.header);
    put(0x1a20, depth.header);
    put(0x794, device + 0x2678);
    put(0x948, 0);
    put(0x95c, std::bit_cast<uint32_t>(1.0f));
    put(0x960, std::bit_cast<uint32_t>(1.0f));
    put(0x964, std::bit_cast<uint32_t>(1.0f));
    put(0x96c, std::bit_cast<uint32_t>(1.0f));
    put(0x970, std::bit_cast<uint32_t>(1.0f));
    put(0xee8, pp[0]);
    put(0xeec, pp[1]);
    put(0xef4, std::bit_cast<uint32_t>(1.0f));
    memory_->Write(ctx, device, metadata.data(), metadata.size());
    memory_->Write(ctx, devicePointer, &device, 4);
    if (!Backend().Init(pp[0], pp[1])) { throw Fault(ctx, device, 0, "graphics backend initialization failed"); }
    state_ = gpu::State{};
    state_.viewport = {0, 0, pp[0], pp[1], 0, 1};
    state_.rt = back;
    state_.zs = depth;
#ifdef SYM_D3DDevice_SetVertexShader
    runtime_.Call(ctx, base, DG_ADDRESS(D3DDevice_SetVertexShader), {4});
    runtime_.Call(ctx, base, DG_ADDRESS(D3DDevice_SetVertexShader), {2});
#endif
#ifdef SYM_D3DDevice_SetRenderTarget
    runtime_.Call(ctx, base, DG_ADDRESS(D3DDevice_SetRenderTarget), {back.header, depth.header});
#endif
    runtime_.Call(ctx, base, DG_ADDRESS(D3D_InitDefaultState), {});
    Capture(ctx);
    if (capture) {
      ApplyCommands(ctx);
      CaptureState(ctx, true);
    }
#ifdef SYM_D3D_RenderState
    memory_->Read(ctx, DG_ADDRESS(D3D_RenderState), state_.rs.data(), state_.rs.size() * sizeof(uint32_t));
#endif
#ifdef SYM_D3D_DeferredTextureState
    for (uint32_t n = 0; n < 4; ++n) {
      memory_->Read(ctx, DG_ADDRESS(D3D_DeferredTextureState) + n * 128, state_.tss[n].data(), gpu::kTssBumpEnvMat00 * sizeof(uint32_t));
    }
#endif
    {
      auto lease = memory_->Borrow();
      runtime_.Checkpoint(ctx);
      memory_->CheckRange(ctx, frame[6], 4);
      memory_->Write(ctx, frame[6], &device, 4);
      initialized_ = true;
      creating_ = false;
      ctx = original;
      ctx.eax = 0;
      ctx.esp += 28;
    }
    (void)front;
  } catch (...) {
    const auto failure = std::current_exception();
    try {
      auto lease = memory_->Borrow();
      memory_->Write(original, rollbackBase, saved.data(), saved.size());
      memory_->Write(original, devicePointer, &savedPointer, 4);
    } catch (...) {}
    for (auto address = allocationsOwned_.rbegin(); address != allocationsOwned_.rend(); ++address) {
      try { allocations_.release(original, *address); } catch (...) {}
    }
    allocationsOwned_.clear();
    resources_.clear();
    push_ = pushBytes_ = capturedWords_ = decodedWords_ = 0;
    unsubmitted_ = false;
    failed_ = bool(renderer_);
    commands_.clear();
    backend_->Reset();
    initialized_ = false;
    creating_ = false;
    ctx = original;
    std::rethrow_exception(failure);
  }
}
bool DiagnosticGraphics::Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) try {
  Require(ctx, base);
  if (id == 3) { CreateDevice(ctx, base); return false; }
  std::lock_guard lock(lock_);
  if (id == 68) {
    if (!initialized_ || (mode_ != GraphicsMode::Rendering && mode_ != GraphicsMode::StateCapture)) {
      Unavailable(Request::Unsupported, id, "overlay control requires an initialized presentation owner");
    }
    if (ctx.esp > UINT32_MAX - 8) { throw Fault(ctx, ctx.esp, 8, "overlay control cleanup overflow"); }
    std::array<uint32_t, 2> frame;
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    if (frame[1]) { Unavailable(Request::Unsupported, id, "enabled overlay requires an implemented compositor", {id, frame[1]}); }
    ctx.eax = 0;
    ctx.esp += 8;
    return false;
  }
  if (id == 66 || id == 67) {
    if (!resourceReleaseDepth) { throw Fault(ctx, ctx.esp, 0, "resource release observation requires its original call scope"); }
    auto& release = resourceReleases[resourceReleaseDepth - 1];
    if (release.context != &ctx || release.base != base) { throw Fault(ctx, ctx.esp, 0, "resource release observation belongs to another worker"); }
    if (id == 66) {
      if (release.owner || release.completed || release.stack != ctx.esp || ctx.esp > UINT32_MAX - 8) {
        throw Fault(ctx, ctx.esp, 8, "invalid resource release entry frame");
      }
      std::array<uint32_t, 2> frame;
      memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      release.header = frame[1];
      memory_->Read(ctx, release.header, release.words.data(), sizeof(release.words));
      if ((release.words[0] & 0x70000) == 0x50000) {
        if (release.header > UINT32_MAX - 24) { throw Fault(ctx, release.header, 24, "resource release surface header overflow"); }
        memory_->Read(ctx, release.header + 20, &release.parent, 4);
        if (release.parent) { memory_->Read(ctx, release.parent, release.parentWords.data(), sizeof(release.parentWords)); }
      }
      release.lock = std::unique_lock(lock_);
      release.owner = this;
      return true;
    }
    if (release.owner != this || !release.completed || ctx.esp != release.stack + 8) {
      throw Fault(ctx, ctx.esp, 0, "resource release completion has no matching completed original");
    }
    release.owner = nullptr;
    const auto retire = [&](uint32_t header, uint32_t data) {
      const uint32_t address = 0x80000000 | (data & 0x3ffffff);
      if (renderer_ && renderer_->ReleaseResource(header, address)) { gpuWritten_.erase(address); }
    };
    const uint32_t references = release.words[0] & 0x78ffff;
    if (release.internal) {
      if ((references & 0x780000) == 0x80000 && release.parent && (release.parentWords[0] & 0x78ffff) == 0x80000) {
        retire(release.parent, release.parentWords[1]);
      }
      if (references == 0x80000) { retire(release.header, release.words[1]); }
    } else if (references == 1 && ctx.eax == 0) {
      retire(release.header, release.words[1]);
    }
    return true;
  }
  if ((creating_ || initialized_) && (mode_ == GraphicsMode::StateCapture || mode_ == GraphicsMode::Rendering) && id == 18) {
    if (!creating_) { Capture(ctx); }
    if (ctx.esp > UINT32_MAX - 12) { throw Fault(ctx, ctx.esp, 12, "graphics state setter cleanup overflow"); }
    std::array<uint32_t, 3> frame;
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    if (frame[1] < gpu::kRsDeferredFirst) {
#ifdef SYM_D3D_SimpleRenderStateEncode
      uint32_t method;
      memory_->Read(ctx, DG_ADDRESS(D3D_SimpleRenderStateEncode) + frame[1] * 4, &method, 4);
      state_.simple[(method & 0x1ffc) >> 2] = frame[2];
#else
      Unavailable(Request::Unsupported, frame[1], "graphics simple state encoding table is unavailable");
#endif
    } else if (frame[1] < gpu::kRsMax) {
#ifdef SYM_D3D_RenderState
      memory_->Write(ctx, DG_ADDRESS(D3D_RenderState) + frame[1] * 4, &frame[2], 4);
#endif
      state_.rs[frame[1]] = frame[2];
    }
    ctx.eax = 0;
    ctx.esp += 12;
    return false;
  }
  if (creating_ && (id == 18 || id >= 29)) { return true; }
  if (id >= 29 && mode_ != GraphicsMode::Rendering && mode_ != GraphicsMode::StateCapture) { return true; }
  Capture(ctx);
  if (initialized_ && (mode_ == GraphicsMode::Rendering || mode_ == GraphicsMode::StateCapture) && TypedState(ctx, base, id)) { return false; }
  if (initialized_ && id >= 29) {
    Unavailable(Request::Unsupported, id, "graphics typed frontend state operation is not implemented");
  }
  if (initialized_ && (mode_ == GraphicsMode::Rendering || mode_ == GraphicsMode::StateCapture) && (id == 8 || id == 10 || id == 11)) {
    if (ctx.esp > UINT32_MAX - 8) { throw Fault(ctx, ctx.esp, 8, "graphics resource cleanup overflow"); }
    std::array<uint32_t, 2> frame;
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    uint32_t result = 0;
    if (frame[1]) {
      std::array<uint32_t, 2> header;
      memory_->Read(ctx, frame[1], header.data(), sizeof(header));
      const uint32_t type = header[0] & 0x70000;
      const uint32_t data = 0x80000000 | (header[1] & 0x3ffffff);
      const bool written = gpuWritten_.contains(data);
      if (id == 11) {
        uint32_t parent = 0;
        if (type == 0x50000) {
          memory_->Read(ctx, frame[1] + 20, &parent, 4);
          if (parent) { memory_->Read(ctx, parent, &parent, 4); }
        }
        result = (header[0] & 0x780000) || (parent & 0x780000) || (written && renderer_ && renderer_->Busy(frame[1]));
      } else if (written) {
        if (!renderer_) { Unavailable(Request::ReadUnavailable, frame[1], "resource requires unavailable GPU results"); }
        if (id == 8 && (type == 0x40000 || type == 0x50000)) {
          memory_->CheckRange(ctx, data, 1);
          renderer_->Sync(data, 0);
        } else {
          renderer_->WaitResource(frame[1]);
        }
      }
    }
    ctx.eax = result;
    ctx.esp += 8;
    return false;
  }
  if (initialized_ && (mode_ == GraphicsMode::Rendering || mode_ == GraphicsMode::StateCapture) && id == 9) {
    std::array<uint32_t, 7> frame;
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    std::array<uint32_t, 6> header;
    memory_->Read(ctx, frame[1], header.data(), sizeof(header));
    std::array<uint32_t, 2> output;
    memory_->Read(ctx, frame[4], output.data(), sizeof(output));
    if (frame[5]) {
      std::array<uint32_t, 4> rect;
      memory_->Read(ctx, frame[5], rect.data(), sizeof(rect));
    }
    const uint32_t data = 0x80000000 | (header[1] & 0x3ffffff);
    memory_->CheckRange(ctx, data, 1);
    if ((frame[6] & 0x20) && gpuWritten_.contains(data)) {
      if (!renderer_) { Unavailable(Request::ReadUnavailable, frame[1], "CPU lock requires unavailable GPU results"); }
      renderer_->SyncLate(data);
    }
    return true;
  }
  const auto stopSdk = [&](const char* reason) {
    constexpr std::array<uint32_t, 29> counts{0, 1, 2, 6, 0, 2, 0, 0, 1, 6, 1, 1, 0, 1, 1, 0, 1, 6, 2, 3, 3, 5, 1, 0, 1, 2, 0, 0, 2};
    const uint32_t count = id < counts.size() ? std::min(counts[id], 5u) : 0;
    std::array<uint32_t, 6> frame{};
    memory_->Read(ctx, ctx.esp, frame.data(), size_t(count + 1) * 4);
    frame[0] = id;
    Unavailable(Request::Unsupported, id, reason, frame);
  };
  if (initialized_ && mode_ == GraphicsMode::Rendering) {
    if (id == 15) {
      if (ctx.esp > UINT32_MAX - 4) { throw Fault(ctx, ctx.esp, 4, "graphics PersistDisplay cleanup overflow"); }
      memory_->CheckRange(ctx, ctx.esp, 4);
      if (resources_.size() < 2) { throw Fault(ctx, device, 0, "PersistDisplay requires its owned front surface"); }
      const auto& front = resources_.front();
      std::array<uint32_t, 5> header;
      memory_->Read(ctx, front.header, header.data(), sizeof(header));
      const uint32_t data = 0x80000000 | (header[1] & 0x3ffffff);
      const uint32_t pitch = ((header[4] >> 24) + 1) * 64;
      const uint32_t height = ((header[4] >> 12) & 0xfff) + 1;
      const uint64_t bytes = uint64_t(pitch) * height;
      if ((header[0] & 0x70000) != 0x50000 || data != front.data || header[3] != front.format ||
          header[4] != front.size || bytes != front.bytes || bytes > 0x800000) {
        throw Fault(ctx, front.header, 20, "PersistDisplay front metadata escaped owned storage");
      }
      memory_->CheckRange(ctx, data, size_t(bytes));
      PersistedDisplay display{header[0], header[3], header[4], std::vector<uint8_t>(size_t(bytes))};
      renderer_->Sync(data, uint32_t(bytes));
      memory_->Read(ctx, data, display.pixels.data(), display.pixels.size());
      persistedDisplay_ = std::move(display);
      Record(Request::PersistDisplay, {front.header, data, uint32_t(bytes), header[3], header[4]});
      ctx.eax = 0;
      ctx.esp += 4;
      return false;
    }
    if (id == 4) {
      if (ctx.esp > UINT32_MAX - 4) { throw Fault(ctx, ctx.esp, 4, "graphics MakeSpace cleanup overflow"); }
      memory_->CheckRange(ctx, ctx.esp, 4);
      if (!push_ || decodedWords_ != capturedWords_ || unsubmitted_) {
        throw Fault(ctx, push_, 0, "graphics MakeSpace requires fully decoded owned state");
      }
      memory_->Write(ctx, device, &push_, 4);
      commands_.clear();
      capturedWords_ = decodedWords_ = 0;
      ctx.eax = push_;
      ctx.esp += 4;
      return false;
    }
    if (id == 7) {
      if (ctx.esp > UINT32_MAX - 4) { throw Fault(ctx, ctx.esp, 4, "graphics idle cleanup overflow"); }
      memory_->CheckRange(ctx, ctx.esp, 4);
      renderer_->Flush();
      Record(Request::Flush);
      ctx.eax = 0;
      ctx.esp += 4;
      return false;
    }
    CaptureState(ctx);
    if (id == 17) {
      if (ctx.esp > UINT32_MAX - 28) { throw Fault(ctx, ctx.esp, 28, "graphics Clear cleanup overflow"); }
      std::array<uint32_t, 7> frame;
      memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      if (frame[1] > 4096 || (frame[1] && !frame[2])) { throw Fault(ctx, frame[2], frame[1], "graphics Clear rectangle limit or pointer invalid"); }
      std::vector<uint8_t> rects(size_t(frame[1]) * 16);
      if (!rects.empty()) { memory_->Read(ctx, frame[2], rects.data(), rects.size()); }
      renderer_->Clear(state_, frame[1], rects.empty() ? nullptr : rects.data(), frame[3], frame[4], std::bit_cast<float>(frame[5]), frame[6]);
      if ((frame[3] & gpu::kClearTarget) && state_.rt.header) { gpuWritten_.insert(0x80000000 | (state_.rt.data & 0x3ffffff)); }
      if ((frame[3] & (gpu::kClearZ | gpu::kClearStencil)) && state_.zs.header) { gpuWritten_.insert(0x80000000 | (state_.zs.data & 0x3ffffff)); }
      Record(Request::Clear, {frame[1], frame[3], frame[4], frame[5], frame[6], state_.rt.header});
      ctx.eax = 0;
      ctx.esp += 28;
      return false;
    }
    if (id == 21) {
      if (ctx.esp > UINT32_MAX - 24) { throw Fault(ctx, ctx.esp, 24, "graphics CopyRects cleanup overflow"); }
      std::array<uint32_t, 6> frame;
      memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      if (frame[3] > 4096 || (frame[3] && !frame[2]) || (!frame[3] && (frame[2] || frame[5]))) {
        throw Fault(ctx, frame[2], frame[3], "graphics CopyRects invalid rectangle list");
      }
      if (frame[3]) {
        memory_->CheckRange(ctx, frame[2], size_t(frame[3]) * 16);
        if (frame[5]) { memory_->CheckRange(ctx, frame[5], size_t(frame[3]) * 8); }
      }
      const auto surface = [&](uint32_t address) {
        if (!address) { throw Fault(ctx, address, 20, "graphics CopyRects requires non-null surface headers"); }
        std::array<uint32_t, 5> header;
        memory_->Read(ctx, address, header.data(), sizeof(header));
        const uint32_t type = header[0] & 0x70000;
        if (type != 0x50000 && type != 0x40000) { throw Fault(ctx, address, 20, "graphics CopyRects requires a surface or texture header"); }
        return gpu::Surface{address, header[1], header[3], header[4]};
      };
      const auto source = surface(frame[1]), destination = surface(frame[4]);
      renderer_->Copy(source, destination, frame[3], frame[2], frame[5]);
      const uint32_t data = 0x80000000 | (destination.data & 0x3ffffff);
      if (renderer_->HasGpuContents(destination.data)) { gpuWritten_.insert(data); }
      else { gpuWritten_.erase(data); }
      Record(Request::Copy, {source.header, destination.header, frame[3], frame[2], frame[5]});
      ctx.eax = 0;
      ctx.esp += 24;
      return false;
    }
    if (id == 19 || id == 20) {
      if (ctx.esp > UINT32_MAX - 16) { throw Fault(ctx, ctx.esp, 16, "graphics draw cleanup overflow"); }
      std::array<uint32_t, 4> frame;
      memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      const uint32_t count = id == 20 ? frame[2] : frame[3];
      if (count > 0x100000) { throw Fault(ctx, ctx.esp, count, "graphics draw count limit exceeded"); }
      std::vector<uint8_t> indices;
      if (id == 20 && frame[3]) {
        indices.resize(size_t(count) * 2);
        memory_->Read(ctx, frame[3], indices.data(), indices.size());
      }
      renderer_->Draw(state_, frame[1], id == 20 ? 0 : frame[2], count, indices.empty() ? nullptr : indices.data());
      if (state_.rt.header) { gpuWritten_.insert(0x80000000 | (state_.rt.data & 0x3ffffff)); }
      if (state_.zs.header) { gpuWritten_.insert(0x80000000 | (state_.zs.data & 0x3ffffff)); }
      Record(Request::Draw, {frame[1], id == 20 ? 0 : frame[2], count, state_.rt.header, state_.zs.header, id == 20 ? frame[3] : 0});
      ctx.eax = 0;
      ctx.esp += 16;
      return false;
    }
    if (id == 28) {
      if (ctx.esp > UINT32_MAX - 12) { throw Fault(ctx, ctx.esp, 12, "graphics Gamma cleanup overflow"); }
      std::array<uint32_t, 3> frame;
      memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      std::array<uint8_t, 768> ramp;
      memory_->Read(ctx, frame[2], ramp.data(), ramp.size());
      renderer_->SetGamma(ramp.data());
      Record(Request::Gamma, {frame[1], frame[2]});
      ctx.eax = 0;
      ctx.esp += 12;
      return false;
    }
    if (id == 16) {
      if (ctx.esp > UINT32_MAX - 8) { throw Fault(ctx, ctx.esp, 8, "graphics Swap cleanup overflow"); }
      std::array<uint32_t, 2> frame;
      memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
      if (resources_.size() < 2) { throw Fault(ctx, device, 0, "graphics Swap requires its color surface ring"); }
      std::array<std::array<uint32_t, 2>, 2> payload;
      for (uint32_t n = 0; n < payload.size(); ++n) {
        memory_->Read(ctx, resources_[n].header + 4, payload[n].data(), sizeof(payload[n]));
      }
      std::array<uint32_t, 5> header;
      memory_->Read(ctx, resources_[1].header, header.data(), sizeof(header));
      const gpu::Surface front{resources_[1].header, header[1], header[3], header[4]};
      uint32_t count;
      memory_->Read(ctx, device + 0x2478, &count, 4);
      renderer_->Present(front, false);
      for (uint32_t n = 0; n < payload.size(); ++n) {
        const auto& next = payload[1 - n];
        memory_->Write(ctx, resources_[n].header + 4, next.data(), sizeof(next));
        resources_[n].data = 0x80000000 | (next[0] & 0x3ffffff);
      }
      ++count;
      memory_->Write(ctx, device + 0x2478, &count, 4);
      CaptureState(ctx);
      Record(Request::Present, {front.header, 0, count, front.data});
      ctx.eax = count;
      ctx.esp += 8;
      ++completedPresents_;
      if (presentCheckpoint_ && completedPresents_ == presentCheckpoint_) { throw GraphicsPresentCheckpoint(ctx, completedPresents_); }
      return false;
    }
    stopSdk("rendering SDK operation is not implemented");
  }
  if (initialized_ && mode_ == GraphicsMode::StateCapture) {
    CaptureState(ctx);
    stopSdk("state capture stopped before an unrendered SDK operation");
  }
  if (initialized_ && (id == 8 || id == 9 || id == 10)) {
    std::array<uint32_t, 2> frame;
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    const auto resource = std::find_if(resources_.begin(), resources_.end(), [&](const auto& entry) { return entry.header == frame[1]; });
    if (resource == resources_.end()) { Unavailable(Request::Unsupported, frame[1], "diagnostic CPU lock requires an owned resource"); }
    std::array<uint32_t, 5> header;
    memory_->Read(ctx, resource->header, header.data(), sizeof(header));
    if (header[1] != (resource->data & 0xfffffff) || header[3] != resource->format || header[4] != resource->size) {
      Unavailable(Request::Unsupported, resource->header, "diagnostic CPU resource storage metadata changed");
    }
    if (id == 9) {
      std::array<uint32_t, 7> lockFrame;
      memory_->Read(ctx, ctx.esp, lockFrame.data(), sizeof(lockFrame));
      if (lockFrame[2] || lockFrame[3] || lockFrame[5] || (lockFrame[6] & ~uint32_t(0x60)) || !lockFrame[4] || (lockFrame[4] & 3)) {
        throw Fault(ctx, ctx.esp, sizeof(lockFrame), "diagnostic CPU lock only supports a whole owned level-zero surface");
      }
      memory_->CheckRange(ctx, lockFrame[4], 8);
      backend_->Sync(resource->data, resource->bytes);
      return true;
    }
    backend_->Sync(resource->data, resource->bytes);
    if (ctx.esp > UINT32_MAX - 8) { throw Fault(ctx, ctx.esp, 8, "diagnostic graphics resource cleanup overflow"); }
    ctx.eax = 0;
    ctx.esp += 8;
    return false;
  }
  if (initialized_ && id == 16) {
    uint32_t target;
    memory_->Read(ctx, device + 0x1a04, &target, 4);
    if (target != state_.rt.header) { Unavailable(Request::Unsupported, target, "diagnostic graphics Swap target metadata changed"); }
    std::array<uint32_t, 2> frame;
    if (ctx.esp > UINT32_MAX - 8) { throw Fault(ctx, ctx.esp, 8, "diagnostic graphics Swap cleanup overflow"); }
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    if (frame[1] || resources_.size() != 2) { Unavailable(Request::Unsupported, frame[1], "diagnostic graphics Swap only supports its two owned surfaces and zero flags"); }
    auto& front = resources_[0];
    auto& back = resources_[1];
    uint32_t count;
    {
      auto lease = memory_->Borrow();
      memory_->Read(ctx, device + 0x2478, &count, 4);
      if (count == UINT32_MAX) { throw Fault(ctx, device + 0x2478, 4, "diagnostic graphics swap count exhausted"); }
      for (const auto& resource : resources_) {
        std::array<uint32_t, 5> header;
        memory_->Read(ctx, resource.header, header.data(), sizeof(header));
        if (header[1] != (resource.data & 0xfffffff) || header[2] || header[3] != resource.format || header[4] != resource.size) {
          throw Fault(ctx, resource.header, 20, "diagnostic graphics Swap resource storage metadata changed");
        }
      }
    }
    backend_->Present(state_.rt, false);
    {
      auto lease = memory_->Borrow();
      runtime_.Checkpoint(ctx);
      memory_->CheckRange(ctx, front.header + 4, 4);
      memory_->CheckRange(ctx, back.header + 4, 4);
      memory_->CheckRange(ctx, device + 0x2478, 4);
      const uint32_t frontData = back.data & 0xfffffff;
      const uint32_t backData = front.data & 0xfffffff;
      memory_->Write(ctx, front.header + 4, &frontData, 4);
      memory_->Write(ctx, back.header + 4, &backData, 4);
      memory_->Write(ctx, device + 0x2478, &(++count), 4);
      std::swap(front.data, back.data);
      std::swap(front.bytes, back.bytes);
      std::swap(front.unavailable, back.unavailable);
      state_.rt.data = backData;
      ctx.eax = count;
      ctx.esp += 8;
    }
    Record(Request::CpuSwap, {front.header, front.data, back.header, back.data, count});
    return false;
  }
  if (initialized_ && id == 17) {
    if (ctx.esp > UINT32_MAX - 28) { throw Fault(ctx, ctx.esp, 28, "diagnostic graphics Clear cleanup overflow"); }
    std::array<uint32_t, 7> frame;
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    if (frame[1] || frame[2] || frame[3] != gpu::kClearTarget) {
      throw Fault(ctx, ctx.esp, 28, "diagnostic graphics only records whole-target Clear requests");
    }
    uint32_t target;
    memory_->Read(ctx, device + 0x1a04, &target, 4);
    if (target != state_.rt.header) { Unavailable(Request::Unsupported, target, "diagnostic CPU clear target metadata changed"); }
    std::array<uint32_t, 4> viewport;
    memory_->Read(ctx, device + 0xee0, viewport.data(), sizeof(viewport));
    if (viewport[0] || viewport[1] || viewport[2] != state_.viewport.w || viewport[3] != state_.viewport.h) {
      Unavailable(Request::Unsupported, device + 0xee0, "diagnostic CPU clear requires the unchanged full viewport");
    }
    backend_->Clear(state_, 0, nullptr, frame[3], frame[4], std::bit_cast<float>(frame[5]), frame[6]);
    ctx.eax = 0;
    ctx.esp += 28;
    return false;
  }
  Unavailable(Request::Unsupported, id, id == 4 || id == 5 ? "diagnostic graphics bounded command capture is full" :
    "diagnostic graphics SDK operation is not implemented");
} catch (const Cancelled&) {
  throw;
} catch (const Fault& error) {
  if (error.context->host) { throw; }
  throw Fault(ctx, error.address, error.size, error.what());
} catch (const std::exception& error) {
  throw Fault(ctx, 0, 0, error.what());
}
DiagnosticGraphics::SnapshotData DiagnosticGraphics::Snapshot() const {
  std::lock_guard lock(lock_);
  SnapshotData result;
  result.initialized = initialized_;
  result.total = total_;
  result.count = size_t(std::min<uint64_t>(total_, events_.size()));
  result.resources = resources_;
  result.completedPresents = completedPresents_;
  result.commandWords = capturedWords_;
  result.commands = commands_;
  if (persistedDisplay_) {
    result.persistedDisplay = {persistedDisplay_->common, persistedDisplay_->format, persistedDisplay_->size, uint32_t(persistedDisplay_->pixels.size())};
  }
  for (size_t n = 0; n < result.count; ++n) { result.events[n] = events_[(total_ - result.count + n) % events_.size()]; }
  return result;
}
std::optional<DiagnosticGraphics::PersistedDisplay> DiagnosticGraphics::PersistedDisplaySnapshot() const {
  std::lock_guard lock(lock_);
  return persistedDisplay_;
}
gpu::Backend& DiagnosticGraphics::Backend() { return renderer_ ? *renderer_ : *backend_; }
gpu::State DiagnosticGraphics::State() const {
  std::lock_guard lock(lock_);
  return state_;
}
gpu::State DiagnosticGraphics::SnapshotState() const {
  std::lock_guard lock(lock_);
  auto result = state_;
  if (result.vs) { result.vs = std::make_shared<gpu::VertexShader>(*result.vs); }
  if (result.ps) { result.ps = std::make_shared<gpu::PixelShader>(*result.ps); }
  return result;
}

}
