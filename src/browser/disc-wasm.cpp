#include "browser/file-provider.h"
#include <emscripten.h>
#include <array>
#include <cmath>

uint32_t readIso(double offset, void* destination, uint32_t length) {
  return MAIN_THREAD_EM_ASM_INT({
    try {
      return Module['readIsoBytes']($0, $1 >>> 0, $2);
    } catch (error) {
      Module['isoError'] = error.code ?? 'READ_FAILED';
      return 0;
    }
  }, offset, destination, length);
}

namespace {
std::shared_ptr<kernel::DiscIndex> disc = std::make_shared<kernel::DiscIndex>();
std::array<char, 240> pathBuffer;
std::array<uint8_t, 1u << 20> readBuffer;
const kernel::DiscIndex::Entry* file = nullptr;
}

namespace browser {
std::shared_ptr<const kernel::DiscIndex> MountedIsoDisc() { return disc; }
}

extern "C" {

EMSCRIPTEN_KEEPALIVE int mount_disc(double size) {
  file = nullptr;
  disc = std::make_shared<kernel::DiscIndex>();
  if (!std::isfinite(size) || size < 0 || size > 9007199254740991.0 || std::floor(size) != size) { return 0; }
  std::string error;
  return disc->Open(uint64_t(size), [](uint64_t offset, void* destination, uint32_t length) {
    return readIso(double(offset), destination, length);
  }, error);
}

EMSCRIPTEN_KEEPALIVE double disc_partition() { return double(disc->Partition()); }
EMSCRIPTEN_KEEPALIVE char* validation_path() { return pathBuffer.data(); }
EMSCRIPTEN_KEEPALIVE uint8_t* validation_buffer() { return readBuffer.data(); }

EMSCRIPTEN_KEEPALIVE int validation_find(uint32_t length) {
  file = length <= pathBuffer.size() ? disc->Find(std::string_view(pathBuffer.data(), length)) : nullptr;
  return !file ? 0 : file->dir ? 2 : 1;
}

EMSCRIPTEN_KEEPALIVE double validation_size() { return file ? file->size : 0; }

EMSCRIPTEN_KEEPALIVE uint32_t validation_read(uint32_t position, uint32_t length) {
  if (!file || length > readBuffer.size()) { return 0; }
  return disc->Read(*file, position, readBuffer.data(), length);
}

}
