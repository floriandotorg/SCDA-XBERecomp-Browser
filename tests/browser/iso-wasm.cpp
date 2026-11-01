#include "browser/iso-disc.h"
#include <emscripten.h>
#include <array>
#include <memory>

EM_JS(uint32_t, readIso, (double offset, void* destination, uint32_t length), {
  try {
    return Module['readIsoBytes'](offset, destination >>> 0, length);
  } catch (error) {
    Module['isoError'] = error.code ?? 'READ_FAILED';
    return 0;
  }
});

namespace {
std::unique_ptr<browser::Memory> memory;
std::unique_ptr<browser::Runtime> runtime;
kernel::DiscIndex disc;
X86Context context{};
std::array<char, 240> validationPath;
std::array<uint8_t, 1u << 20> validationBuffer;
const kernel::DiscIndex::Entry* validationFile = nullptr;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE int mount_disc(double size) {
  validationFile = nullptr;
  memory = std::make_unique<browser::Memory>();
  memory->Ensure(0x10000, 4096);
  runtime = std::make_unique<browser::Runtime>(std::span<const X86FuncMapping>{}, std::span<const uint32_t>{});
  std::string error;
  return disc.Open(uint64_t(size), [](uint64_t offset, void* destination, uint32_t length) {
    return readIso(double(offset), destination, length);
  }, error);
}

EMSCRIPTEN_KEEPALIVE double disc_partition() { return double(disc.Partition()); }

EMSCRIPTEN_KEEPALIVE char* validation_path() { return validationPath.data(); }
EMSCRIPTEN_KEEPALIVE uint8_t* validation_buffer() { return validationBuffer.data(); }

EMSCRIPTEN_KEEPALIVE int validation_find(uint32_t length) {
  validationFile = length <= validationPath.size() ? disc.Find(std::string_view(validationPath.data(), length)) : nullptr;
  return !validationFile ? 0 : validationFile->dir ? 2 : 1;
}

EMSCRIPTEN_KEEPALIVE double validation_size() { return validationFile ? validationFile->size : 0; }

EMSCRIPTEN_KEEPALIVE uint32_t validation_read(uint32_t position, uint32_t length) {
  if (!validationFile || length > validationBuffer.size()) { return 0; }
  return disc.Read(*validationFile, position, validationBuffer.data(), length);
}

EMSCRIPTEN_KEEPALIVE int read_guest(uint32_t destination, uint32_t length, uint32_t position) {
  const auto* file = disc.Find("/DEFAULT.XBE");
  if (!file) { return -2; }
  try {
    return int(browser::ReadDiscToGuest(disc, *file, position, *memory, *runtime, context, destination, length));
  } catch (const browser::Fault&) {
    return -1;
  }
}

EMSCRIPTEN_KEEPALIVE int check_guest() {
  std::array<uint8_t, 9> result{};
  memory->Read(context, 0x10000, result.data(), result.size());
  return result == std::array<uint8_t, 9>{0, 10, 20, 30, 40, 50, 60, 70, 0};
}

}
