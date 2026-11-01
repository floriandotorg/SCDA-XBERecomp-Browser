#pragma once

#include "browser/cpu_runtime.h"
#include "kernel/disc_index.h"

namespace browser {

uint32_t ReadDiscToGuest(const kernel::DiscIndex& disc, const kernel::DiscIndex::Entry& file,
  uint64_t position, Memory& memory, Runtime& runtime, const X86Context& context,
  uint32_t destination, uint32_t length);

}
