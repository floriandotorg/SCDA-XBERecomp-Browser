#include "browser/iso-disc.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
  std::array<uint8_t, 44 * 2048> image{};
  const auto put = [&](size_t at, uint32_t value) {
    for (size_t n = 0; n < 4; ++n) { image[at + n] = uint8_t(value >> (8 * n)); }
  };
  std::memcpy(image.data() + 32 * 2048, "MICROSOFT*XBOX*MEDIA", 20);
  std::memcpy(image.data() + 32 * 2048 + 0x7ec, "MICROSOFT*XBOX*MEDIA", 20);
  put(32 * 2048 + 0x14, 40);
  put(32 * 2048 + 0x18, 28);
  put(40 * 2048 + 4, 41);
  put(40 * 2048 + 8, 7);
  image[40 * 2048 + 13] = 11;
  std::memcpy(image.data() + 40 * 2048 + 14, "default.xbe", 11);
  std::memset(image.data() + 41 * 2048, 7, 7);
  browser::Memory memory;
  memory.Ensure(0x10000, 4096);
  browser::Runtime runtime({}, {});
  X86Context context{};
  kernel::DiscIndex disc;
  int mode = 0;
  size_t reads = 0;
  std::string error;
  assert(disc.Open(image.size(), [&](uint64_t offset, void* destination, uint32_t length) {
    assert(!browser::Memory::HasBorrow());
    ++reads;
    if (offset >= 41 * 2048) {
      if (mode == 1) { memory.Decommit(0x10000, 4096); }
      if (mode == 2) { return 0u; }
      if (mode == 3) { runtime.Cancel(); }
    }
    std::memcpy(destination, image.data() + offset, length);
    return length;
  }, error));
  const auto* file = disc.Find("DEFAULT.XBE");
  assert(file);
  const auto read = [&](uint32_t destination, uint32_t length, uint64_t position = 0) {
    return browser::ReadDiscToGuest(disc, *file, position, memory, runtime, context, destination, length);
  };
  const auto faults = [&](const auto& operation) {
    bool failed = false;
    try { operation(); } catch (const browser::Fault&) { failed = true; }
    assert(failed);
  };
  const size_t mountedReads = reads;
  faults([&] { read(0x10fff, 7); });
  faults([&] { read(0xfffffffe, 7); });
  faults([&] { auto lease = memory.Borrow(); read(0x10000, 7); });
  faults([&] { read(0x10000, (1u << 20) + 1); });
  assert(reads == mountedReads);
  assert(read(0x10000, 7) == 7);
  std::array<uint8_t, 9> output{};
  memory.Read(context, 0x10000, output.data(), output.size());
  assert(output[0] == 7 && output[6] == 7 && output[7] == 0);
  assert(read(0x10000, 7, 5) == 2);
  assert(read(0x10000, 7, 7) == 0);
  mode = 2;
  assert(read(0x10000, 7) == 0);
  mode = 1;
  faults([&] { read(0x10000, 7); });
  memory.Commit(0x10000, 4096);
  memory.Read(context, 0x10000, output.data(), output.size());
  assert(output[0] == 0);
  mode = 3;
  faults([&] { read(0x10000, 7); });
  memory.Read(context, 0x10000, output.data(), output.size());
  assert(output[0] == 0);
  std::puts("iso-guest-contracts-ok");
}
