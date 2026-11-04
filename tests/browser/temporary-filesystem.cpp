#include "browser/temporary-filesystem.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>
#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

constexpr size_t MiB = 1024 * 1024;
std::atomic<size_t> liveBytes{0};
std::atomic<size_t> liveLargeBytes{0};
std::atomic<size_t> peakBytes{0};
std::atomic<size_t> totalBytes{0};
std::atomic<size_t> allocationBudget{std::numeric_limits<size_t>::max()};
std::atomic<size_t> failAllocation{0};
size_t completedBytes = 0;

struct alignas(std::max_align_t) AllocationHeader {
  size_t size;
};

void* operator new(size_t size) {
  if ((failAllocation.load() && failAllocation.fetch_sub(1) == 1) ||
      size > std::numeric_limits<size_t>::max() - sizeof(AllocationHeader) ||
      size > allocationBudget.load() - liveBytes.load()) { throw std::bad_alloc(); }
  auto* allocation = static_cast<AllocationHeader*>(std::malloc(sizeof(AllocationHeader) + std::max<size_t>(size, 1)));
  if (!allocation) { throw std::bad_alloc(); }
  allocation->size = size;
  if (size >= 65536) { liveLargeBytes.fetch_add(size); }
  const auto live = liveBytes.fetch_add(size) + size;
  auto peak = peakBytes.load();
  while (peak < live && !peakBytes.compare_exchange_weak(peak, live)) {}
  totalBytes.fetch_add(size);
  return allocation + 1;
}

void operator delete(void* pointer) noexcept {
  if (!pointer) { return; }
  auto* allocation = static_cast<AllocationHeader*>(pointer) - 1;
  liveBytes.fetch_sub(allocation->size);
  if (allocation->size >= 65536) { liveLargeBytes.fetch_sub(allocation->size); }
  std::free(allocation);
}

void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, size_t) noexcept { ::operator delete(pointer); }

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

const auto sameMetadata = [](const browser::TemporaryFilesystem::Metadata& left, const browser::TemporaryFilesystem::Metadata& right) {
  return left.directory == right.directory && left.size == right.size && left.attributes == right.attributes &&
    left.creationTime == right.creationTime && left.lastAccessTime == right.lastAccessTime &&
    left.lastWriteTime == right.lastWriteTime && left.changeTime == right.changeTime && left.allocationSize == right.allocationSize;
};

const auto sameUsage = [](const browser::TemporaryFilesystem::Accounting& left, const browser::TemporaryFilesystem::Accounting& right) {
  return left.capacity == right.capacity && left.allocated == right.allocated && left.entries == right.entries;
};

const auto pattern = [](size_t offset) { return uint8_t((offset * 37 + (offset >> 13) * 19 + 0x53) & 0xff); };

