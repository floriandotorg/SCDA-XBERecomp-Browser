#include "browser/cache-fatx.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

constexpr uint64_t partitionBytes = 64 * 1024 * 1024;
constexpr uint32_t clusterBytes = 16384;
constexpr uint32_t headerBytes = 4096;
constexpr uint32_t fatBytes = (((partitionBytes / clusterBytes + 1) * 2 + 4095) / 4096) * 4096;
constexpr uint32_t rootOffset = headerBytes + fatBytes;
constexpr uint64_t usableBytes = ((partitionBytes - rootOffset) / clusterBytes) * clusterBytes;
std::atomic<size_t> rawAllocations{0};
std::atomic<size_t> failAllocation{0};

struct alignas(std::max_align_t) AllocationHeader {
  size_t size;
};

void* operator new(size_t size) {
  if ((failAllocation.load() && failAllocation.fetch_sub(1) == 1) ||
      size > std::numeric_limits<size_t>::max() - sizeof(AllocationHeader)) { throw std::bad_alloc(); }
  auto* allocation = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + std::max<size_t>(size, 1)));
  if (!allocation) { throw std::bad_alloc(); }
  allocation->size = size;
  if (size == partitionBytes) { ++rawAllocations; }
  return allocation + 1;
}

void operator delete(void* pointer) noexcept {
  if (!pointer) { return; }
  auto* allocation = static_cast<AllocationHeader*>(pointer) - 1;
  if (allocation->size == partitionBytes) { --rawAllocations; }
  std::free(allocation);
}

void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, size_t) noexcept { ::operator delete(pointer); }

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

const auto put32 = [](std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  for (uint32_t n = 0; n < 4; ++n) { bytes[offset + n] = uint8_t(value >> (n * 8)); }
};

const auto formatted = [](uint32_t tableBytes = fatBytes) {
  std::vector<uint8_t> bytes(headerBytes + tableBytes + clusterBytes, 0xff);
  put32(bytes, 0, 0x58544146);
  put32(bytes, 4, 0x12345678);
  put32(bytes, 8, 32);
  put32(bytes, 12, 1);
  bytes[16] = bytes[17] = 0;
  std::fill(bytes.begin() + headerBytes, bytes.begin() + headerBytes + tableBytes, 0);
  bytes[4096] = 0xf8;
  bytes[4097] = bytes[4098] = bytes[4099] = 0xff;
  return bytes;
};

const auto writeFormat = [](browser::CacheFatx& cache, const std::vector<uint8_t>& bytes) {
  for (size_t n = 0; n < bytes.size() / 4096; ++n) {
    check(cache.WriteRaw(n * 4096, std::span(bytes).subspan(n * 4096, 4096)) == 0, "guest format write failed");
  }
};

const auto mount = [](browser::CacheFatx& cache) {
  check(cache.OpenRaw() == 0, "format open failed");
  writeFormat(cache, formatted());
  check(cache.Dismount() == 0, "capacity-derived formatter bytes rejected");
  cache.CloseRaw();
  check(cache.Mount() == 0, "validated format failed mount");
  return cache.Filesystem();
};

