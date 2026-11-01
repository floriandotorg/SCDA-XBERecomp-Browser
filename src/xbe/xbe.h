#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace xbe {

constexpr uint32_t kRetailEntryKey = 0xA8FC57AB;
constexpr uint32_t kDebugEntryKey = 0x94859D4B;
constexpr uint32_t kRetailThunkKey = 0x5B6D40B6;
constexpr uint32_t kDebugThunkKey = 0xEFB1F152;

enum SectionFlags : uint32_t {
    kWritable = 1,
    kPreload = 2,
    kExecutable = 4,
    kInsertedFile = 8,
};

struct Section {
    std::string name;
    uint32_t flags = 0;
    uint32_t va = 0, vsize = 0;
    uint32_t raw = 0, rsize = 0;
};

struct Library {
    std::string name;
    uint16_t major = 0, minor = 0, build = 0, flags = 0;
};

struct Tls {
    uint32_t data_start = 0, data_end = 0, index_addr = 0, callbacks = 0, zero_fill = 0, characteristics = 0;
};

struct Image {
    std::vector<uint8_t> file;
    uint32_t base = 0, header_size = 0, image_size = 0;
    uint32_t entry = 0, kernel_thunk = 0, tls_addr = 0;
    uint32_t stack_commit = 0, heap_reserve = 0, heap_commit = 0;
    uint32_t cert = 0, title_id = 0, init_flags = 0;
    bool debug = false;
    std::vector<Section> sections;
    std::vector<Library> libraries;
    Tls tls;

    uint32_t U32(uint32_t offset) const {
        if (!Contains(0, file.size(), offset, 4)) {
            return 0;
        }
        return uint32_t(file[offset]) | uint32_t(file[offset + 1]) << 8 |
               uint32_t(file[offset + 2]) << 16 | uint32_t(file[offset + 3]) << 24;
    }

    int64_t Offset(uint32_t va) const {
        return FileRange(va, 1);
    }

    const Section* SectionAt(uint32_t va) const {
        for (const auto& section : sections) {
            if (Contains(section.va, section.vsize, va, 1)) {
                return &section;
            }
        }
        return nullptr;
    }

    bool Read32(uint32_t va, uint32_t& out) const {
        const int64_t offset = FileRange(va, 4);
        if (offset < 0) {
            return false;
        }
        out = U32(uint32_t(offset));
        return true;
    }

    std::string CStr(uint32_t va) const {
        std::string result;
        for (uint32_t n = 0; n < 4096; ++n) {
            if (uint64_t(va) + n > UINT32_MAX) {
                return {};
            }
            const int64_t offset = FileRange(va + n, 1);
            if (offset < 0) {
                return {};
            }
            const uint8_t byte = file[size_t(offset)];
            if (!byte) {
                return result;
            }
            result += char(byte);
        }
        return {};
    }

    bool Parse(std::vector<uint8_t> data, std::string& error) {
        *this = {};
        error.clear();
        Image parsed;
        parsed.file = std::move(data);
        if (!parsed.ParseContents(error)) {
            return false;
        }
        *this = std::move(parsed);
        return true;
    }

private:
    static bool Contains(uint64_t start, uint64_t size, uint64_t position, uint64_t length) {
        return position >= start && position - start <= size && length <= size - (position - start);
    }

    static bool Overlaps(uint64_t first, uint64_t firstSize, uint64_t second, uint64_t secondSize) {
        return firstSize && secondSize && first < second + secondSize && second < first + firstSize;
    }

    bool HeaderRange(uint32_t va, uint64_t length) const {
        return Contains(base, header_size, va, length);
    }

