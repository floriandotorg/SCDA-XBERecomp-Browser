#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace browser {

class TemporaryFilesystem {
public:
  static constexpr uint32_t AllocationUnit = 16384;
  struct Metadata {
    bool directory;
    uint64_t size;
    uint32_t attributes;
    uint64_t creationTime;
    uint64_t lastAccessTime;
    uint64_t lastWriteTime;
    uint64_t changeTime;
  };
  struct Accounting {
    uint64_t capacity;
    uint64_t allocated;
    size_t entries;
  };
  explicit TemporaryFilesystem(uint64_t capacity = 16 * 1024 * 1024, size_t maximumEntries = 256, std::function<uint64_t()> clock = {});
  std::optional<Metadata> Stat(const std::string& path) const;
  Accounting Usage() const;
  uint32_t CreateDirectory(const std::string& path);
  uint32_t CreateFile(const std::string& path, uint32_t attributes);
  uint32_t OverwriteFile(const std::string& path, uint32_t attributes);
  uint32_t WriteFile(const std::string& path, std::span<const uint8_t> bytes);
  uint32_t Write(const std::string& path, uint64_t offset, std::span<const uint8_t> bytes);
  size_t Read(const std::string& path, uint64_t offset, std::span<uint8_t> output) const;

private:
  struct Node {
    bool directory;
    std::vector<uint8_t> bytes;
    uint32_t attributes;
    uint64_t creationTime;
    uint64_t lastAccessTime;
    uint64_t lastWriteTime;
    uint64_t changeTime;
  };
  mutable std::mutex lock_;
  mutable std::map<std::string, Node> nodes_;
  std::function<uint64_t()> clock_;
  Node NewNode(bool directory, uint32_t attributes, std::vector<uint8_t> bytes = {}) const;
  uint64_t capacity_;
  uint64_t allocated_ = AllocationUnit;
  size_t maximumEntries_;
  static std::optional<std::string> Normalize(std::string path);
  static uint64_t Allocation(uint64_t size);
  uint32_t ParentStatus(const std::string& path) const;
};

}
