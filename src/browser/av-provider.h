#pragma once
#include "browser/cpu_runtime.h"

namespace browser {

void InstallAvProvider(Runtime& runtime, Memory& memory, std::function<uint32_t()> restoredDisplay = {});

}
