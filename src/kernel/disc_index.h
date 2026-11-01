#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace kernel {

struct DiscLimits {
    uint32_t maxDepth = 32;
    uint32_t maxDirectories = 8192;
    uint32_t maxEntries = 131072;
    uint64_t maxIndexBytes = 64ull << 20;
    uint32_t maxTableBytes = 16u << 20;
    uint32_t maxLoadBytes = 64u << 20;
};

class DiscIndex {
public:
    struct Entry {
        std::string name;
        uint64_t offset = 0;
        uint32_t size = 0;
        bool dir = false;
        std::vector<Entry> children;
    };
    using Reader = std::function<uint32_t(uint64_t, void*, uint32_t)>;

    explicit DiscIndex(DiscLimits limits = {}) : limits_(limits) {}
    bool Open(uint64_t size, Reader reader, std::string& error);
    const Entry* Find(const std::vector<std::string>& parts) const;
    const Entry* Find(std::string_view path) const;
    uint32_t Read(const Entry& file, uint64_t pos, void* out, uint32_t len) const;
    bool Load(std::string_view path, std::vector<uint8_t>& out) const;
    uint64_t Time() const { return time_; }
    uint64_t Partition() const { return base_; }

private:
    DiscLimits limits_;
    Reader reader_;
    uint64_t size_ = 0;
    uint64_t base_ = 0;
    uint64_t time_ = 0;
    Entry root_;
};

}
