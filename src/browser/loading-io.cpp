#include "browser/cpu_runtime.h"
#include "cpu/x86_ops.h"
#include <array>

X86_FUNC(__imp__sub_004B8EC0);
X86_FUNC(sub_004B8EC0) {
  browser::Memory::PauseExecution pause;
  auto& runtime = browser::Runtime::From(ctx);
  if (!base || !runtime.HasPublishedImage() || !runtime.OwnsCurrentContext(ctx) || browser::Memory::HasBorrow()) {
    throw browser::Fault(ctx, 0x4b8ec0, 0, "loading work query requires its published current guest worker without a lease");
  }
  runtime.Checkpoint(ctx);
  bool held = false;
  {
    if (uint64_t(ctx.esp) + 12 > UINT32_MAX) { throw browser::Fault(ctx, ctx.esp, 12, "loading work query frame overflow"); }
    auto& memory = browser::Memory::From(base);
    auto lease = memory.Borrow();
    std::array<uint32_t, 3> frame{};
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    if (frame[1] < 0x20c) { throw browser::Fault(ctx, frame[1], 4, "loading work query has an invalid Bink task pointer"); }
    const uint32_t bink = frame[1] - 0x20c;
    uint32_t reader = 0, readsHeld = 0, readEnd = 0;
    memory.Read(ctx, bink + 0x120, &reader, 4);
    if (reader == 0x35aa1) {
      memory.Read(ctx, 0x5e3e24, &readsHeld, 4);
      if (!readsHeld) { memory.Read(ctx, bink + 0x12c, &readEnd, 4); }
      held = readsHeld || readEnd;
    }
  }
  runtime.Checkpoint(ctx);
  if (held) {
    ctx.eax = 0;
    ctx.esp += 12;
    return;
  }
  __imp__sub_004B8EC0(ctx, base);
}
