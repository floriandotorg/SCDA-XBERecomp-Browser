#include "browser/diagnostic-graphics.h"
#include "cpu/x86_ops.h"
#include "image_symbols.h"
#include <algorithm>
#include <bit>

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
    if (indices) { owner_.Unavailable(Request::Unsupported, count, "diagnostic graphics indexed draws are not recorded"); }
    owner_.Record(Request::Draw, {primitive, start, count, state.rt.header, state.zs.header});
    owner_.Invalidate(state.rt.header);
    if (state.zs.header) { owner_.Invalidate(state.zs.header); }
  }
  void Copy(const gpu::Surface& source, const gpu::Surface& destination, uint32_t count, uint32_t rects, uint32_t points) override {
    std::lock_guard lock(owner_.lock_);
    owner_.Record(Request::Copy, {source.header, destination.header, count, rects, points});
    owner_.Invalidate(destination.header);
  }
  void Present(const gpu::Surface& surface, bool pillarbox) override {
    std::lock_guard lock(owner_.lock_);
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

DiagnosticGraphics::DiagnosticGraphics(Runtime& runtime, PreparedImage& image, ContiguousMemoryAccess allocations, GraphicsMode mode)
  : runtime_(runtime), image_(image), memory_(image.memory.get()), allocations_(std::move(allocations)), mode_(mode), backend_(std::make_unique<Recorder>(*this)) {
  if (!memory_ || !allocations_.allocate || !allocations_.release || image.importsBound || image.boundRuntime || runtime.HasPublishedImage() || runtime.ActiveWorkers() || Memory::HasBorrow()) {
    throw Fault({}, device, 0, "diagnostic graphics requires its cold owned image and allocator");
  }
}
DiagnosticGraphics::~DiagnosticGraphics() = default;

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
[[noreturn]] void DiagnosticGraphics::Unavailable(Request request, uint32_t address, const char* reason) {
  Record(request, {address});
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
  if (words == capturedWords_) { return; }
  const size_t oldSize = commands_.size();
  commands_.resize(words);
  memory_->Read(ctx, push_ + capturedWords_ * 4, commands_.data() + oldSize, (words - capturedWords_) * 4);
  Record(Request::Commands, {push_ + capturedWords_ * 4, words - capturedWords_});
  capturedWords_ = words;
  if (!creating_) { Invalidate(); }
}
void DiagnosticGraphics::CreateDevice(X86Context& ctx, uint8_t* base) {
  Require(ctx, base);
  std::lock_guard lock(lock_);
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
  if (frame[1] != 0 || frame[2] != 1 || frame[3] != 0 || frame[4] != 0x40 || !frame[6] ||
      pp[0] == 0 || pp[0] > 2048 || pp[1] == 0 || pp[1] > 2048 || pp[2] != 18 || pp[3] != 1 ||
      pp[4] || pp[5] || pp[6] || pp[7] || pp[8] || pp[9] || pp[10] || pp[11] || pp[12]) {
    throw Fault(ctx, frame[5], sizeof(pp), "diagnostic graphics unsupported CreateDevice presentation contract");
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
    if (!backend_->Init(pp[0], pp[1])) { throw Fault(ctx, device, 0, "diagnostic graphics recorder initialization failed"); }
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
#ifdef SYM_D3D_RenderState
    memory_->Read(ctx, DG_ADDRESS(D3D_RenderState), state_.rs.data(), state_.rs.size() * sizeof(uint32_t));
#endif
#ifdef SYM_D3D_DeferredTextureState
    for (uint32_t n = 0; n < 4; ++n) {
      memory_->Read(ctx, DG_ADDRESS(D3D_DeferredTextureState) + n * 128, state_.tss[n].data(), 32 * sizeof(uint32_t));
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
    push_ = pushBytes_ = capturedWords_ = 0;
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
  if (creating_ && id == 18) { return true; }
  Capture(ctx);
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
} catch (const Fault& error) {
  if (error.context->host) { throw; }
  throw Fault(ctx, error.address, error.size, error.what());
}
DiagnosticGraphics::SnapshotData DiagnosticGraphics::Snapshot() const {
  std::lock_guard lock(lock_);
  SnapshotData result;
  result.initialized = initialized_;
  result.total = total_;
  result.count = size_t(std::min<uint64_t>(total_, events_.size()));
  result.resources = resources_;
  result.commandWords = capturedWords_;
  result.commands = commands_;
  for (size_t n = 0; n < result.count; ++n) { result.events[n] = events_[(total_ - result.count + n) % events_.size()]; }
  return result;
}
gpu::Backend& DiagnosticGraphics::Backend() { return *backend_; }
gpu::State DiagnosticGraphics::State() const {
  std::lock_guard lock(lock_);
  return state_;
}

}
