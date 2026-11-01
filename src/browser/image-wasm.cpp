#include "browser/image.h"
#include <emscripten.h>

namespace {
std::vector<uint8_t> input;
std::unique_ptr<browser::PreparedImage> prepared;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE void image_reset() {
  prepared.reset();
  std::vector<uint8_t>{}.swap(input);
}

EMSCRIPTEN_KEEPALIVE uint8_t* image_allocate(uint32_t size) {
  image_reset();
  if (!size || size > (64u << 20)) { return nullptr; }
  input.resize(size);
  return input.data();
}

EMSCRIPTEN_KEEPALIVE int image_prepare(uint32_t base, uint32_t end, uint32_t entry) {
  prepared.reset();
  try {
    prepared = browser::PrepareImage(std::move(input), {base, end, entry});
    return 1;
  } catch (const std::runtime_error&) {
    return 0;
  }
}

EMSCRIPTEN_KEEPALIVE uint32_t image_sections() { return prepared ? prepared->image.sections.size() : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_imports() { return prepared ? prepared->imports.size() : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_tls_bytes() { return prepared ? prepared->tlsTemplate.size() : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_backing() { return prepared ? prepared->memory->BackingBytes() : 0; }

}
