#pragma once
#include "browser/cpu_runtime.h"
#include "browser/contiguous-memory.h"

namespace browser {

struct PreparedImage;

void InstallVirtualMemory(Runtime& runtime, Memory& memory);
void InstallVirtualMemory(Runtime& runtime, PreparedImage& image, QueryContiguousMemoryUsage contiguousUsage = {});

}
