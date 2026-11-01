#include "kernel/disc_index.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <utility>

namespace kernel {
namespace {

constexpr uint64_t sectorBytes = 2048;
constexpr uint64_t volumeOffset = 32 * sectorBytes;
constexpr uint32_t maxReadBytes = 1u << 20;
constexpr std::string_view magic = "MICROSOFT*XBOX*MEDIA";

const auto upper = [](std::string_view name) {
    std::string result(name);
    for (char& character : result) {
        if (character >= 'a' && character <= 'z') {
            character = char(character - 'a' + 'A');
        }
    }
    return result;
};

const auto integer = [](const uint8_t* bytes, size_t length) {
    uint64_t value = 0;
    for (size_t n = 0; n < length; ++n) {
        value |= uint64_t(bytes[n]) << (n * 8);
    }
    return value;
};

const auto readExact = [](const DiscIndex::Reader& reader, uint64_t at, void* out, uint32_t length) {
    auto* bytes = static_cast<uint8_t*>(out);
    while (length > 0) {
        const auto chunk = std::min(length, maxReadBytes);
        if (reader(at, bytes, chunk) != chunk) {
            return false;
        }
        at += chunk;
        bytes += chunk;
        length -= chunk;
    }
    return true;
};

struct Builder {
    const DiscIndex::Reader& reader;
    const DiscLimits& limits;
    std::string& error;
    uint64_t size;
    uint64_t base = 0;
    uint64_t time = 0;
    uint64_t chargedBytes = 0;
    uint32_t entries = 0;
    uint32_t directories = 0;
    std::map<uint64_t, uint64_t> extents;
    DiscIndex::Entry root;

    bool fail(std::string_view message) {
        error = message;
        return false;
    }

    bool charge(uint64_t bytes) {
        if (bytes > limits.maxIndexBytes - chargedBytes) {
            return fail("Disc index allocation budget exceeded.");
        }
        chargedBytes += bytes;
        return true;
    }

    bool extent(uint64_t offset, uint32_t length) {
        if (offset < base || offset > size || length > size - offset) {
            return fail("Disc extent is outside the selected file.");
        }
        if (length == 0) {
            return true;
        }
        const uint64_t end = offset + length;
        const auto next = extents.lower_bound(offset);
        if ((next != extents.end() && next->first < end) ||
            (next != extents.begin() && std::prev(next)->second > offset)) {
            return fail("Disc extents overlap or form a directory cycle.");
        }
        if (!charge(128)) {
            return false;
        }
        extents.emplace(offset, end);
        return true;
    }

    bool directory(DiscIndex::Entry& dir, uint32_t depth) {
        if (depth > limits.maxDepth || ++directories > limits.maxDirectories || dir.size > limits.maxTableBytes) {
            return fail("Disc directory budget exceeded.");
        }
        if (dir.size == 0) {
            return true;
        }
        if (!charge(uint64_t(dir.size) * 2)) {
            return false;
        }
        std::vector<uint8_t> table(dir.size);
        if (!readExact(reader, dir.offset, table.data(), dir.size)) {
            return fail("Disc directory read failed.");
        }
        if (std::all_of(table.begin(), table.end(), [](uint8_t byte) { return byte == 0xff; })) {
            return true;
        }
        std::vector<uint8_t> occupied(dir.size);
        std::vector<uint32_t> todo{0};
        while (!todo.empty()) {
            const uint32_t at = todo.back();
            todo.pop_back();
            if (at > dir.size || dir.size - at < 14) {
                return fail("Invalid disc directory entry link.");
            }
            if (occupied[at] == 2) {
                return fail("Disc directory entry cycle or repeated link.");
            }
            if (occupied[at] != 0) {
                return fail("Disc directory entries overlap.");
            }
            const auto* entry = table.data() + at;
            const uint32_t length = entry[13];
            if (length == 0 || length > dir.size - at - 14) {
                return fail("Invalid disc directory entry name length.");
            }
            const uint32_t recordSize = (14 + length + 3) / 4 * 4;
            if (recordSize > dir.size - at) {
                return fail("Truncated disc directory entry.");
            }
            for (uint32_t n = at; n < at + recordSize; ++n) {
                if (occupied[n] != 0) {
                    return fail("Disc directory entries overlap.");
                }
                occupied[n] = 1;
            }
            occupied[at] = 2;
            std::string name(reinterpret_cast<const char*>(entry + 14), length);
            if (name == "." || name == ".." || std::any_of(name.begin(), name.end(), [](unsigned char character) {
                    return character < 32 || character == 127 || character == '/' || character == '\\' || character == ':';
                })) {
                return fail("Invalid disc entry name.");
            }
            if (++entries > limits.maxEntries || !charge(sizeof(DiscIndex::Entry) * 2 + length * 2 + 64)) {
                return fail("Disc entry allocation budget exceeded.");
            }
            const uint64_t offset = base + integer(entry + 4, 4) * sectorBytes;
            const uint32_t size = uint32_t(integer(entry + 8, 4));
            if (!extent(offset, size)) {
                return false;
            }
            dir.children.push_back({std::move(name), offset, size, (entry[12] & 0x10) != 0, {}});
            for (size_t n = 0; n < 4; n += 2) {
                const auto link = uint32_t(integer(entry + n, 2)) * 4;
                if (link != 0) {
                    todo.push_back(link);
                }
            }
        }
        std::sort(dir.children.begin(), dir.children.end(), [](const auto& first, const auto& second) {
            return upper(first.name) < upper(second.name);
        });
        for (size_t n = 0; n < dir.children.size(); ++n) {
            auto& child = dir.children[n];
            if (n > 0 && upper(child.name) == upper(dir.children[n - 1].name)) {
                return fail("Duplicate disc entry name.");
            }
            if (child.dir && !directory(child, depth + 1)) {
                return false;
            }
        }
        return true;
    }

