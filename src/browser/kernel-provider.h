#pragma once
#include <functional>
#include "browser/cpu_runtime.h"

namespace browser {

inline constexpr uint32_t KernelPoolBase = 0xb0000000;
inline constexpr uint32_t KernelPoolCapacity = 8u << 20;
inline constexpr uint32_t KernelShaContextBytes = 116;
inline constexpr uint32_t KernelVideoFlags = 0x00080000;
inline constexpr uint32_t KernelVideoPack = 4;

// language: the XC_LANGUAGE id ExQueryNonVolatileSetting index 7 answers (1 English), asked at the game's query
void InstallKernelProvider(Runtime& runtime, Memory& memory, std::function<uint32_t()> language = {});

}
