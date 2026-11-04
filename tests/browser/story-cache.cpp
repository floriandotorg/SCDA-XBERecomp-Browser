#include "browser/raw-cache.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

const auto check = [](bool value, const char* message) {
  if (!value) { throw std::runtime_error(message); }
};

int main() {
  constexpr uint64_t capacity = 512ull << 20;
  constexpr uint32_t cluster = 16384, header = 4096;
  constexpr uint32_t fat = ((capacity / cluster + 1) * 2 + header - 1) / header * header;
  constexpr uint32_t root = header + fat;
  constexpr uint64_t usable = (capacity - root) / cluster * cluster;
  browser::RawCache defaults;
  browser::RawCache story(capacity);
  auto partition = story.Partition(browser::RawCache::FirstPartition);
  check(partition && partition->CapacityBytes() == capacity && partition->RawBackingBytes() == 0,
    "story cache did not retain its bounded lazy capacity profile");
  check(defaults.Partition(3)->CapacityBytes() == (64ull << 20) && defaults.Partition(3)->RawBackingBytes() == 0,
    "story cache changed default capacity or allocated a different session's backing");
  bool rejected = false;
  try { browser::RawCache invalid(513ull << 20); }
  catch (const std::invalid_argument&) { rejected = true; }
  check(rejected, "unbounded cache capacity profile accepted");
  check(partition->OpenRaw() == 0 && partition->RawBackingBytes() == capacity, "story raw profile allocation failed");
  const auto format = [&](uint32_t tableBytes) {
    std::vector<uint8_t> bytes(header + tableBytes + cluster, 0xff);
    const auto put = [&](uint32_t offset, uint32_t value) {
      for (uint32_t n = 0; n < 4; ++n) { bytes[offset + n] = uint8_t(value >> (n * 8)); }
    };
    put(0, 0x58544146); put(4, 0x12345678); put(8, 32); put(12, 1);
    bytes[16] = bytes[17] = 0;
    std::fill(bytes.begin() + header, bytes.begin() + header + tableBytes, 0);
    put(header, 0xfffffff8);
    for (size_t n = 0; n < bytes.size() / header; ++n) {
      check(partition->WriteRaw(n * header, std::span(bytes).subspan(n * header, header)) == 0,
        "story formatter write exceeded a checked raw transfer");
    }
  };
  format(12288);
  check(partition->Dismount() == 0xc000014f, "story profile accepted the default profile's FAT geometry");
  format(fat);
  std::array<uint8_t, 512> marker{};
  marker.fill(0xa7);
  check(partition->WriteRaw(capacity - marker.size(), marker) == 0, "story raw profile lost its last sector");
  std::array<uint8_t, 512> read{};
  check(partition->ReadRaw(capacity - marker.size(), read) == 0 && read == marker &&
    partition->WriteRaw(capacity, marker) == 0xc000000d, "story raw bounds or backing bytes are false");
  check(partition->Dismount() == 0, "story formatter did not validate its capacity-derived bytes");
  partition->CloseRaw();
  check(partition->Mount() == 0 && partition->RawBackingBytes() == 0 && partition->OpenRaw() == 0xc00000bb,
    "story namespace conversion retained or reopened its raw backing");
  auto files = partition->Filesystem();
  check(files && files->Usage().capacity == usable, "story namespace and raw geometry report different capacities");
  constexpr uint32_t length = 175345664, chunk = 2u << 20;
  check(files->CreateFile("SYNTHETIC.BANK", 0x80) == 0, "story synthetic cache file creation failed");
  std::vector<uint8_t> bytes(chunk);
  const auto pattern = [](uint64_t offset) { return uint8_t(offset * 37 + (offset >> 13)); };
  for (uint32_t n = 0; n < (length + chunk - 1) / chunk; ++n) {
    const uint32_t offset = n * chunk, count = std::min(chunk, length - offset);
    for (uint32_t n = 0; n < count; ++n) { bytes[n] = pattern(uint64_t(offset) + n); }
    check(files->Write("SYNTHETIC.BANK", offset, std::span(bytes).first(count)) == 0,
      "story cache cannot retain the observed common bank size through bounded writes");
  }
  check(files->Stat("SYNTHETIC.BANK")->size == length, "story cache did not retain actual logical EOF");
  for (const uint32_t offset : {0u, length / 2, length - 512}) {
    check(files->Read("SYNTHETIC.BANK", offset, read) == read.size(), "story cache read lost committed data");
    for (uint32_t n = 0; n < read.size(); ++n) { check(read[n] == pattern(uint64_t(offset) + n), "story cache changed committed sample bytes"); }
  }
  const auto before = files->Usage().allocated;
  check(files->Write("SYNTHETIC.BANK", usable, marker) == 0xc000007f && files->Usage().allocated == before &&
    files->Stat("SYNTHETIC.BANK")->size == length, "story cache quota failure mutated committed bytes or allocation");
  check(!defaults.Partition(3)->Filesystem(), "story files escaped their session's cache namespace");
  std::puts("story-cache-contracts-ok");
}
