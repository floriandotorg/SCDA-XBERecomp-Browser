#pragma once
#include "browser/dispatcher-handles.h"

namespace browser {

void InvokeDispatcherWait(DispatcherHandles& handles, Memory& memory, X86Context& ctx,
  uint32_t ordinal, std::chrono::milliseconds watchdog);

}
