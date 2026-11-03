#include "browser/av-provider.h"

namespace browser {
namespace {

struct SavedDisplay {
  uint32_t address = 0;

  void Invoke(Runtime& runtime, Memory& memory, X86Context& context, uint32_t ordinal) {
    if (!runtime.OwnsCurrentContext(context) || Memory::HasBorrow()) {
      throw Fault(context, ordinal, 0, "AV saved display requires its current owned context without a backing lease");
    }
    const uint32_t frameBytes = ordinal == 1 ? 4 : 8;
    const uint64_t stackEnd = uint64_t(context.esp) + frameBytes;
    if (stackEnd > UINT32_MAX) {
      throw Fault(context, context.esp, frameBytes, "AV saved display stack cleanup overflow");
    }
    auto lease = memory.Borrow();
    std::array<uint32_t, 2> frame{};
    memory.Read(context, context.esp, frame.data(), frameBytes);
    if (ordinal == 4 && frame[1]) {
      throw Fault(context, frame[1], 0, "AV saved display requires lifetime-bound guest storage; non-null persistence is unavailable");
    }
    runtime.Checkpoint(context);
    if (ordinal == 4) { address = 0; }
    context.eax = ordinal == 1 ? address : 0;
    context.esp = uint32_t(stackEnd);
  }
};

}

void InstallAvProvider(Runtime& runtime, Memory& memory) {
  runtime.Checkpoint({});
  if (runtime.ActiveWorkers() || runtime.HasPublishedImage() || Memory::HasBorrow() ||
      runtime.KernelTarget(1) || runtime.KernelTarget(4)) {
    throw Fault({}, 1, 0, "AV provider requires an unpublished stopped runtime and unused saved-display ordinals");
  }
  auto saved = std::make_shared<SavedDisplay>();
  for (const uint32_t ordinal : {1u, 4u}) {
    runtime.RegisterKernel(ordinal, [&runtime, &memory, saved, ordinal](X86Context& context, uint8_t* base) {
      if (context.host != &runtime || base != memory.Base()) {
        throw Fault(context, ordinal, 0, "AV saved display session mismatch");
      }
      saved->Invoke(runtime, memory, context, ordinal);
    });
  }
}

}
