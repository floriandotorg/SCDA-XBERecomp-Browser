#include "xbe/xbe.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {

const auto check = [](bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
};

const auto put = [](std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t n = 0; n < 4; ++n) {
        bytes.at(offset + n) = uint8_t(value >> (n * 8));
    }
};

const auto fixture = [](bool debug = false) {
    std::vector<uint8_t> bytes(0x700);
    std::memcpy(bytes.data(), "XBEH", 4);
    put(bytes, 0x104, 0x10000);
    put(bytes, 0x108, 0x400);
    put(bytes, 0x10c, 0x3000);
    put(bytes, 0x118, 0x10200);
    put(bytes, 0x11c, 2);
    put(bytes, 0x120, 0x10240);
    put(bytes, 0x128, 0x11000 ^ (debug ? xbe::kDebugEntryKey : xbe::kRetailEntryKey));
    put(bytes, 0x12c, 0x12000);
    put(bytes, 0x130, 0x10000);
    put(bytes, 0x134, 0x100000);
    put(bytes, 0x138, 0x10000);
    put(bytes, 0x158, 0x12040 ^ (debug ? xbe::kDebugThunkKey : xbe::kRetailThunkKey));
    put(bytes, 0x160, 1);
    put(bytes, 0x164, 0x102c0);
    put(bytes, 0x200, 0x20);
    put(bytes, 0x208, 0x12345678);
    put(bytes, 0x240, xbe::kExecutable | xbe::kPreload);
    put(bytes, 0x244, 0x11000);
    put(bytes, 0x248, 0x200);
    put(bytes, 0x24c, 0x400);
    put(bytes, 0x250, 0x200);
    put(bytes, 0x254, 0x102b0);
    put(bytes, 0x278, xbe::kWritable | xbe::kPreload);
    put(bytes, 0x27c, 0x12000);
    put(bytes, 0x280, 0x400);
    put(bytes, 0x284, 0x600);
    put(bytes, 0x288, 0x100);
    put(bytes, 0x28c, 0x102b8);
    std::memcpy(bytes.data() + 0x2b0, ".text", 6);
    std::memcpy(bytes.data() + 0x2b8, ".data", 6);
    std::memcpy(bytes.data() + 0x2c0, "XAPILIB", 8);
    put(bytes, 0x2c8, 1 | (2 << 16));
    put(bytes, 0x2cc, 3 | (4 << 16));
    put(bytes, 0x600, 0x12080);
    put(bytes, 0x604, 0x12084);
    put(bytes, 0x608, 0x12060);
    put(bytes, 0x60c, 0x12050);
    put(bytes, 0x610, 16);
    put(bytes, 0x640, 0x80000001);
    put(bytes, 0x650, 0x11010);
    put(bytes, 0x680, 0x44332211);
    return bytes;
};

const auto accept = [](xbe::Image& image, std::vector<uint8_t> bytes) {
    std::string error = "previous error";
    check(image.Parse(std::move(bytes), error), error.c_str());
    check(error.empty(), "success retained an old error");
};

const auto reject = [](std::vector<uint8_t> bytes) {
    xbe::Image image;
    std::string error;
    check(!image.Parse(std::move(bytes), error), "corrupt XBE accepted");
    check(!error.empty(), "rejection has no diagnostic");
    check(image.file.empty() && image.sections.empty() && image.libraries.empty() && image.entry == 0,
          "failed parse published partial image");
};

const auto invalidField = [](size_t offset, uint32_t value) {
    auto bytes = fixture();
    put(bytes, offset, value);
    reject(std::move(bytes));
};