const auto lazyBacking = [] {
  using browser::CacheFatx;
  std::array<CacheFatx, 8> discOnly;
  check(rawAllocations == 0, "disc-only sessions allocated raw partition storage");
  for (auto& session : discOnly) {
    check(session.RawBackingBytes() == 0, "constructor reports allocated raw backing");
    check(session.Mount() == 0xc00000a3 && session.Dismount() == 0xc00000a3,
      "unopened session accepted mount or dismount");
    check(session.RawBackingBytes() == 0, "unopened lifecycle allocated raw backing");
  }
  auto& cache = discOnly[0];
  std::array<uint8_t, 512> sector;
  sector.fill(0x51);
  failAllocation = 1;
  check(cache.OpenRaw() == 0xc000009a, "raw allocation failure did not return INSUFFICIENT_RESOURCES");
  check(failAllocation == 0 && rawAllocations == 0 && cache.RawBackingBytes() == 0,
    "failed open published or retained raw allocation");
  check(!cache.Mounted() && !cache.Filesystem() && cache.Mount() == 0xc00000a3 && cache.Dismount() == 0xc00000a3,
    "failed raw open changed lifecycle flags");
  check(cache.ReadRaw(0, sector) == 0xc00000a3 && sector[0] == 0x51 && cache.WriteRaw(0, sector) == 0xc00000a3,
    "failed raw open published an I/O lease or changed output");
  cache.CloseRaw();
  check(cache.OpenRaw() == 0 && cache.RawBackingBytes() == partitionBytes && rawAllocations == 1,
    "successful open did not allocate exactly one 64 MiB backing");
  std::vector<uint8_t> chunk(CacheFatx::MaximumTransfer, 0x51);
  for (uint64_t n = 0; n < partitionBytes / chunk.size(); ++n) {
    check(cache.ReadRaw(n * chunk.size(), chunk) == 0 &&
      std::all_of(chunk.begin(), chunk.end(), [](uint8_t value) { return value == 0; }),
      "fresh raw partition contains nonzero bytes");
  }
  sector.fill(0xa7);
  check(cache.WriteRaw(partitionBytes - sector.size(), sector) == 0, "lazy backing lost tail write");
  failAllocation = 1;
  check(cache.OpenRaw() == 0xc0000043 && failAllocation == 1 && rawAllocations == 1,
    "busy raw open allocated or changed exclusive status");
  cache.CloseRaw();
  check(cache.RawBackingBytes() == partitionBytes && cache.OpenRaw() == 0 && failAllocation == 1,
    "reopen allocated again or discarded retained raw bytes");
  failAllocation = 0;
  std::array<uint8_t, 512> retained{};
  check(cache.ReadRaw(partitionBytes - retained.size(), retained) == 0 && retained == sector,
    "close/reopen lost exact raw bytes");
  check(cache.Dismount() == 0xc000014f && cache.RawBackingBytes() == partitionBytes,
    "blank format passed validation or discarded retained bytes");
  auto& isolated = discOnly[1];
  check(isolated.RawBackingBytes() == 0 && isolated.OpenRaw() == 0 && rawAllocations == 2,
    "separate session did not own independent lazy backing");
  check(isolated.ReadRaw(partitionBytes - retained.size(), retained) == 0 && retained == std::array<uint8_t, 512>{},
    "lazy session inherited another session's bytes");
};