const auto peakCopy = [] {
#ifdef __EMSCRIPTEN__
  check(emscripten_get_heap_size() == 256 * MiB, "Wasm fixture must use exactly 256 MiB linear memory");
#else
  allocationBudget = 252 * MiB;
#endif
  std::vector<uint8_t> retained(184 * MiB);
  for (size_t n = 0; n < retained.size(); ++n) { retained[n] = pattern(n + 123); }
  uint64_t clock = 100;
  browser::TemporaryFilesystem storage(64 * MiB, 256, [&] { return clock; });
  browser::TemporaryFilesystem isolated(64 * MiB);
  check(storage.CreateFile("PAYLOAD.BIN", 0x80) == 0, "copy destination create failed");
  std::array<uint8_t, 65536> chunk{};
  constexpr size_t fileSize = 18 * MiB;
  for (size_t n = 0; n < fileSize / chunk.size(); ++n) {
    const auto offset = n * chunk.size();
    for (size_t n = 0; n < chunk.size(); ++n) { chunk[n] = pattern(offset + n); }
    ++clock;
    check(storage.Write("PAYLOAD.BIN", offset, chunk) == 0, "bounded chunk copy returned failure");
    completedBytes = offset + chunk.size();
    const auto metadata = storage.Stat("PAYLOAD.BIN");
    check(metadata && metadata->size == completedBytes && metadata->lastWriteTime == clock && metadata->changeTime == clock,
      "copy published fake size or stale timestamps");
    check(storage.Usage().allocated == completedBytes + browser::TemporaryFilesystem::AllocationUnit,
      "copy accounting differs from committed bytes");
  }
  check(storage.Stat("PAYLOAD.BIN")->size == fileSize && !isolated.Stat("PAYLOAD.BIN"), "copy size or mount isolation wrong");
  const auto committed = *storage.Stat("PAYLOAD.BIN");
  const auto committedUsage = storage.Usage();
  const auto committedStorage = liveLargeBytes.load();
  ++clock;
  bool failed = false;
  try { storage.Write("PAYLOAD.BIN", 40 * MiB, chunk); } catch (const std::bad_alloc&) { failed = true; }
  check(failed && sameMetadata(committed, *storage.Stat("PAYLOAD.BIN")) && sameUsage(committedUsage, storage.Usage()) &&
    liveLargeBytes == committedStorage, "large growth allocation failure changed committed file or retained storage");
  for (size_t n = 0; n < fileSize / chunk.size(); ++n) {
    const auto offset = n * chunk.size();
    check(storage.Read("PAYLOAD.BIN", offset, chunk) == chunk.size(), "full copied bytes missing");
    for (size_t n = 0; n < chunk.size(); ++n) { check(chunk[n] == pattern(offset + n), "copied byte mismatch"); }
  }
  check(storage.Read("PAYLOAD.BIN", fileSize, chunk) == 0, "copy read passed exact EOF");
  for (size_t n = 0; n < retained.size(); ++n) { check(retained[n] == pattern(n + 123), "retained synthetic memory corrupted"); }
  check(peakBytes <= 233 * MiB, "copy peak retained more than old plus new geometric storage");
  check(totalBytes <= 249 * MiB, "chunk copying still allocates quadratically");
  std::printf("temporary-filesystem-peak-ok copied=%zu retained=%zu peak=%zu cumulative=%zu capacity=%llu allocated=%llu rollback=bad_alloc\n",
    fileSize, retained.size(), peakBytes.load(), totalBytes.load(),
    static_cast<unsigned long long>(storage.Usage().capacity), static_cast<unsigned long long>(storage.Usage().allocated));
};

struct ClockFailure {};

const auto reservations = [] {
  using Storage = browser::TemporaryFilesystem;
  constexpr uint64_t unit = Storage::AllocationUnit;
  Storage storage(3 * unit);
  check(storage.CreateFile("META", 4, 1) == 0, "initial allocation hint refused");
  check(storage.Stat("META")->size == 0 && storage.Stat("META")->allocationSize == unit &&
    storage.Usage().allocated == 2 * unit, "reservation changed EOF or escaped quota");
  check(storage.CreateFile("FULL", 0x80, unit) == 0 && storage.Usage().allocated == 3 * unit,
    "exact allocation reservation failed");
  const auto before = storage.Usage();
  check(storage.CreateFile("OVER", 0x80, 1) == 0xc000007f && !storage.Stat("OVER") &&
    sameUsage(before, storage.Usage()), "failed reservation published partial ownership");
  const std::array<uint8_t, 3> bytes{1, 2, 3};
  check(storage.Write("META", 0, bytes) == 0 && storage.Stat("META")->size == 3 &&
    sameUsage(before, storage.Usage()), "write double charged reserved allocation");
  check(storage.WriteFile("META", {}) == 0 && storage.Stat("META")->size == 0 &&
    storage.Stat("META")->allocationSize == unit && sameUsage(before, storage.Usage()), "replacement lost reservation");
  check(storage.Write("META", unit, bytes) == 0xc000007f && storage.Stat("META")->size == 0,
    "reservation bypassed growth quota");
  check(storage.OverwriteFile("META", 0x80) == 0 && storage.Usage().allocated == 2 * unit &&
    storage.Stat("META")->allocationSize == 0, "overwrite did not release reservation");
  check(storage.CreateFile("NEGATIVE", 0x80, UINT64_MAX) == 0xc000000d &&
    storage.CreateFile("HUGE", 0x80, 4 * unit) == 0xc000007f, "invalid allocation hints were rounded or accepted");
};

