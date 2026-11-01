#pragma once
#include "browser/image.h"

namespace browser {

void InstallThreadProvider(Runtime& runtime, PreparedImage& image, std::function<bool(uint32_t)> closeFile = {});

}
