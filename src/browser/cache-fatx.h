#pragma once

#include "browser/temporary-filesystem.h"
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace browser {

class CacheFatx {
public:
  static constexpr uint64_t Capacity = 64 * 1024 * 1024;
  static constexpr uint32_t SectorSize = 512;
  static constexpr uint32_t AllocationUnit = 16384;
  static constexpr uint32_t MaximumTransfer = 64 * 1024;
  uint32_t OpenRaw();
  void CloseRaw();
  uint32_t ReadRaw(uint64_t offset, std::span<uint8_t> output) const;
  uint32_t WriteRaw(uint64_t offset, std::span<const uint8_t> input);
  uint32_t Dismount();
  uint32_t Mount();
  bool Mounted() const;
  size_t RawBackingBytes() const;
  std::shared_ptr<TemporaryFilesystem> Filesystem() const;

private:
  static constexpr uint32_t HeaderSize = 4096;
  static constexpr uint32_t FatSize = (((Capacity / AllocationUnit + 1) * sizeof(uint16_t) + HeaderSize - 1) / HeaderSize) * HeaderSize;
  static constexpr uint32_t DataOffset = HeaderSize + FatSize;
  static constexpr uint64_t UsableCapacity = ((Capacity - DataOffset) / AllocationUnit) * AllocationUnit;
  mutable std::mutex lock_;
  std::vector<uint8_t> bytes_;
  std::shared_ptr<TemporaryFilesystem> filesystem_;
  bool rawOpen_ = false;
  bool dismounted_ = false;
  bool ValidFormat() const;
  static bool ValidTransfer(uint64_t offset, size_t length);
};

}
