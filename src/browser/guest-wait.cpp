#include "browser/guest-wait.h"
#include <bit>

namespace browser {

uint32_t WaitGuestStatus(Dispatcher& dispatcher, Memory& memory, const X86Context& ctx,
  std::span<const uint32_t> objects, Dispatcher::WaitMode mode, uint32_t timeoutAddress,
  bool alertable, std::chrono::milliseconds watchdog, uint32_t owner) {
  if (Memory::HasBorrow()) { throw Fault(ctx, timeoutAddress, 0, "guest wait requires no backing lease"); }
  if (alertable) { throw Fault(ctx, 0, 0, "alertable guest waits are not implemented"); }
  std::optional<int64_t> timeout;
  if (timeoutAddress) {
    std::array<uint8_t, 8> bytes{};
    memory.Read(ctx, timeoutAddress, bytes.data(), bytes.size());
    uint64_t bits = 0;
    for (size_t n = 0; n < bytes.size(); ++n) { bits |= uint64_t(bytes[n]) << (n * 8); }
    timeout = std::bit_cast<int64_t>(bits);
  }
  const auto outcome = dispatcher.WaitMultipleGuest(ctx, objects, mode, timeout, watchdog, owner);
  switch (outcome.result) {
    case Dispatcher::WaitResult::Signaled: return uint32_t(outcome.index.value_or(0));
    case Dispatcher::WaitResult::Abandoned: return 0x80 + uint32_t(outcome.index.value_or(0));
    case Dispatcher::WaitResult::TimedOut: return 0x102;
    case Dispatcher::WaitResult::WatchdogExpired: throw WaitWatchdogExpired(ctx);
    case Dispatcher::WaitResult::Destroyed: throw Fault(ctx, 0, 0, "guest wait object was internally retired");
  }
  throw Fault(ctx, 0, 0, "unknown guest wait outcome");
}

}