    int64_t FileRange(uint32_t va, uint32_t length) const {
        if (HeaderRange(va, length) && Contains(0, file.size(), uint64_t(va) - base, length)) {
            return int64_t(va) - base;
        }
        for (const auto& section : sections) {
            if (Contains(section.va, section.rsize, va, length) &&
                Contains(section.va, section.vsize, va, length)) {
                const uint64_t offset = uint64_t(section.raw) + (uint64_t(va) - section.va);
                if (Contains(0, file.size(), offset, length)) {
                    return int64_t(offset);
                }
            }
        }
        return -1;
    }

    bool Executable(uint32_t va) const {
        const auto* section = SectionAt(va);
        return section && (section->flags & kExecutable) && FileRange(va, 1) >= 0;
    }

    bool ParseContents(std::string& error) {
        const auto fail = [&](const char* message) {
            error = message;
            return false;
        };
        if (file.size() < 0x180 || file.size() > 64u * 1024 * 1024 || std::memcmp(file.data(), "XBEH", 4)) {
            return fail("invalid XBE file or size");
        }
        base = U32(0x104);
        header_size = U32(0x108);
        image_size = U32(0x10c);
        if (base < 0x10000 || header_size < 0x180 || header_size > 16u * 1024 * 1024 ||
            header_size > file.size() || image_size < header_size || image_size > 256u * 1024 * 1024 ||
            uint64_t(base) + image_size > (uint64_t(1) << 32)) {
            return fail("invalid XBE image bounds");
        }
        cert = U32(0x118);
        const uint32_t sectionCount = U32(0x11c), sectionAddress = U32(0x120);
        init_flags = U32(0x124);
        tls_addr = U32(0x12c);
        stack_commit = U32(0x130);
        heap_reserve = U32(0x134);
        heap_commit = U32(0x138);
        if (stack_commit > 16u * 1024 * 1024 || heap_reserve > 256u * 1024 * 1024 || heap_commit > heap_reserve) {
            return fail("invalid XBE stack or heap budget");
        }
        if (!HeaderRange(cert, 12) || U32(cert - base) < 12 || !HeaderRange(cert, U32(cert - base))) {
            return fail("invalid XBE certificate");
        }
        title_id = U32(cert - base + 8);
        if (!sectionCount || sectionCount > 1024 || !HeaderRange(sectionAddress, uint64_t(sectionCount) * 56)) {
            return fail("invalid XBE section table");
        }
        sections.reserve(sectionCount);
        for (uint32_t n = 0; n < sectionCount; ++n) {
            const uint32_t offset = sectionAddress - base + n * 56;
            Section section;
            section.flags = U32(offset);
            section.va = U32(offset + 4);
            section.vsize = U32(offset + 8);
            section.raw = U32(offset + 12);
            section.rsize = U32(offset + 16);
            const uint32_t name = U32(offset + 20);
            if (!section.vsize || !Contains(base, image_size, section.va, section.vsize) ||
                Overlaps(base, header_size, section.va, section.vsize) || section.rsize > section.vsize ||
                !Contains(0, file.size(), section.raw, section.rsize) ||
                Overlaps(0, header_size, section.raw, section.rsize)) {
                return fail("invalid XBE section extent");
            }
            for (const auto& previous : sections) {
                if (Overlaps(previous.va, previous.vsize, section.va, section.vsize) ||
                    Overlaps(previous.raw, previous.rsize, section.raw, section.rsize)) {
                    return fail("overlapping XBE sections");
                }
            }
            if (!HeaderRange(name, 1)) {
                return fail("invalid XBE section name");
            }
            uint32_t length = 0;
            while (length < 256 && HeaderRange(name, uint64_t(length) + 1) && file[name - base + length]) {
                ++length;
            }
            if (length == 256 || !HeaderRange(name, uint64_t(length) + 1)) {
                return fail("unterminated XBE section name");
            }
            section.name.assign(reinterpret_cast<const char*>(file.data() + name - base), length);
            sections.push_back(std::move(section));
        }
        const uint32_t encodedEntry = U32(0x128), encodedThunk = U32(0x158);
        const auto decode = [&](uint32_t entryKey, uint32_t thunkKey) {
            entry = encodedEntry ^ entryKey;
            kernel_thunk = encodedThunk ^ thunkKey;
            return Executable(entry) && FileRange(kernel_thunk, 4) >= 0 && !(kernel_thunk % 4);
        };
        if (!decode(kRetailEntryKey, kRetailThunkKey)) {
            if (!decode(kDebugEntryKey, kDebugThunkKey)) {
                return fail("invalid XBE entry point or kernel thunk");
            }
            debug = true;
        }
        bool terminated = false;
        for (uint32_t n = 0; n < 4096; ++n) {
            uint32_t value = 0;
            const uint64_t slot = uint64_t(kernel_thunk) + n * 4;
            if (slot > UINT32_MAX || !Read32(uint32_t(slot), value)) {
                return fail("truncated XBE kernel thunk table");
            }
            if (!value) {
                terminated = true;
                break;
            }
            if (!(value & 0x80000000) || !(value & 0x7fffffff) || (value & 0x7fffffff) > 65535) {
                return fail("invalid XBE kernel ordinal");
            }
        }
        if (!terminated) {
            return fail("unterminated XBE kernel thunk table");
        }
        const uint32_t libraryCount = U32(0x160), libraryAddress = U32(0x164);
        if (libraryCount > 256 || (libraryCount && !HeaderRange(libraryAddress, uint64_t(libraryCount) * 16))) {
            return fail("invalid XBE library table");
        }
        libraries.reserve(libraryCount);
        for (uint32_t n = 0; n < libraryCount; ++n) {
            const uint32_t offset = libraryAddress - base + n * 16;
            Library library;
            size_t length = 0;
            while (length < 8 && file[offset + length]) {
                ++length;
            }
            library.name.assign(reinterpret_cast<const char*>(file.data() + offset), length);
            const uint32_t version = U32(offset + 8), build = U32(offset + 12);
            library.major = uint16_t(version);
            library.minor = uint16_t(version >> 16);
            library.build = uint16_t(build);
            library.flags = uint16_t(build >> 16);
            libraries.push_back(std::move(library));
        }
        if (tls_addr) {
            const int64_t offset = FileRange(tls_addr, 24);
            if (offset < 0) {
                return fail("truncated XBE TLS descriptor");
            }
            tls.data_start = U32(uint32_t(offset));
            tls.data_end = U32(uint32_t(offset) + 4);
            tls.index_addr = U32(uint32_t(offset) + 8);
            tls.callbacks = U32(uint32_t(offset) + 12);
            tls.zero_fill = U32(uint32_t(offset) + 16);
            tls.characteristics = U32(uint32_t(offset) + 20);
            const auto* indexSection = SectionAt(tls.index_addr);
            if (tls.data_end < tls.data_start || uint64_t(tls.data_end) - tls.data_start + tls.zero_fill > 1024 * 1024 ||
                (tls.data_end != tls.data_start && FileRange(tls.data_start, tls.data_end - tls.data_start) < 0) ||
                !indexSection || !(indexSection->flags & kWritable) ||
                !Contains(indexSection->va, indexSection->vsize, tls.index_addr, 4)) {
                return fail("invalid XBE TLS template or index");
            }
            if (tls.callbacks) {
                terminated = false;
                for (uint32_t n = 0; n < 257; ++n) {
                    const uint64_t slot = uint64_t(tls.callbacks) + n * 4;
                    uint32_t callback = 0;
                    if (slot > UINT32_MAX || !Read32(uint32_t(slot), callback)) {
                        return fail("truncated XBE TLS callbacks");
                    }
                    if (!callback) {
                        terminated = true;
                        break;
                    }
                    if (n == 256 || !Executable(callback)) {
                        return fail("invalid XBE TLS callback");
                    }
                }
                if (!terminated) {
                    return fail("unterminated XBE TLS callbacks");
                }
            }
        }
        return true;
    }
};

}
