#include "browser/delay-provider.h"
#include <algorithm>
#include <bit>

namespace browser {
namespace {

void Delay(Runtime& runtime, X86Context& context, uint8_t* base) {
  if (!base || context.host != &runtime || !runtime.OwnsCurrentContext(context) || Memory::HasBorrow()) {
    throw Fault(context, 99, 0, "execution delay requires its owned runtime worker without a backing lease");
  }
  const uint64_t stackEnd = uint64_t(context.esp) + 16;
  if (stackEnd > UINT32_MAX) { throw Fault(context, context.esp, 16, "execution delay stack cleanup overflow"); }
  int64_t interval;
  {
    auto& memory = Memory::From(base);
    auto lease = memory.Borrow();
    std::array<uint8_t, 16> bytes{};
    memory.Read(context, context.esp, bytes.data(), bytes.size());
    std::array<uint32_t, 4> frame{};
    for (size_t n = 0; n < bytes.size(); ++n) { frame[n / 4] |= uint32_t(bytes[n]) << ((n % 4) * 8); }
    if (frame[1] > 1) { throw Fault(context, 99, 0, "invalid delay processor mode"); }
    if (frame[2] & 0xff) { throw Fault(context, 99, 0, "alertable execution delays are not implemented"); }
    if (!frame[3]) { throw Fault(context, 0, 8, "execution delay requires an interval pointer"); }
    std::array<uint8_t, 8> dueTime{};
    memory.Read(context, frame[3], dueTime.data(), dueTime.size());
    uint64_t bits = 0;
    for (size_t n = 0; n < dueTime.size(); ++n) { bits |= uint64_t(dueTime[n]) << (n * 8); }
    interval = std::bit_cast<int64_t>(bits);
  }
  if (interval > 0) { throw Fault(context, 99, 0, "absolute execution delays require a system time epoch"); }
  runtime.Checkpoint(context);
  if (interval == 0) {
    runtime.Poll(context, base);
    std::this_thread::yield();
  } else {
    const uint64_t deadline = runtime.RelativeTimerDeadline(context, interval);
    for (;;) {
      const uint64_t sequence = runtime.Sequence();
      runtime.Poll(context, base);
      const uint64_t now = runtime.TimerTime(context);
      if (now >= deadline) { break; }
      const uint64_t remaining = deadline - now;
      const uint64_t milliseconds = remaining / 10000 + uint64_t(remaining % 10000 != 0);
      runtime.Wait(context, sequence, std::chrono::milliseconds(std::min<uint64_t>(milliseconds, 10)));
    }
  }
  runtime.Checkpoint(context);
  context.eax = 0;
  context.esp = uint32_t(stackEnd);
}

}

void InstallDelayProvider(Runtime& runtime) {
  if (Memory::HasBorrow()) { throw Fault({}, 99, 0, "execution delay installation during backing lease"); }
  runtime.RegisterKernel(99, [&runtime](X86Context& context, uint8_t* base) { Delay(runtime, context, base); });
}

}
