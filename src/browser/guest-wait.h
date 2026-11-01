#pragma once
#include "browser/dispatcher.h"

namespace browser {

struct WaitWatchdogExpired : Fault {
  explicit WaitWatchdogExpired(const X86Context& ctx)
    : Fault(ctx, 0, 0, "guest wait host watchdog expired") {}
};

uint32_t WaitGuestStatus(Dispatcher& dispatcher, Memory& memory, const X86Context& ctx,
  std::span<const uint32_t> objects, Dispatcher::WaitMode mode, uint32_t timeoutAddress,
  bool alertable, std::chrono::milliseconds watchdog, uint32_t owner = 0);

}