const auto lifecycle = [] {
  using browser::CacheFatx;
  check(CacheFatx::Capacity == partitionBytes && fatBytes == 12288 && rootOffset == 16384, "64 MiB formatter geometry regressed");
  const auto bytes = formatted();
  CacheFatx cache;
  check(rawAllocations == 0, "constructor eagerly allocated raw partition backing");
  std::array<uint8_t, 512> sector{};
  check(!cache.Filesystem() && !cache.Mounted(), "blank cache published a namespace");
  check(cache.Mount() != 0 && cache.Dismount() != 0, "blank cache accepted lifecycle transition");
  check(cache.ReadRaw(0, sector) != 0 && cache.WriteRaw(0, sector) != 0, "raw I/O bypassed open lease");
  check(cache.OpenRaw() == 0 && cache.OpenRaw() != 0, "raw lease is not exclusive");
  check(cache.ReadRaw(0, sector) == 0 && sector == std::array<uint8_t, 512>{}, "fresh raw device is not zero");
  check(cache.Mount() != 0, "mount bypassed live raw handle");
  check(cache.Dismount() == 0xc000014f && rawAllocations == 1, "invalid format discarded raw backing");
  std::array<uint8_t, 512> marker;
  marker.fill(0xa7);
  check(cache.WriteRaw(partitionBytes - 512, marker) == 0, "last raw sector rejected");
  for (const uint64_t offset : {1ull, uint64_t(partitionBytes), 0xffffffffffffffffull}) {
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
  check(cache.WriteRaw(partitionBytes - 512, std::span(retained).first(1024)) != 0, "crossing partition end accepted");
  check(cache.WriteRaw(512, std::span(retained).first(4095)) != 0, "invalid write changed valid format");
  check(cache.ReadRaw(0, retained) == 0 && retained == bytes, "failed raw write changed FATX");
  check(cache.ReadRaw(partitionBytes - 512, sector) == 0 && sector == marker, "formatter changed untouched tail bytes");
  check(cache.Dismount() == 0 && !cache.Filesystem() && !cache.Mounted(), "dismount published a mount");
  check(cache.Dismount() == 0 && rawAllocations == 1, "repeated dismount released raw backing");
  check(cache.Mount() != 0, "mount bypassed final raw close");
  check(cache.ReadRaw(0, sector) != 0 && cache.WriteRaw(0, marker) != 0, "sealed format permits raw I/O");
  cache.CloseRaw();
  for (size_t n = 1; n <= 2; ++n) {
    failAllocation = n;
    bool failed = false;
    try { cache.Mount(); } catch (const std::bad_alloc&) { failed = true; }
    check(failed && !cache.Mounted() && !cache.Filesystem() && rawAllocations == 1, "failed namespace allocation lost validated raw backing");
  }
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
  check(rawAllocations == 0 && cache.RawBackingBytes() == 0, "successful handoff retained duplicate 64 MiB raw storage");
  check(filesystem && filesystem->Stat("")->directory, "mounted namespace lacks root");
  check(filesystem->Usage().capacity == usableBytes && filesystem->Usage().allocated == clusterBytes, "FATX accounting includes metadata or tail");
  check(cache.Mount() == 0 && cache.Filesystem() == filesystem && rawAllocations == 0, "repeated mount replaced namespace or reallocated raw storage");
  check(filesystem->CreateFile("CACHE.BIN", 0x80) == 0 && filesystem->Write("CACHE.BIN", 0, marker) == 0, "mounted cache is not writable");
  check(filesystem->Read("CACHE.BIN", 0, sector) == 512 && sector == marker, "mounted namespace lost bytes");
  check(filesystem->Usage().allocated == 32768, "mounted allocations disagree with cluster size");
  failAllocation = 1;
  check(cache.OpenRaw() == 0xc00000bb && failAllocation == 1 && cache.RawBackingBytes() == 0,
    "mounted raw open allocated backing or changed unsupported status");
  failAllocation = 0;
  check(cache.ReadRaw(0, sector) != 0 && cache.WriteRaw(0, marker) != 0, "mounted namespace exposes stale raw snapshot");
  cache.CloseRaw();
  check(cache.Dismount() != 0 && cache.Filesystem() == filesystem, "late raw control destroyed mounted state");
};

const auto malformedFormats = [] {
  for (uint32_t n = 0; n < 15; ++n) {
    browser::CacheFatx invalid;
    auto malformed = formatted();
    if (n == 0) { malformed[0] = 0; }
    if (n == 1) { put32(malformed, 8, 0); }
    if (n == 2) { put32(malformed, 8, 8); }
    if (n == 3) { put32(malformed, 8, 31); }
    if (n == 4) { put32(malformed, 12, 2); }
    if (n == 5) { malformed[16] = 1; }
    if (n == 6) { malformed[4096] = 0; }
    if (n == 7) { malformed[4098] = 0; }
    if (n == 8) { malformed[4100] = 0xff; }
    if (n == 9) { malformed[rootOffset] = 1; }
    if (n == 10) { malformed[rootOffset + clusterBytes - 1] = 0; }
    if (n == 11) { malformed.resize(rootOffset); }
    if (n == 12) { malformed = formatted(4096); }
    if (n == 13) { malformed = formatted(8192); }
    if (n == 14) { malformed[headerBytes + fatBytes - 1] = 1; }
    check(invalid.OpenRaw() == 0, "invalid fixture raw open failed");
    writeFormat(invalid, malformed);
    check(invalid.Dismount() == 0xc000014f && !invalid.Filesystem() && rawAllocations == 1, "unsupported FATX became namespace or lost backing");
    invalid.CloseRaw();
    check(invalid.Mount() != 0 && !invalid.Filesystem(), "failed dismount mounted");
    check(invalid.OpenRaw() == 0, "failed format cannot be retried");
    writeFormat(invalid, formatted());
    check(invalid.Dismount() == 0, "corrected guest format rejected");
    invalid.CloseRaw();
    check(invalid.Mount() == 0 && rawAllocations == 0, "corrected format failed handoff");
  }
};

const auto pattern = [](uint64_t offset, uint32_t seed) { return uint8_t((offset * 17 + (offset >> 13) + seed) % 251); };

const auto copyFile = [](const std::shared_ptr<browser::TemporaryFilesystem>& filesystem, const char* name, uint64_t size, uint32_t seed) {
  check(filesystem->CreateFile(name, 0x80) == 0, "copy destination create failed");
  std::vector<uint8_t> chunk(1024 * 1024);
  for (uint64_t n = 0; n < size / chunk.size() + bool(size % chunk.size()); ++n) {
    const auto offset = n * chunk.size();
    const auto length = size_t(std::min<uint64_t>(chunk.size(), size - offset));
    for (size_t n = 0; n < length; ++n) { chunk[n] = pattern(offset + n, seed); }
    check(filesystem->Write(name, offset, std::span(chunk).first(length)) == 0, "copy inside 64 MiB budget returned DISK_FULL");
  }
  check(filesystem->Stat(name)->size == size, "copy changed true file size");
  for (uint64_t n = 0; n < size / chunk.size() + bool(size % chunk.size()); ++n) {
    const auto offset = n * chunk.size();
    const auto length = size_t(std::min<uint64_t>(chunk.size(), size - offset));
    check(filesystem->Read(name, offset, chunk) == length, "copy returned wrong byte count");
    for (size_t n = 0; n < length; ++n) { check(chunk[n] == pattern(offset + n, seed), "copy lost actual source bytes"); }
  }
  chunk[0] = 0x7e;
  check(filesystem->Read(name, size, chunk) == 0 && chunk[0] == 0x7e, "EOF changed output or count");
};

const auto copiesAndQuota = [] {
  browser::CacheFatx cache;
  const auto filesystem = mount(cache);
  check(rawAllocations == 0, "raw backing survived before large copies");
  check(filesystem->CreateDirectory("SOUND") == 0 && filesystem->CreateDirectory("MENU") == 0, "cache directories failed");
  copyFile(filesystem, "SOUND\\LARGE.BIN", 17 * 1024 * 1024 + 512 * 1024 + 13, 23);
  copyFile(filesystem, "SOUND\\SECOND.BIN", 7 * 1024 * 1024 + 512 * 1024 + 7, 41);
  copyFile(filesystem, "MENU\\SCREEN.BIN", 3 * 1024 * 1024 + 19, 79);
  browser::TemporaryFilesystem profile;
  check(profile.Usage().capacity == 16 * 1024 * 1024 && !profile.Stat("SOUND\\LARGE.BIN"), "Partition1 inherited cache capacity or files");
  std::array<uint8_t, 512> marker;
  marker.fill(0x39);
  check(profile.WriteFile("PROFILE", marker) == 0 && !filesystem->Stat("PROFILE"), "profile/cache namespace isolation failed");
  const auto before = filesystem->Usage();
  check(filesystem->CreateFile("FILL", 0x80) == 0, "fill destination create failed");
  const auto available = before.capacity - before.allocated;
  check(filesystem->Write("FILL", available - marker.size(), marker) == 0, "exact namespace capacity rejected");
  check(filesystem->Usage().allocated == usableBytes, "exact capacity left unaccounted storage");
  check(filesystem->Write("FILL", available, marker) == 0xc000007f && filesystem->Stat("FILL")->size == available, "overfull write changed file size");
  check(filesystem->Write("FILL", std::numeric_limits<uint64_t>::max(), marker) == 0xc000007f, "wrapped namespace offset accepted");
  check(filesystem->CreateDirectory("OVERFLOW") == 0xc000007f && filesystem->Usage().allocated == usableBytes, "overfull directory changed quota");
  std::array<uint8_t, 512> output{};
  check(filesystem->Read("FILL", available - marker.size(), output) == marker.size() && output == marker, "failed extension lost retained bytes");
  check(filesystem->Read("SOUND\\LARGE.BIN", 0, output) == output.size(), "quota failure lost earlier copy");
  for (size_t n = 0; n < output.size(); ++n) { check(output[n] == pattern(n, 23), "quota failure changed earlier copy"); }
};

const auto incompleteAndConcurrent = [] {
  using browser::CacheFatx;
  {
    CacheFatx incomplete;
    check(incomplete.OpenRaw() == 0, "incomplete fixture open failed");
    writeFormat(incomplete, formatted());
    incomplete.CloseRaw();
    check(incomplete.Mount() != 0 && !incomplete.Filesystem(), "close substituted for dismount");
    check(incomplete.OpenRaw() == 0 && incomplete.Dismount() == 0, "reopen cannot complete dismount");
    incomplete.CloseRaw();
    check(incomplete.OpenRaw() == 0 && incomplete.Mount() != 0, "reopen retained stale dismount barrier");
    incomplete.CloseRaw();
    check(incomplete.Mount() != 0 && rawAllocations == 1, "reopened phase mounted or discarded raw bytes");
  }
  CacheFatx concurrent;
  check(concurrent.OpenRaw() == 0, "concurrent raw open failed");
  std::vector<uint8_t> maximum(CacheFatx::MaximumTransfer, 0x69);
  check(concurrent.WriteRaw(partitionBytes - maximum.size(), maximum) == 0, "maximum aligned tail transfer failed");
  std::vector<uint8_t> output(maximum.size());
  check(concurrent.ReadRaw(partitionBytes - maximum.size(), output) == 0 && output == maximum, "maximum raw transfer changed bytes");
  std::thread writer([&] {
    std::array<uint8_t, 512> update;
    for (uint32_t n = 0; n < 1000; ++n) {
      update.fill(uint8_t(n));
      check(concurrent.WriteRaw(32768, update) == 0, "concurrent raw write failed");
    }
  });
  std::array<uint8_t, 512> sector{};
  for (uint32_t n = 0; n < 1000; ++n) {
    check(concurrent.ReadRaw(32768, sector) == 0, "concurrent raw read failed");
    for (const auto value : sector) { check(value == sector[0], "torn raw sector observed"); }
  }
  writer.join();
};

const auto run = [] {
  lazyBacking();
  check(rawAllocations == 0, "lazy backing sessions leaked raw storage");
  lifecycle();
  check(rawAllocations == 0, "lifecycle leaked raw storage");
  malformedFormats();
  {
    browser::CacheFatx bounded;
    const auto filesystem = mount(bounded);
    for (size_t n = 0; n < 255; ++n) {
      check(filesystem->CreateFile(std::to_string(n), 0x80) == 0, "entry bound refused within budget");
    }
    check(filesystem->Usage().entries == 256 && filesystem->CreateFile("OVERFLOW", 0x80) == 0xc000009a, "entry bound became unbounded");
    check(filesystem->Usage().allocated == clusterBytes, "empty file entries fabricated content allocation");
  }
  copiesAndQuota();
  incompleteAndConcurrent();
  check(rawAllocations == 0, "retired session retained raw storage");
  browser::CacheFatx isolated;
  std::array<uint8_t, 512> sector{};
  check(!isolated.Filesystem() && isolated.OpenRaw() == 0, "session namespace leaked");
  check(isolated.ReadRaw(32768, sector) == 0 && sector == std::array<uint8_t, 512>{}, "session raw bytes leaked");
  check(isolated.Dismount() == 0xc000014f, "replacement session inherited formatted bytes");
  std::puts("cache-fatx-contracts-ok");
};

int main() {
  try { run(); }
  catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
