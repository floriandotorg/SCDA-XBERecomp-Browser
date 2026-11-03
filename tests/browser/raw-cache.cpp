#include "browser/raw-cache.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <thread>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

int main() {
  check(browser::RawCache::PartitionCapacity == 64 * 1024 * 1024 &&
    browser::RawCache::PartitionCapacity == browser::CacheFatx::Capacity, "raw/cache capacity sources disagree");
  browser::RawCache cache;
  browser::RawCache isolated;
  std::array<uint8_t, 512> bytes{};
  std::array<uint8_t, 512> output{};
  check(cache.Read(0x800, output) == 0 && output == bytes, "fresh metadata is not zero");
  check(cache.Partition(3) && !cache.Partition(1) && !cache.Partition(4), "cache mapping disagrees with count");
  const auto partition = cache.Partition(3);
  check(!partition->Filesystem(), "unformatted cache namespace exposed");
  check(partition->OpenRaw() == 0 && partition->OpenRaw() == 0xc0000043, "cache raw lease is not exclusive");
  partition->CloseRaw();
  bytes.fill(0x73);
  check(cache.Write(0x800, bytes) == 0 && cache.Read(0x800, output) == 0 && output == bytes, "metadata write did not persist");
  for (uint64_t offset : {0ull, 0x7ffull, 0x801ull, 0xa00ull, 0xffffffffffffffffull}) {
    output.fill(0x41);
    check(cache.Read(offset, output) == 0xc000000d && output[0] == 0x41, "invalid raw read changed output");
    check(cache.Write(offset, output) == 0xc000000d, "invalid raw write accepted");
  }
  check(cache.Write(0x800, std::span(bytes).first(511)) == 0xc000000d, "short raw write accepted");
  check(cache.Read(0x800, output) == 0 && output == bytes, "failed raw writes changed metadata");
  check(isolated.Read(0x800, output) == 0 && output == std::array<uint8_t, 512>{}, "session metadata leaked");
  check(!isolated.Partition(3)->Filesystem(), "session namespace leaked");
  std::thread writer([&] {
    std::array<uint8_t, 512> update;
    for (uint32_t n = 0; n < 1000; ++n) {
      update.fill(uint8_t(n));
      check(cache.Write(0x800, update) == 0, "concurrent raw write failed");
    }
  });
  for (uint32_t n = 0; n < 1000; ++n) {
    check(cache.Read(0x800, output) == 0, "concurrent raw read failed");
    for (const auto byte : output) { check(byte == output[0], "raw read observed torn sector"); }
  }
  writer.join();
  {
    browser::RawCache replacement;
    check(replacement.Read(0x800, output) == 0 && output == std::array<uint8_t, 512>{}, "replacement did not reset metadata");
    check(!replacement.Partition(3)->Filesystem(), "replacement retained old allocations");
  }
  std::puts("raw-cache-contracts-ok");
}
