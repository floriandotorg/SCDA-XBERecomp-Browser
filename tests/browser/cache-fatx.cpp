#include "browser/cache-fatx.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

const auto put32 = [](std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (uint32_t n = 0; n < 4; ++n) { bytes[offset + n] = uint8_t(value >> (n * 8)); }
};

const auto formatted = [] {
  std::vector<uint8_t> bytes(24576, 0xff);
  put32(bytes, 0, 0x58544146);
  put32(bytes, 4, 0x12345678);
  put32(bytes, 8, 32);
  put32(bytes, 12, 1);
  bytes[16] = bytes[17] = 0;
  std::fill(bytes.begin() + 4096, bytes.begin() + 8192, 0);
  bytes[4096] = 0xf8;
  bytes[4097] = bytes[4098] = bytes[4099] = 0xff;
  return bytes;
};

const auto writeFormat = [](browser::CacheFatx& cache, const std::vector<uint8_t>& bytes) {
  for (size_t n = 0; n < bytes.size(); n += 4096) {
    check(cache.WriteRaw(n, std::span(bytes).subspan(n, 4096)) == 0, "guest format write failed");
  }
};

int main() {
  using browser::CacheFatx;
  const auto bytes = formatted();
  CacheFatx cache;
  std::array<uint8_t, 512> sector{};
  check(!cache.Filesystem() && !cache.Mounted(), "blank cache published a namespace");
  check(cache.Mount() != 0 && cache.Dismount() != 0, "blank cache accepted lifecycle transition");
  check(cache.ReadRaw(0, sector) != 0 && cache.WriteRaw(0, sector) != 0, "raw I/O bypassed open lease");
  check(cache.OpenRaw() == 0 && cache.OpenRaw() != 0, "raw lease is not exclusive");
  check(cache.ReadRaw(0, sector) == 0 && sector == std::array<uint8_t, 512>{}, "fresh raw device is not zero");
  check(cache.Mount() != 0, "mount bypassed live raw handle");
  check(cache.Dismount() == 0xc000014f, "unformatted raw device was accepted");
  std::array<uint8_t, 512> marker;
  marker.fill(0xa7);
  check(cache.WriteRaw(CacheFatx::Capacity - 512, marker) == 0, "last raw sector rejected");
  for (const uint64_t offset : {1ull, uint64_t(CacheFatx::Capacity), 0xffffffffffffffffull}) {
    sector.fill(0x51);
    check(cache.ReadRaw(offset, sector) != 0 && sector[0] == 0x51, "invalid read changed output");
    check(cache.WriteRaw(offset, marker) != 0, "invalid raw range accepted");
  }
  check(cache.WriteRaw(0, std::span(marker).first(511)) != 0, "unaligned raw length accepted");
  check(cache.ReadRaw(0, {}) != 0, "empty raw transfer accepted");
  std::vector<uint8_t> excessive(CacheFatx::MaximumTransfer + 512);
  check(cache.WriteRaw(0, excessive) != 0, "oversized transfer accepted");
  writeFormat(cache, bytes);
  std::vector<uint8_t> retained(bytes.size());
  check(cache.ReadRaw(0, retained) == 0 && retained == bytes, "raw formatter bytes were rewritten");
  check(cache.WriteRaw(CacheFatx::Capacity - 512, std::span(retained).first(1024)) != 0, "crossing partition end was accepted");
  check(cache.WriteRaw(512, std::span(retained).first(4095)) != 0, "invalid write changed valid format");
  check(cache.ReadRaw(0, retained) == 0 && retained == bytes, "failed raw write changed valid FATX");
  check(cache.ReadRaw(CacheFatx::Capacity - 512, sector) == 0 && sector == marker, "formatter changed untouched tail bytes");
  check(cache.Dismount() == 0 && !cache.Filesystem() && !cache.Mounted(), "dismount published a mount");
  check(cache.Dismount() == 0 && !cache.Filesystem(), "repeated dismount mounted or unsealed format");
  check(cache.Mount() != 0, "mount bypassed final raw close");
  check(cache.ReadRaw(0, sector) != 0 && cache.WriteRaw(0, marker) != 0, "sealed format still permits raw I/O");
  cache.CloseRaw();
  check(cache.Dismount() != 0 && !cache.Filesystem(), "closed handle dismounted or mounted volume");
  std::array<std::shared_ptr<browser::TemporaryFilesystem>, 2> mounts;
  std::thread mounter([&] {
    check(cache.Mount() == 0, "concurrent lazy mount failed");
    mounts[0] = cache.Filesystem();
  });
  check(cache.Mount() == 0 && cache.Mounted(), "valid guest FATX failed lazy mount");
  mounts[1] = cache.Filesystem();
  mounter.join();
  const auto filesystem = cache.Filesystem();
  check(mounts[0] == filesystem && mounts[1] == filesystem, "concurrent mount published detached namespaces");
  check(filesystem && filesystem->Stat("")->directory, "mounted namespace lacks root");
  check(filesystem->Usage().capacity == 1023 * 16384 && filesystem->Usage().allocated == 16384, "FATX accounting includes metadata or tail");
  check(cache.Mount() == 0 && cache.Filesystem() == filesystem, "repeated mount replaced namespace");
  check(filesystem->CreateFile("CACHE.BIN", 0x80) == 0 && filesystem->Write("CACHE.BIN", 0, marker) == 0, "mounted cache is not writable");
  check(filesystem->Read("CACHE.BIN", 0, sector) == 512 && sector == marker, "mounted namespace lost bytes");
  check(filesystem->Usage().allocated == 32768, "mounted allocations disagree with cluster size");
  check(cache.OpenRaw() != 0 && cache.ReadRaw(0, sector) != 0 && cache.WriteRaw(0, marker) != 0, "mounted namespace exposes stale raw snapshot");
  cache.CloseRaw();
  check(cache.Dismount() != 0 && cache.Filesystem() == filesystem, "late raw control destroyed mounted state");
  std::vector<uint8_t> fill(1022 * 16384);
  check(filesystem->Write("CACHE.BIN", 0, fill) == 0 && filesystem->Usage().allocated == filesystem->Usage().capacity, "true formatted capacity cannot be allocated");
  check(filesystem->CreateFile("OVERFLOW", 0x80) == 0 && filesystem->Write("OVERFLOW", 0, marker) == 0xc000007f, "mounted quota permits metadata or tail allocation");
  check(filesystem->Stat("OVERFLOW")->size == 0, "quota failure partially wrote file");

  for (uint32_t n = 0; n < 12; ++n) {
    CacheFatx invalid;
    auto malformed = bytes;
    if (n == 0) { malformed[0] = 0; }
    if (n == 1) { put32(malformed, 8, 0); }
    if (n == 2) { put32(malformed, 8, 8); }
    if (n == 3) { put32(malformed, 8, 31); }
    if (n == 4) { put32(malformed, 12, 2); }
    if (n == 5) { malformed[16] = 1; }
    if (n == 6) { malformed[4096] = 0; }
    if (n == 7) { malformed[4098] = 0; }
    if (n == 8) { malformed[4100] = 0xff; }
    if (n == 9) { malformed[8192] = 1; }
    if (n == 10) { malformed[8192 + 16383] = 0; }
    if (n == 11) { malformed.resize(8192); }
    check(invalid.OpenRaw() == 0, "invalid fixture raw open failed");
    writeFormat(invalid, malformed);
    check(invalid.Dismount() == 0xc000014f && !invalid.Filesystem(), "corrupt or unsupported FATX became namespace");
    invalid.CloseRaw();
    check(invalid.Mount() != 0 && !invalid.Filesystem(), "failed dismount still mounted");
    check(invalid.OpenRaw() == 0, "failed format cannot be retried");
    writeFormat(invalid, bytes);
    check(invalid.Dismount() == 0, "corrected guest format was rejected");
    invalid.CloseRaw();
    check(invalid.Mount() == 0, "corrected guest format failed mount");
  }

  CacheFatx incomplete;
  check(incomplete.OpenRaw() == 0, "incomplete fixture open failed");
  writeFormat(incomplete, bytes);
  incomplete.CloseRaw();
  check(incomplete.Mount() != 0 && !incomplete.Filesystem(), "close substituted for dismount");
  check(incomplete.OpenRaw() == 0 && incomplete.Dismount() == 0, "reopen cannot complete dismount");
  incomplete.CloseRaw();
  check(incomplete.OpenRaw() == 0 && incomplete.Mount() != 0, "reopen retained stale dismount barrier");
  incomplete.CloseRaw();
  check(incomplete.Mount() != 0, "reopened raw phase mounted without new dismount");

  CacheFatx concurrent;
  check(concurrent.OpenRaw() == 0, "concurrent raw open failed");
  std::vector<uint8_t> maximum(CacheFatx::MaximumTransfer, 0x69);
  check(concurrent.WriteRaw(CacheFatx::Capacity - maximum.size(), maximum) == 0, "maximum aligned tail transfer failed");
  std::vector<uint8_t> maximumOutput(maximum.size());
  check(concurrent.ReadRaw(CacheFatx::Capacity - maximum.size(), maximumOutput) == 0 && maximumOutput == maximum, "maximum raw transfer changed bytes");
  std::thread writer([&] {
    std::array<uint8_t, 512> update;
    for (uint32_t n = 0; n < 1000; ++n) {
      update.fill(uint8_t(n));
      check(concurrent.WriteRaw(32768, update) == 0, "concurrent raw write failed");
    }
  });
  for (uint32_t n = 0; n < 1000; ++n) {
    check(concurrent.ReadRaw(32768, sector) == 0, "concurrent raw read failed");
    for (const auto value : sector) { check(value == sector[0], "torn raw sector observed"); }
  }
  writer.join();
  CacheFatx isolated;
  check(!isolated.Filesystem() && isolated.OpenRaw() == 0, "session namespace leaked");
  check(isolated.ReadRaw(32768, sector) == 0 && sector == std::array<uint8_t, 512>{}, "session raw bytes leaked");
  std::puts("cache-fatx-contracts-ok");
}
