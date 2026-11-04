#include "browser/cache-fatx.h"
#include <algorithm>
#include <new>
#include <stdexcept>

namespace browser {

CacheFatx::CacheFatx(uint64_t capacity) : capacity_(capacity) {
  if (capacity != Capacity && capacity != StoryCapacity) { throw std::invalid_argument("unsupported cache capacity profile"); }
}

bool CacheFatx::ValidTransfer(uint64_t offset, size_t length) const {
  return length && length <= MaximumTransfer && !(offset % SectorSize) && !(length % SectorSize) &&
    offset <= capacity_ && length <= capacity_ - offset;
}

bool CacheFatx::ValidFormat() const {
  if (bytes_.size() != capacity_) { return false; }
  const auto read32 = [&](size_t offset) {
    uint32_t value = 0;
    for (uint32_t n = 0; n < 4; ++n) { value |= uint32_t(bytes_[offset + n]) << (n * 8); }
    return value;
  };
  if (read32(0) != 0x58544146 || read32(8) != AllocationUnit / SectorSize ||
      read32(12) != 1 || bytes_[16] || bytes_[17] || read32(HeaderSize) != 0xfffffff8) { return false; }
  if (!std::all_of(bytes_.begin() + HeaderSize + 4, bytes_.begin() + DataOffset(),
      [](uint8_t value) { return value == 0; })) { return false; }
  return std::all_of(bytes_.begin() + DataOffset(), bytes_.begin() + DataOffset() + AllocationUnit,
    [](uint8_t value) { return value == 0xff; });
}

uint32_t CacheFatx::OpenRaw() {
  std::lock_guard lock(lock_);
  if (filesystem_) { return 0xc00000bb; }
  if (rawOpen_) { return 0xc0000043; }
  if (bytes_.empty()) {
    try {
      std::vector<uint8_t> bytes(capacity_);
      bytes_.swap(bytes);
    } catch (const std::bad_alloc&) { return 0xc000009a; }
  }
  rawOpen_ = true;
  dismounted_ = false;
  return 0;
}

void CacheFatx::CloseRaw() {
  std::lock_guard lock(lock_);
  rawOpen_ = false;
}

uint32_t CacheFatx::ReadRaw(uint64_t offset, std::span<uint8_t> output) const {
  if (!ValidTransfer(offset, output.size())) { return 0xc000000d; }
  std::lock_guard lock(lock_);
  if (!rawOpen_ || dismounted_ || filesystem_) { return 0xc00000a3; }
  std::copy_n(bytes_.begin() + offset, output.size(), output.begin());
  return 0;
}

uint32_t CacheFatx::WriteRaw(uint64_t offset, std::span<const uint8_t> input) {
  if (!ValidTransfer(offset, input.size())) { return 0xc000000d; }
  std::lock_guard lock(lock_);
  if (!rawOpen_ || dismounted_ || filesystem_) { return 0xc00000a3; }
  std::copy(input.begin(), input.end(), bytes_.begin() + offset);
  return 0;
}

uint32_t CacheFatx::Dismount() {
  std::lock_guard lock(lock_);
  if (!rawOpen_ || filesystem_) { return 0xc00000a3; }
  if (!ValidFormat()) { return 0xc000014f; }
  dismounted_ = true;
  return 0;
}

uint32_t CacheFatx::Mount() {
  std::lock_guard lock(lock_);
  if (filesystem_) { return 0; }
  if (rawOpen_ || !dismounted_) { return 0xc00000a3; }
  if (!ValidFormat()) { return 0xc000014f; }
  filesystem_ = std::make_shared<TemporaryFilesystem>((capacity_ - DataOffset()) / AllocationUnit * AllocationUnit);
  std::vector<uint8_t>().swap(bytes_);
  return 0;
}

bool CacheFatx::Mounted() const {
  std::lock_guard lock(lock_);
  return bool(filesystem_);
}

size_t CacheFatx::RawBackingBytes() const {
  std::lock_guard lock(lock_);
  return bytes_.capacity();
}

std::shared_ptr<TemporaryFilesystem> CacheFatx::Filesystem() const {
  std::lock_guard lock(lock_);
  return filesystem_;
}

}
