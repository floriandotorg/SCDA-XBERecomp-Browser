#pragma once

#include <string>

#include "gpu/shaders.h"

namespace browser::menu {

struct PixelTranslation {
  bool supported = false;
  std::string body;
  std::string diagnostic;
};

PixelTranslation TranslatePixel(const gpu::PixelKey& key);

}
