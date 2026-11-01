#pragma once
#include "browser/image.h"

namespace browser {

void InstallDpcProvider(Runtime& runtime, PreparedImage& image);
bool DeliverImageDpc(PreparedImage& image, Runtime& runtime, const X86Context& interrupted, uint32_t address);
void DeliverLockedTimerDpc(PreparedImage& image, Runtime& runtime, const X86Context& interrupted,
  uint32_t address, uint64_t systemTime, const std::unique_lock<std::mutex>& delivery);

}
