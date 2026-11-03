#pragma once

#include "browser/image.h"
#include "kernel/disc_index.h"
#include "browser/temporary-filesystem.h"

namespace browser {

struct DiscReadSnapshot {
  uint64_t assetBytes = 0;
  uint64_t assetOperations = 0;
  uint64_t executableBytes = 0;
  uint64_t executableOperations = 0;
};

struct FileOpenDiagnostic {
  std::string originalPath;
  std::string resolvedPath;
  uint32_t status = 0;
  uint32_t root = 0;
  uint32_t disposition = 0;
  uint32_t options = 0;
  uint32_t access = 0;
  uint32_t attributes = 0;
  uint64_t allocationBytes = 0;
};

std::function<bool(uint32_t)> InstallFileProvider(Runtime& runtime, PreparedImage& image,
  std::shared_ptr<const kernel::DiscIndex> disc, std::shared_ptr<TemporaryFilesystem> temporary = {},
  std::vector<KernelDataExport>* dataExports = nullptr,
  std::function<DiscReadSnapshot()>* discReads = nullptr,
  std::function<void(FileOpenDiagnostic)> fileOpenDiagnostic = {},
  std::function<bool(uint32_t)>* ownsHandle = nullptr);
std::shared_ptr<const kernel::DiscIndex> MountedIsoDisc();

}
