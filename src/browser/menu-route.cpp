#include "browser/menu-route.h"
#include "cpu/x86_ops.h"

X86_FUNC(__imp__sub_0018697A);
X86_FUNC(sub_0018697A) {
  if (browser::Runtime::ObserveSdkEntry(ctx, base, 65)) { __imp__sub_0018697A(ctx, base); }
}

namespace browser {
namespace {
constexpr uint32_t pressHandler = 0x18697a;
constexpr uint32_t pressBase = 0x2758;
constexpr uint32_t gameModePage = 0x2788;
constexpr uint32_t isPage = 0x28fa86;
constexpr uint32_t isElement = 0x1a6090;
constexpr uint32_t findList = 0x1a60e6;
constexpr uint32_t pageName = 0x541a98;
constexpr uint32_t listName = 0x541c28;
constexpr uint32_t selectionOffset = 0x9c;
}

MenuRoute::MenuRoute(Runtime& runtime, PreparedImage& image)
  : runtime_(runtime), image_(image), memory_(image.memory.get()) {
  runtime.Checkpoint({});
  if (!memory_ || image.importsBound || image.boundRuntime || runtime.HasPublishedImage() || runtime.ActiveWorkers() ||
      Memory::HasBorrow() || (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) ||
      runtime.Find(pressHandler) != sub_0018697A || !runtime.Find(isPage) || !runtime.Find(isElement) || !runtime.Find(findList)) {
    throw Fault({}, 0, 0, "menu route requires its cold owned generated image and native helpers");
  }
  memory_->CheckRange({}, pageName, sizeof("P_OnlineOffline"));
  memory_->CheckRange({}, listName, sizeof("Lst_OfflineOnline"));
  image.diagnosticRuntime = &runtime;
}

bool MenuRoute::BelongsTo(Runtime& runtime, PreparedImage& image) const {
  return &runtime == &runtime_ && &image == &image_ && image.memory.get() == memory_;
}

MenuRoute::SnapshotData MenuRoute::Snapshot() const {
  std::lock_guard lock(lock_);
  return snapshot_;
}

uint32_t MenuRoute::Call(X86Context& ctx, uint8_t* base, uint32_t target, uint32_t self, std::initializer_list<uint32_t> arguments) {
  struct Restore {
    X86Context& context;
    X86Context saved;
    ~Restore() { context = saved; }
  } restore{ctx, ctx};
  ctx.ecx = self;
  const auto result = runtime_.Call(ctx, base, target, arguments);
  runtime_.Checkpoint(ctx);
  return result;
}

bool MenuRoute::Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) {
  if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ ||
      base != memory_->Base() || !runtime_.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, ctx.esp, 0, "menu route requires its bound current image worker without a lease");
  }
  runtime_.Checkpoint(ctx);
  if (id != 65) { return true; }
  if (uint64_t(ctx.esp) + 8 > UINT32_MAX || ctx.esp < 12 || ctx.ecx < pressBase ||
      uint64_t(ctx.ecx - pressBase) + gameModePage + 4 > UINT32_MAX) {
    throw Fault(ctx, ctx.esp, 8, "menu route frame or menu address overflow");
  }
  const uint32_t menu = ctx.ecx - pressBase;
  std::array<uint32_t, 2> frame{};
  {
    auto lease = memory_->Borrow();
    memory_->Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    memory_->CheckRange(ctx, ctx.esp - 12, 12);
  }
  if (!(Call(ctx, base, isPage, menu, {pageName}) & 0xff)) { return true; }
  if (!Call(ctx, base, isElement, ctx.ecx, {frame[1], listName})) { return true; }
  uint32_t page = 0;
  {
    auto lease = memory_->Borrow();
    memory_->Read(ctx, menu + gameModePage, &page, sizeof(page));
  }
  if (!page) { return true; }
  const uint32_t list = Call(ctx, base, findList, ctx.ecx, {page, listName});
  if (!list) { return true; }
  if (uint64_t(list) + selectionOffset + 2 > UINT32_MAX) {
    throw Fault(ctx, list, 2, "menu route selection address overflow");
  }
  int16_t selection = 0;
  {
    auto lease = memory_->Borrow();
    memory_->Read(ctx, list + selectionOffset, &selection, sizeof(selection));
  }
  if (selection != 1 && selection != 2) { return true; }
  runtime_.Checkpoint(ctx);
  {
    std::lock_guard lock(lock_);
    ++snapshot_.rejectedAttempts;
    snapshot_.lastSelection = uint32_t(selection);
  }
  ctx.eax = 1;
  ctx.esp += 8;
  return false;
}

}