const auto resizing = [] {
  using Storage = browser::TemporaryFilesystem;
  uint64_t clock = 100;
  bool failClock = false;
  Storage storage(4 * Storage::AllocationUnit, 32, [&] {
    if (failClock) { throw ClockFailure{}; }
    return clock;
  });
  const std::array<uint8_t, 3> bytes{1, 2, 3};
  check(storage.WriteFile("SAVE", bytes) == 0, "resize fixture creation failed");
  ++clock;
  check(storage.Resize("SAVE", 32768) == 0 && storage.Stat("SAVE")->size == 32768 &&
    storage.Stat("SAVE")->lastWriteTime == clock && storage.Usage().allocated == 49152, "resize growth metadata or quota wrong");
  std::vector<uint8_t> result(32768);
  check(storage.Read("SAVE", 0, result) == result.size() && std::equal(bytes.begin(), bytes.end(), result.begin()) &&
    std::all_of(result.begin() + 3, result.end(), [](uint8_t value) { return !value; }), "resize did not preserve bytes and zero growth");
  const auto before = *storage.Stat("SAVE");
  const auto usage = storage.Usage();
  check(storage.Resize("SAVE", 65536) == 0xc000007f && storage.Resize("SAVE", UINT64_MAX) == 0xc000000d &&
    sameMetadata(before, *storage.Stat("SAVE")) && sameUsage(usage, storage.Usage()), "failed resize mutated state");
  failAllocation = 1;
  bool allocationFailed = false;
  try { storage.Resize("SAVE", 49152); } catch (const std::bad_alloc&) { allocationFailed = true; }
  failAllocation = 0;
  check(allocationFailed && sameMetadata(before, *storage.Stat("SAVE")) && sameUsage(usage, storage.Usage()),
    "resize allocation failure changed committed data or quota");
  failClock = true;
  bool failed = false;
  try { storage.Resize("SAVE", 1); } catch (const ClockFailure&) { failed = true; }
  check(failed && sameMetadata(before, *storage.Stat("SAVE")), "resize clock failure truncated data");
  failClock = false;
  check(storage.Resize("SAVE", 1) == 0 && storage.Usage().allocated == 32768 && storage.Resize("SAVE", 3) == 0,
    "truncate did not release quota");
  std::array<uint8_t, 3> tail{};
  check(storage.Read("SAVE", 0, tail) == 3 && tail == std::array<uint8_t, 3>{1, 0, 0}, "truncated bytes reappeared after growth");
  check(storage.Reserve("SAVE", 32768) == 0 && storage.Stat("SAVE")->size == 3 &&
    storage.Stat("SAVE")->allocationSize == 32768 && storage.Usage().allocated == 49152,
    "allocation information changed EOF or lost reservation");
  check(storage.Resize("SAVE", 4096) == 0 && storage.Stat("SAVE")->allocationSize == 32768 &&
    storage.Resize("SAVE", 3) == 0 && storage.Stat("SAVE")->allocationSize == 16384 && storage.Reserve("SAVE", 32768) == 0,
    "EOF growth lost preallocation or truncation retained excess allocation");
  check(storage.Reserve("SAVE", 65536) == 0xc000007f && storage.Reserve("SAVE", UINT64_MAX) == 0xc000000d &&
    storage.Stat("SAVE")->size == 3 && storage.Stat("SAVE")->allocationSize == 32768,
    "failed reservation mutated the file");
  check(storage.Reserve("SAVE", 1) == 0 && storage.Stat("SAVE")->size == 1 &&
    storage.Stat("SAVE")->allocationSize == 16384 && storage.Resize("SAVE", 3) == 0,
    "allocation shrink failed to truncate or release quota");
  const std::array<uint64_t, 4> times{10, 20, 30, 40};
  check(storage.SetBasic("SAVE", times, 1) == 0 && storage.Stat("SAVE")->creationTime == 10 &&
    storage.Stat("SAVE")->lastAccessTime == 20 && storage.Stat("SAVE")->lastWriteTime == 30 &&
    storage.Stat("SAVE")->changeTime == 40 && storage.Resize("SAVE", 1) == 0xc0000022, "basic metadata or readonly enforcement wrong");
  check(storage.SetBasic("SAVE", {}, 0x80) == 0 && storage.Stat("SAVE")->creationTime == 10 &&
    storage.Resize("SAVE", 1) == 0, "basic metadata could not preserve times and clear readonly");
  check(storage.SetBasic("SAVE", {}, 0x10) == 0xc000000d && storage.SetBasic("SAVE", {}, 0x80000000) == 0xc000000d &&
    storage.Resize("", 1) == 0xc00000ba && storage.Resize("ABSENT", 1) == 0xc0000034, "invalid metadata or resize accepted");
};

