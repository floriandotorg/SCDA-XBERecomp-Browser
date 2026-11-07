#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace browser {

class TemporaryFilesystem {
public:
  static constexpr uint32_t AllocationUnit = 16384;
  static constexpr uint64_t MaximumCapacity = 512ull * 1024 * 1024;
  struct Metadata {
    bool directory;
    uint64_t size;
    uint32_t attributes;
    uint64_t creationTime;
    uint64_t lastAccessTime;
    uint64_t lastWriteTime;
    uint64_t changeTime;
    uint64_t allocationSize = 0;
  };
  struct DirectoryEntry {
    std::string name;
    Metadata metadata;
  };
  struct Accounting {
    uint64_t capacity;
    uint64_t allocated;
    size_t entries;
  };
  explicit TemporaryFilesystem(uint64_t capacity = 16 * 1024 * 1024, size_t maximumEntries = 256, std::function<uint64_t()> clock = {});
  std::optional<Metadata> Stat(const std::string& path) const;
  std::optional<std::vector<DirectoryEntry>> List(const std::string& path) const;
  Accounting Usage() const;
  uint32_t CreateDirectory(const std::string& path);
  uint32_t CreateFile(const std::string& path, uint32_t attributes, uint64_t allocation = 0);
  uint32_t OverwriteFile(const std::string& path, uint32_t attributes);
  uint32_t Delete(const std::string& path);
  uint32_t Rename(const std::string& source, const std::string& destination, bool replace);
  uint32_t Resize(const std::string& path, uint64_t size);
  uint32_t Reserve(const std::string& path, uint64_t size);
  uint32_t SetBasic(const std::string& path, const std::array<uint64_t, 4>& times, uint32_t attributes);
  uint32_t WriteFile(const std::string& path, std::span<const uint8_t> bytes);
  uint32_t Write(const std::string& path, uint64_t offset, std::span<const uint8_t> bytes);
  size_t Read(const std::string& path, uint64_t offset, std::span<uint8_t> output) const;
  uint64_t Revision() const { return revision_.load(std::memory_order_acquire); }
  std::shared_ptr<void> HoldWriter();
  size_t Writers() const { return writers_->load(std::memory_order_acquire); }
  std::vector<uint8_t> Serialize(uint64_t* revision = nullptr) const;
  uint32_t Restore(std::span<const uint8_t> image);

private:
  struct Node {
    bool directory;
    std::vector<uint8_t> bytes;
    uint32_t attributes;
    uint64_t creationTime;
    uint64_t lastAccessTime;
    uint64_t lastWriteTime;
    uint64_t changeTime;
    uint64_t reservation = 0;
  };
  mutable std::mutex lock_;
  mutable std::map<std::string, Node> nodes_;
  std::function<uint64_t()> clock_;
  Node NewNode(bool directory, uint32_t attributes, std::vector<uint8_t> bytes = {}) const;
  uint64_t capacity_;
  uint64_t allocated_ = AllocationUnit;
  size_t maximumEntries_;
  std::atomic<uint64_t> revision_ = 0;
  std::shared_ptr<std::atomic<size_t>> writers_ = std::make_shared<std::atomic<size_t>>(0);
  static std::optional<std::string> Normalize(std::string path);
  static uint64_t Allocation(uint64_t size);
  uint32_t ParentStatus(const std::string& path) const;
};

}
