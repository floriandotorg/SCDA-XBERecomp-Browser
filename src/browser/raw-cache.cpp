#include "browser/raw-cache.h"
#include <algorithm>

namespace browser {

RawCache::RawCache(uint64_t capacity) : partition_(std::make_shared<CacheFatx>(capacity)) {}

uint32_t RawCache::Read(uint64_t offset, std::span<uint8_t> output) const {
  if (offset != MetadataOffset || output.size() != SectorSize) { return 0xc000000d; }
  std::lock_guard lock(lock_);
  std::copy(metadata_.begin(), metadata_.end(), output.begin());
  return 0;
}

uint32_t RawCache::Write(uint64_t offset, std::span<const uint8_t> input) {
  if (offset != MetadataOffset || input.size() != SectorSize) { return 0xc000000d; }
  std::lock_guard lock(lock_);
  std::copy(input.begin(), input.end(), metadata_.begin());
  return 0;
}

std::shared_ptr<CacheFatx> RawCache::Partition(uint32_t partition) const {
  return partition == FirstPartition ? partition_ : nullptr;
}

}