const auto contracts = [] {
  using browser::TemporaryFilesystem;
  {
    TemporaryFilesystem files(4 * TemporaryFilesystem::AllocationUnit);
    const std::array<uint8_t, 3> bytes{3, 4, 5};
    check(files.WriteFile("FIRST", bytes) == 0 && files.CreateFile("SECOND", 0x80, 16384) == 0, "rename fixtures failed");
    const auto before = files.Usage();
    check(files.Rename("FIRST", "SECOND", false) == 0xc0000035 && sameUsage(before, files.Usage()), "rename collision changed storage");
    check(files.Rename("FIRST", "MISSING\\CHILD", true) == 0xc000003a && files.Stat("FIRST"), "rename synthesized a parent");
    check(files.Rename("FIRST", "SECOND", true) == 0 && !files.Stat("FIRST") && files.Stat("SECOND")->size == 3 &&
      files.Usage().allocated == before.allocated - 16384 && files.Usage().entries == before.entries - 1, "replace rename lost bytes or quota");
    std::array<uint8_t, 3> read{};
    check(files.Read("SECOND", 0, read) == 3 && read == bytes, "rename changed owned bytes");
    check(files.Delete("SECOND") == 0 && files.Usage().allocated == 16384 && files.Usage().entries == 1 &&
      files.Delete("SECOND") == 0xc0000034, "delete did not release exact ownership");
    check(files.CreateDirectory("DIR") == 0 && files.CreateFile("DIRA", 0x80) == 0 && files.WriteFile("DIR\\CHILD", bytes) == 0 &&
      files.Delete("DIR") == 0xc0000101 && files.Delete("") == 0xc0000022 && files.Delete("..\\CHILD") == 0xc0000033,
      "deletion escaped its namespace or removed a nonempty directory");
    check(files.CreateFile("READONLY", 1) == 0 && files.Delete("READONLY") == 0xc0000022 &&
      files.Rename("READONLY", "OTHER", false) == 0xc0000022, "readonly file mutation succeeded");
  }
  uint64_t clock = 100;
  size_t clockCalls = 0;
  bool failClock = false;
  TemporaryFilesystem storage(4 * TemporaryFilesystem::AllocationUnit, 8, [&] {
    ++clockCalls;
    if (failClock) { throw ClockFailure{}; }
    return clock;
  });
  check(storage.CreateFile("DATA", 4) == 0, "file create failed");
  const auto created = *storage.Stat("DATA");
  std::array<uint8_t, 3> source{7, 8, 9};
  ++clock;
  check(storage.Write("DATA", 5, source) == 0, "offset-gap write failed");
  std::array<uint8_t, 8> result{};
  const auto written = *storage.Stat("DATA");
  check(written.size == 8 && written.attributes == 4 && written.creationTime == created.creationTime &&
    written.lastAccessTime == created.lastAccessTime && written.lastWriteTime == clock && written.changeTime == clock,
    "write metadata changed unrelated fields");
  check(storage.Read("DATA", 0, result) == result.size() && result == std::array<uint8_t, 8>{0, 0, 0, 0, 0, 7, 8, 9},
    "offset gap not zero-filled or payload wrong");
  check(storage.Usage().capacity == 65536 && storage.Usage().allocated == 32768 && storage.Usage().entries == 2,
    "initial quota accounting wrong");
  const auto snapshot = *storage.Stat("DATA");
  const auto usage = storage.Usage();
  const auto calls = clockCalls;
  failClock = true;
  for (const uint64_t offset : {0ull, 1000ull, 32768ull}) {
    bool failed = false;
    try { storage.Write("DATA", offset, source); } catch (const ClockFailure&) { failed = true; }
    check(failed && sameMetadata(snapshot, *storage.Stat("DATA")) && sameUsage(usage, storage.Usage()),
      "throwing clock changed bytes, EOF, metadata or accounting");
  }
  check(clockCalls == calls + 3, "writes did not preflight clock once");
  failClock = false;
  check(storage.Read("DATA", 0, result) == result.size() && result == std::array<uint8_t, 8>{0, 0, 0, 0, 0, 7, 8, 9},
    "clock failure partially overwrote existing data");
  std::array<uint8_t, 1024> growingSource{};
  growingSource.fill(0xa5);
  for (const uint64_t offset : {0ull, 1024ull, 32768ull}) {
    const auto before = *storage.Stat("DATA");
    const auto beforeStorage = liveBytes.load();
    failAllocation = 1;
    bool failed = false;
    try { storage.Write("DATA", offset, growingSource); } catch (const std::bad_alloc&) { failed = true; }
    failAllocation = 0;
    check(failed && sameMetadata(before, *storage.Stat("DATA")) && sameUsage(usage, storage.Usage()) && liveBytes == beforeStorage,
      "allocation failure changed EOF, metadata, accounting or retained storage");
    check(storage.Read("DATA", 0, result) == result.size() && result == std::array<uint8_t, 8>{0, 0, 0, 0, 0, 7, 8, 9},
      "allocation failure partially changed bytes");
  }
  ++clock;
  failAllocation = 1;
  const auto overwriteStatus = storage.Write("DATA", 0, source);
  failAllocation = 0;
  check(overwriteStatus == 0, "allocation-free in-place overwrite failed");
  check(storage.Stat("DATA")->size == 8 && sameUsage(usage, storage.Usage()), "in-place overwrite changed size or quota");
  check(storage.Read("DATA", 0, result) == result.size() && result == std::array<uint8_t, 8>{7, 8, 9, 0, 0, 7, 8, 9},
    "in-place overwrite changed untouched bytes");
  check(storage.CreateFile("READONLY", 1) == 0 && storage.CreateDirectory("DIR") == 0, "status setup failed");
  const auto beforeInvalid = *storage.Stat("DATA");
  const auto beforeInvalidUsage = storage.Usage();
  const auto beforeInvalidCalls = clockCalls;
  check(storage.Write("READONLY", 0, source) == 0xc0000022, "read-only write accepted");
  check(storage.Write("DIR", 0, source) == 0xc00000ba, "directory write accepted");
  check(storage.Write("MISSING", 0, source) == 0xc0000034, "missing write status wrong");
  check(storage.Write("..", 0, source) == 0xc0000033, "invalid path status wrong");
  check(storage.Write("DATA", 65536, source) == 0xc000007f &&
    storage.Write("DATA", std::numeric_limits<uint64_t>::max(), source) == 0xc000007f,
    "overflowing offset write accepted");
  check(storage.Write("DATA", 32768, source) == 0xc000007f, "global quota ignored another directory allocation");
  check(storage.Write("DATA", std::numeric_limits<uint64_t>::max(), {}) == 0, "empty write changed contract");
  check(clockCalls == beforeInvalidCalls && sameMetadata(beforeInvalid, *storage.Stat("DATA")) &&
    sameUsage(beforeInvalidUsage, storage.Usage()), "rejected or empty write changed state or clock");
  ++clock;
  check(storage.Write("DATA", 32767, std::span(source).first(1)) == 0, "exact remaining quota write failed");
  check(storage.Stat("DATA")->size == 32768 && storage.Usage().allocated == storage.Usage().capacity,
    "exact quota size or accounting wrong");
  std::vector<uint8_t> full(32768);
  check(storage.Read("DATA", 0, full) == full.size(), "growing write missing bytes");
  check(std::equal(result.begin(), result.end(), full.begin()) && full.back() == source[0] &&
    std::all_of(full.begin() + result.size(), full.end() - 1, [](uint8_t byte) { return byte == 0; }),
    "growing write lost prefix or zero-filled gap");
  TemporaryFilesystem isolated(65536);
  check(!isolated.Stat("DATA") && isolated.Usage().allocated == TemporaryFilesystem::AllocationUnit, "filesystem instances share data");
  const auto beforeTruncate = *storage.Stat("DATA");
  failClock = true;
  bool failed = false;
  try { storage.OverwriteFile("DATA", 0x80); } catch (const ClockFailure&) { failed = true; }
  check(failed && sameMetadata(beforeTruncate, *storage.Stat("DATA")) && storage.Usage().allocated == 65536,
    "truncate clock failure changed allocation or metadata");
  failClock = false;
  ++clock;
  check(storage.OverwriteFile("DATA", 0x80) == 0 && storage.Stat("DATA")->size == 0 && storage.Usage().allocated == 32768,
    "truncate did not release file allocation");
  check(storage.Write("DATA", 0, source) == 0 && storage.Read("DATA", 0, result) == source.size() &&
    std::equal(source.begin(), source.end(), result.begin()), "truncated file failed rewrite");
  std::puts("temporary-filesystem-contracts-ok");
};

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::strcmp(argv[1], "peak") == 0) { peakCopy(); }
    else { reservations(); resizing(); contracts(); }
    return 0;
  } catch (const std::bad_alloc&) {
    std::fprintf(stderr, "temporary-filesystem-bad-alloc copied=%zu peak=%zu cumulative=%zu\n",
      completedBytes, peakBytes.load(), totalBytes.load());
  } catch (const std::exception& error) {
    std::fprintf(stderr, "temporary-filesystem-failed: %s\n", error.what());
  } catch (...) {
    std::fputs("temporary-filesystem-failed: unknown exception\n", stderr);
  }
  return 1;
}
