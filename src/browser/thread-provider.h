#pragma once
#include "browser/image.h"

namespace browser {

struct ThreadSessionSnapshot {
  uint32_t childrenCreated = 0;
  uint32_t childrenCompleted = 0;
  uint32_t handlesClosed = 0;
  uint32_t childrenSuspended = 0;
  uint32_t objectReferences = 0;
  uint64_t requestedStackBytes = 0;
  uint64_t committedBytes = 0;
};

KernelDataExport InstallThreadProvider(Runtime& runtime, PreparedImage& image, std::function<bool(uint32_t)> closeOther = {},
  std::function<bool(uint32_t)>* ownsHandle = nullptr);
ThreadSessionSnapshot SnapshotThreadSession(const PreparedImage& image);

}
