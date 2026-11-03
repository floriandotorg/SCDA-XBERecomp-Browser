#pragma once

#include "browser/cache-fatx.h"
#include <array>
#include <memory>
#include <mutex>
#include <span>

namespace browser {

class RawCache {
public:
  static constexpr uint32_t PartitionCount = 1;
  static constexpr uint32_t FirstPartition = 3;
  static constexpr uint64_t PartitionCapacity = CacheFatx::Capacity;
  static constexpr uint64_t MetadataOffset = 0x800;
  static constexpr uint32_t SectorSize = 512;
  static constexpr uint32_t MaximumTransfer = SectorSize;
  uint32_t Read(uint64_t offset, std::span<uint8_t> output) const;
  uint32_t Write(uint64_t offset, std::span<const uint8_t> input);
  std::shared_ptr<CacheFatx> Partition(uint32_t partition) const;

private:
  mutable std::mutex lock_;
  std::array<uint8_t, SectorSize> metadata_{};
  const std::shared_ptr<CacheFatx> partition_ = std::make_shared<CacheFatx>();
};

}