const auto contracts = [] {
    invalidField(0x280, 0xfffffff0);
    xbe::Image image;
    accept(image, fixture());
    check(image.entry == 0x11000 && image.kernel_thunk == 0x12040 && !image.debug, "retail keys");
    check(image.title_id == 0x12345678 && image.sections.size() == 2, "image metadata");
    check(image.sections[0].name == ".text" && image.sections[1].name == ".data", "section names");
    check(image.libraries.size() == 1 && image.libraries[0].name == "XAPILIB" &&
          image.libraries[0].major == 1 && image.libraries[0].minor == 2 &&
          image.libraries[0].build == 3 && image.libraries[0].flags == 4, "library metadata");
    check(image.tls.data_start == 0x12080 && image.tls.data_end == 0x12084 && image.tls.zero_fill == 16, "TLS metadata");
    uint32_t value = 0;
    check(image.Read32(0x12080, value) && value == 0x44332211, "mapped data");
    check(!image.Read32(0x111ff, value), "read crossed section boundary");
    check(!image.Read32(0x12100, value) && image.Offset(0x12100) == -1, "zero filled tail is not file data");
    check(image.Offset(0xffffffff) == -1 && image.SectionAt(0xffffffff) == nullptr, "address wrap");
    check(image.CStr(0x102b0) == ".text" && image.CStr(0xffffffff).empty(), "bounded strings");
    check(image.U32(0xffffffff) == 0, "raw scalar bounds");
    accept(image, fixture(true));
    check(image.debug && image.sections.size() == 2 && image.libraries.size() == 1, "parse reuse accumulated state");
    accept(image, fixture());
    check(!image.debug, "debug key state survived retail parse");
    auto minimal = fixture();
    put(minimal, 0x12c, 0);
    put(minimal, 0x160, 0);
    put(minimal, 0x164, 0);
    accept(image, std::move(minimal));
    check(image.tls.data_start == 0 && image.libraries.empty(), "optional data retained");
    std::string error;
    check(!image.Parse({}, error) && image.file.empty() && image.base == 0 && image.sections.empty(), "failed reparse state");
    auto emptySection = fixture();
    put(emptySection, 0x288, 0);
    put(emptySection, 0x284, 0);
    put(emptySection, 0x12c, 0);
    put(emptySection, 0x158, 0x11000 ^ xbe::kRetailThunkKey);
    accept(image, std::move(emptySection));
    check(image.Offset(0x12000) == -1 && image.SectionAt(0x12000), "uninitialized section mapping");
    auto zeroIndex = fixture();
    put(zeroIndex, 0x608, 0x123fc);
    accept(image, std::move(zeroIndex));
    check(image.tls.index_addr == 0x123fc, "TLS index in zero filled writable memory");

    for (const auto& [offset, value] : std::vector<std::pair<size_t, uint32_t>>{
        {0x104, 0xfffff000}, {0x108, 0x100}, {0x108, 0x701}, {0x10c, 0x200}, {0x10c, 0xffffffff},
        {0x118, 0xfffffff0}, {0x200, 0xffffffff}, {0x11c, 0}, {0x11c, 0xffffffff},
        {0x120, 0xfffffff0}, {0x120, 0x103ff}, {0x244, 0xfffffff0}, {0x244, 0x10080},
        {0x27c, 0x11080}, {0x24c, 0xfffffff0}, {0x24c, 0x3ff}, {0x250, 0x201},
        {0x284, 0x500}, {0x254, 0xffffffff}, {0x160, 0xffffffff}, {0x164, 0x103ff},
        {0x128, 0x10080 ^ xbe::kRetailEntryKey}, {0x128, 0x12000 ^ xbe::kRetailEntryKey},
        {0x158, 0x12100 ^ xbe::kRetailThunkKey}, {0x12c, 0x120f0}, {0x604, 0x1207f},
        {0x604, 0x12101}, {0x608, 0xffffffff}, {0x610, 0xffffffff}, {0x60c, 0x120ff},
        {0x650, 0x12000}, {0x640, 1}, {0x640, 0x80000000}, {0x640, 0x80010000},
        {0x134, 0x1000}, {0x130, 0xffffffff}, {0x134, 0xffffffff},
        {0x11c, 1025}, {0x160, 257}, {0x608, 0x123fd}, {0x608, 0x11000},
    }) {
        invalidField(offset, value);
    }
    auto bytes = fixture();
    std::fill(bytes.begin() + 0x2b0, bytes.begin() + 0x400, 'x');
    reject(std::move(bytes));
    bytes = fixture();
    std::fill(bytes.begin() + 0x640, bytes.begin() + 0x700, 0xff);
    reject(std::move(bytes));
    bytes = fixture();
    bytes.resize(64u * 1024 * 1024 + 1);
    reject(std::move(bytes));
    const auto largeTable = [] {
        auto result = fixture();
        result.resize(0x5600);
        put(result, 0x10c, 0x8000);
        put(result, 0x280, 0x5000);
        put(result, 0x288, 0x5000);
        return result;
    };
    bytes = largeTable();
    for (size_t n = 0; n < 4096; ++n) {
        put(bytes, 0x640 + n * 4, 0x80000001);
    }
    reject(bytes);
    put(bytes, 0x640 + 4095 * 4, 0);
    put(bytes, 0x60c, 0);
    accept(image, std::move(bytes));
    bytes = largeTable();
    put(bytes, 0x60c, 0x12200);
    for (size_t n = 0; n < 257; ++n) {
        put(bytes, 0x800 + n * 4, 0x11000);
    }
    reject(bytes);
    put(bytes, 0x800 + 256 * 4, 0);
    accept(image, std::move(bytes));
    for (size_t n = 0; n < fixture().size(); ++n) {
        bytes = fixture();
        bytes.resize(n);
        reject(std::move(bytes));
    }
    uint32_t random = 0x91827364;
    for (size_t n = 0; n < 2000; ++n) {
        bytes = fixture();
        random = random * 1664525 + 1013904223;
        const size_t offset = random % (bytes.size() - 4);
        random = random * 1664525 + 1013904223;
        put(bytes, offset, random);
        if (image.Parse(std::move(bytes), error)) {
            check(image.entry != 0 && image.sections.size() <= 1024, "accepted mutation invariant");
            for (const auto& section : image.sections) {
                check(uint64_t(section.raw) + section.rsize <= image.file.size(), "accepted raw bounds");
                check(uint64_t(section.va) + section.vsize <= uint64_t(image.base) + image.image_size, "accepted virtual bounds");
            }
        } else {
            check(image.file.empty() && image.sections.empty(), "mutation failure published state");
        }
    }
};

}

int main() {
    contracts();
    std::cout << "xbe-contracts-ok\n";
}
