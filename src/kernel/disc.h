#pragma once
// Xbox DVD file system images (.iso): a whole disc dump, its game partition behind the video one, or the game
// partition alone (xiso). Read only.
#include "kernel/disc_index.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kernel {

class Disc {
public:
    using Entry = DiscIndex::Entry;
    Disc() = default;
    Disc(const Disc&) = delete;
    Disc& operator=(const Disc&) = delete;
    ~Disc();
    // false with the reason when the file is no Xbox disc image
    bool Open(const std::filesystem::path& path, std::string& error);
    // from the root, any case, ".." steps up; null when missing
    const Entry* Find(const std::vector<std::string>& parts) const;
    const Entry* Find(std::string_view path) const;
    // at a position in the file, from any thread; the count read, short at the file's end
    uint32_t Read(const Entry& file, uint64_t pos, void* out, uint32_t len) const;
    bool Load(std::string_view path, std::vector<uint8_t>& out) const;
    uint64_t Time() const { return index_.Time(); }
    const std::filesystem::path& Path() const { return path_; }

private:
    uint32_t ReadAt(uint64_t at, void* out, uint32_t len) const;

    void* file_ = nullptr;
    std::filesystem::path path_;
    DiscIndex index_;
};

// D: from a disc image instead of a folder, set before Boot
void MountDisc(std::unique_ptr<Disc> disc);
const Disc* MountedDisc();

}  // namespace kernel