    bool mount() {
        for (const uint64_t partition : {0ull, 0x18300000ull}) {
            const uint64_t at = partition + volumeOffset;
            if (at >= size) {
                continue;
            }
            if (sectorBytes > size - at) {
                return fail("Truncated Xbox volume descriptor.");
            }
            std::array<uint8_t, sectorBytes> volume{};
            if (!readExact(reader, at, volume.data(), volume.size())) {
                return fail("Disc volume read failed.");
            }
            const bool first = std::memcmp(volume.data(), magic.data(), magic.size()) == 0;
            const bool last = std::memcmp(volume.data() + 0x7ec, magic.data(), magic.size()) == 0;
            if (!first && !last) {
                continue;
            }
            if (!first || !last) {
                return fail("Corrupt Xbox volume descriptor signatures.");
            }
            base = partition;
            time = integer(volume.data() + 0x1c, 8);
            root = {{}, base + integer(volume.data() + 0x14, 4) * sectorBytes,
                    uint32_t(integer(volume.data() + 0x18, 4)), true, {}};
            return extent(at, sectorBytes) && extent(root.offset, root.size) && directory(root, 0);
        }
        return fail("No supported Xbox filesystem found.");
    }
};

}

bool DiscIndex::Open(uint64_t size, Reader reader, std::string& error) {
    reader_ = {};
    root_ = {};
    size_ = base_ = time_ = 0;
    error.clear();
    const DiscLimits ceiling;
    if (!reader || limits_.maxDepth > ceiling.maxDepth || limits_.maxDirectories > ceiling.maxDirectories ||
        limits_.maxEntries > ceiling.maxEntries || limits_.maxIndexBytes > ceiling.maxIndexBytes ||
        limits_.maxTableBytes > ceiling.maxTableBytes || limits_.maxLoadBytes > ceiling.maxLoadBytes) {
        error = "Invalid disc reader or allocation budget.";
        return false;
    }
    Builder builder{reader, limits_, error, size, 0, 0, 0, 0, 0, {}, {}};
    if (!builder.mount()) {
        return false;
    }
    root_ = std::move(builder.root);
    base_ = builder.base;
    time_ = builder.time;
    size_ = size;
    reader_ = std::move(reader);
    return true;
}

const DiscIndex::Entry* DiscIndex::Find(const std::vector<std::string>& parts) const {
    if (!reader_) {
        return nullptr;
    }
    std::vector<const Entry*> path{&root_};
    for (const auto& part : parts) {
        if (!path.back()->dir) {
            return nullptr;
        }
        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (path.size() > 1) {
                path.pop_back();
            }
            continue;
        }
        const auto& children = path.back()->children;
        const auto name = upper(part);
        const auto entry = std::lower_bound(children.begin(), children.end(), name, [](const Entry& entry, const auto& name) {
            return upper(entry.name) < name;
        });
        if (entry == children.end() || upper(entry->name) != name) {
            return nullptr;
        }
        path.push_back(&*entry);
    }
    return path.back();
}

const DiscIndex::Entry* DiscIndex::Find(std::string_view path) const {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size()) {
        const size_t end = std::min(path.find_first_of("\\/", start), path.size());
        parts.emplace_back(path.substr(start, end - start));
        start = end + 1;
    }
    return Find(parts);
}

uint32_t DiscIndex::Read(const Entry& file, uint64_t pos, void* out, uint32_t len) const {
    if (!reader_ || file.dir || pos >= file.size || !out || file.offset > size_ || file.size > size_ - file.offset) {
        return 0;
    }
    const auto length = uint32_t(std::min<uint64_t>(len, file.size - pos));
    return readExact(reader_, file.offset + pos, out, length) ? length : 0;
}

bool DiscIndex::Load(std::string_view path, std::vector<uint8_t>& out) const {
    out.clear();
    const auto* file = Find(path);
    if (!file || file->dir || file->size > limits_.maxLoadBytes) {
        return false;
    }
    out.resize(file->size);
    if (Read(*file, 0, out.data(), file->size) != file->size) {
        out.clear();
        return false;
    }
    return true;
}

}
