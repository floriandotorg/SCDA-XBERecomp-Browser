#include "kernel/disc_index.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <stdexcept>

namespace {

constexpr uint64_t sectorBytes = 2048;
constexpr uint64_t fullPartition = 0x18300000;
constexpr char magic[] = "MICROSOFT*XBOX*MEDIA";

const auto check = [](bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
};

struct Image {
    uint64_t base = 0;
    uint64_t size = 6ull << 30;
    uint32_t maxRead = 0;
    uint64_t bytesRead = 0;
    bool readFailure = false;
    std::map<uint64_t, std::vector<uint8_t>> regions;

    explicit Image(uint64_t partition = 0) : base(partition) {
        auto& volume = regions[base + 32 * sectorBytes];
        volume.resize(sectorBytes);
        std::memcpy(volume.data(), magic, 20);
        std::memcpy(volume.data() + 0x7ec, magic, 20);
        put(volume, 0x14, 40, 4);
        put(volume, 0x18, 256, 4);
        put(volume, 0x1c, 123456789, 8);
        auto& root = regions[base + 40 * sectorBytes];
        root.resize(256);
        entry(root, 0, "default.xbe", 50, 6, false, 0, 8);
        entry(root, 32, "Story", 41, 256, true);
        auto& story = regions[base + 41 * sectorBytes];
        story.resize(256);
        entry(story, 0, "level.bin", uint32_t((1ull << 32) / sectorBytes + 64), 6, false);
        regions[base + 50 * sectorBytes] = {1, 2, 3, 4, 5, 6};
        regions[base + (1ull << 32) + 64 * sectorBytes] = {7, 8, 9, 10, 11, 12};
    }

    static void put(std::vector<uint8_t>& bytes, size_t at, uint64_t value, size_t length) {
        for (size_t n = 0; n < length; ++n) {
            bytes[at + n] = uint8_t(value >> (n * 8));
        }
    }

    static void entry(std::vector<uint8_t>& table, size_t at, std::string_view name,
                      uint32_t sector, uint32_t size, bool directory, uint16_t left = 0, uint16_t right = 0) {
        put(table, at, left, 2);
        put(table, at + 2, right, 2);
        put(table, at + 4, sector, 4);
        put(table, at + 8, size, 4);
        table[at + 12] = directory ? 0x10 : 0;
        table[at + 13] = uint8_t(name.size());
        std::memcpy(table.data() + at + 14, name.data(), name.size());
    }

    uint32_t read(uint64_t offset, void* destination, uint32_t length) {
        check(offset <= size && length <= size - offset, "reader received an unchecked range");
        maxRead = std::max(maxRead, length);
        bytesRead += length;
        if (readFailure) {
            return length - 1;
        }
        auto* out = static_cast<uint8_t*>(destination);
        std::memset(out, 0, length);
        for (const auto& [start, bytes] : regions) {
            const auto first = std::max(start, offset);
            const auto last = std::min(start + bytes.size(), offset + length);
            if (first < last) {
                std::memcpy(out + first - offset, bytes.data() + first - start, last - first);
            }
        }
        return length;
    }

    bool open(kernel::DiscIndex& index, std::string& error) {
        return index.Open(size, [this](uint64_t offset, void* out, uint32_t length) {
            return read(offset, out, length);
        }, error);
    }

    std::vector<uint8_t>& root() { return regions[base + 40 * sectorBytes]; }
    std::vector<uint8_t>& volume() { return regions[base + 32 * sectorBytes]; }
};

const auto valid = [](uint64_t partition) {
    Image image(partition);
    kernel::DiscIndex index;
    std::string error;
    check(image.open(index, error), error.c_str());
    check(index.Partition() == partition && index.Time() == 123456789, "volume metadata");
    const auto* root = index.Find("\\");
    check(root && root->children.size() == 2, "root enumeration");
    check(root->children[0].name == "default.xbe" && root->children[1].dir, "sorted entries");
    const auto* file = index.Find("/STORY//./../story/LeVeL.BIN");
    check(file && file->offset > (1ull << 32), "large case insensitive extent");
    check(index.Find("../../DEFAULT.XBE") && !index.Find("default.xbe/../Story"), "path confinement");
    check(!index.Find("story/missing") && !index.Find("story/level.bin/child"), "missing paths");
    std::array<uint8_t, 10> bytes{};
    check(index.Read(*file, 4, bytes.data(), bytes.size()) == 2 && bytes[0] == 11 && bytes[1] == 12, "bounded EOF");
    check(index.Read(*file, 6, bytes.data(), bytes.size()) == 0, "EOF");
    check(index.Read(*root, 0, bytes.data(), bytes.size()) == 0, "directory read");
    std::vector<uint8_t> loaded;
    check(index.Load("default.xbe", loaded) && loaded == std::vector<uint8_t>({1, 2, 3, 4, 5, 6}), "load executable");
    check(image.maxRead <= (1u << 20) && image.bytesRead < 8192, "bounded sparse probe");
};

const auto corrupt = [](const auto& mutate, std::string_view expected = "") {
    Image image;
    mutate(image);
    kernel::DiscIndex index;
    std::string error;
    check(!image.open(index, error), "corrupt image accepted");
    check(!error.empty() && error.find(expected) != std::string::npos, error.c_str());
    check(!index.Find("default.xbe"), "failed mount exposed partial index");
};

}

