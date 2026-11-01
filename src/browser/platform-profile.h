#pragma once
#include "browser/image.h"
#include <array>

namespace browser {

std::array<KernelDataExport, 6> InstallPlatformProfile(Runtime& runtime, PreparedImage& image);
KernelDataExport InstallVirtualDriveKey(Runtime& runtime, PreparedImage& image);

}
