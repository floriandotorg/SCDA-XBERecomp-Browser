#pragma once
#include "browser/cpu_runtime.h"

namespace browser {

struct PersistentContiguousRange {
  uint32_t address;
  std::vector<uint8_t> bytes;
};

using SnapshotContiguousMemory = std::function<std::vector<PersistentContiguousRange>()>;

using QueryContiguousMemoryUsage = std::function<uint32_t(const X86Context&, Memory&)>;

struct ContiguousMemoryAccess {
  std::function<uint32_t(const X86Context&, uint32_t, uint32_t)> allocate;
  std::function<void(const X86Context&, uint32_t)> release;
};

SnapshotContiguousMemory InstallContiguousMemory(Runtime& runtime, Memory& memory, QueryContiguousMemoryUsage* usage = nullptr,
  ContiguousMemoryAccess* access = nullptr);

}