int main() {
    valid(0);
    valid(fullPartition);
    corrupt([](Image& image) { image.volume()[0] = 0; }, "descriptor");
    corrupt([](Image& image) { image.volume()[0x7ec] = 0; }, "descriptor");
    corrupt([](Image& image) { image.volume().assign(2048, 0); }, "filesystem");
    corrupt([](Image& image) { image.size = 32 * sectorBytes + 100; }, "Truncated");
    corrupt([](Image& image) { image.size = 40 * sectorBytes + 20; }, "extent");
    corrupt([](Image& image) { image.readFailure = true; }, "read");
    corrupt([](Image& image) { Image::put(image.volume(), 0x18, 17 << 20, 4); }, "budget");
    corrupt([](Image& image) { Image::put(image.root(), 2, 0xffff, 2); }, "link");
    corrupt([](Image& image) { Image::put(image.root(), 32, 8, 2); }, "cycle");
    corrupt([](Image& image) { Image::put(image.root(), 32, 0, 2); Image::put(image.root(), 34, 8, 2); }, "cycle");
    corrupt([](Image& image) { Image::put(image.root(), 32 + 4, 40, 4); }, "overlap");
    corrupt([](Image& image) { Image::put(image.root(), 32 + 4, 32, 4); }, "overlap");
    corrupt([](Image& image) { Image::put(image.root(), 4, 0xffffffff, 4); }, "extent");
    corrupt([](Image& image) { image.root()[13] = 0; }, "name");
    corrupt([](Image& image) { image.root()[14] = '/'; }, "name");
    corrupt([](Image& image) { image.root()[14] = 0; }, "name");
    corrupt([](Image& image) { Image::entry(image.root(), 32, "DEFAULT.XBE", 51, 4, false); }, "Duplicate");
    corrupt([](Image& image) { Image::put(image.root(), 2, 3, 2); }, "overlap");
    corrupt([](Image& image) { Image::put(image.root(), 2, 63, 2); }, "entry");
    corrupt([](Image& image) { Image::entry(image.root(), 32, "..", 41, 256, true); }, "name");

    for (const auto& limits : {kernel::DiscLimits{.maxDepth = 0}, kernel::DiscLimits{.maxDirectories = 1},
                              kernel::DiscLimits{.maxEntries = 1}, kernel::DiscLimits{.maxIndexBytes = 300}}) {
        Image image;
        kernel::DiscIndex index(limits);
        std::string error;
        check(!image.open(index, error) && error.find("budget") != std::string::npos, "index budget ignored");
    }

    Image image;
    kernel::DiscIndex index;
    std::string error;
    check(image.open(index, error), "initial mount");
    image.root()[13] = 0;
    check(!image.open(index, error) && !index.Find("default.xbe"), "failed remount retained stale entries");
    image.root().assign(256, 0xff);
    check(image.open(index, error) && index.Find("/")->children.empty(), "empty FF table");
    image.root()[255] = 0;
    check(!image.open(index, error), "mixed FF table accepted");
    Image large;
    large.regions[50 * sectorBytes].resize((1u << 20) + 123, 42);
    Image::put(large.root(), 8, large.regions[50 * sectorBytes].size(), 4);
    check(large.open(index, error), "large subfile mount");
    std::vector<uint8_t> bytes;
    check(index.Load("default.xbe", bytes) && bytes.size() == (1u << 20) + 123 && bytes.back() == 42, "chunked load");
    check(large.maxRead == (1u << 20), "physical request ceiling");
    large.readFailure = true;
    check(!index.Load("default.xbe", bytes) && bytes.empty(), "failed load retained data");
    large.readFailure = false;
    kernel::DiscIndex smallLoad(kernel::DiscLimits{.maxLoadBytes = 5});
    check(large.open(smallLoad, error) && !smallLoad.Load("default.xbe", bytes) && bytes.empty(), "load allocation budget");
    Image::put(large.root(), 8, 0, 4);
    check(large.open(index, error) && index.Load("default.xbe", bytes) && bytes.empty(), "empty file");

    Image overlapping;
    Image::put(overlapping.regions[41 * sectorBytes], 4, 50, 4);
    check(!overlapping.open(index, error) && error.find("overlap") != std::string::npos, "overlapping files");

    Image deep;
    deep.regions.erase(41 * sectorBytes);
    for (uint32_t n = 0; n < 35; ++n) {
        auto& table = deep.regions[(41 + n) * sectorBytes];
        table.resize(256);
        Image::entry(table, 0, "nested", 42 + n, n == 34 ? 0 : 256, true);
    }
    Image::put(deep.root(), 4, 1000, 4);
    check(!deep.open(index, error) && error.find("budget") != std::string::npos, "default recursion budget");

    uint32_t random = 0x12345678;
    for (uint32_t n = 0; n < 1000; ++n) {
        Image mutated;
        for (uint32_t byte = 0; byte < 8; ++byte) {
            random = random * 1664525u + 1013904223u;
            const auto at = (random >> 16) % mutated.root().size();
            mutated.root()[at] ^= uint8_t(random);
        }
        if (mutated.open(index, error)) {
            const auto* mountedRoot = index.Find("/");
            check(mountedRoot && mountedRoot->dir, "mutated root invariant");
            for (const auto& entry : mountedRoot->children) {
                check(entry.offset <= mutated.size && entry.size <= mutated.size - entry.offset, "mutated extent invariant");
            }
        } else {
            check(!error.empty() && !index.Find("/"), "mutated failure invariant");
        }
        check(mutated.maxRead <= (1u << 20), "mutated read ceiling");
    }
    std::cout << "disc-index-contracts-ok mutations=1000\n";
}
