#pragma once
#include "browser/image.h"

namespace browser {

struct SynchronizationHandleRoutes {
  std::function<bool(uint32_t)> ownsFile;
  std::function<bool(uint32_t)> ownsThread;
};

std::function<bool(uint32_t)> InstallSynchronizationProvider(Runtime& runtime, PreparedImage& image,
  std::shared_ptr<SynchronizationHandleRoutes> routes = {});

}
