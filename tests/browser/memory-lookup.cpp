#include "browser/cpu_runtime.h"
#include <algorithm>
#include <cstdio>

int main() {
  browser::Memory memory;
  X86Context ctx{};
  for (uint32_t n = 0; n < 1024; ++n) {
    memory.Map(0x10000 + n * 0x2000, 0x1000);
    memory.Commit(0x10000 + n * 0x2000, 0x1000);
  }
  const auto sample = [&](uint32_t address) {
    auto best = std::chrono::steady_clock::duration::max();
    for (uint32_t n = 0; n < 3; ++n) {
      const auto start = std::chrono::steady_clock::now();
      uint32_t value = 0;
      for (uint32_t n = 0; n < 100000; ++n) { memory.Read(ctx, address, &value, sizeof(value)); }
      best = std::min(best, std::chrono::steady_clock::now() - start);
    }
    return std::chrono::duration<double, std::milli>(best).count();
  };
  const auto first = sample(0x10000);
  const auto last = sample(0x80E000);
  std::printf("first-region-ms=%.3f last-region-ms=%.3f ratio=%.2f\n", first, last, last / first);
  if (last > first * 10) { return 1; }
  memory.Discard(0x10000, 0x1000);
  const uint32_t expected = 0x12345678;
  memory.Write(ctx, 0x80E000, &expected, sizeof(expected));
  uint32_t value = 0;
  memory.Read(ctx, 0x80E000, &value, sizeof(value));
  if (value != expected) { return 2; }
  memory.Decommit(0x80E000, 0x1000);
  try { memory.Read(ctx, 0x80E000, &value, sizeof(value)); return 3; }
  catch (const browser::Fault&) {}
  memory.Discard(0x80E000, 0x1000);
  memory.Map(0x80E000, 0x1000);
  memory.Commit(0x80E000, 0x1000);
  memory.Read(ctx, 0x80E000, &value, sizeof(value));
  if (value != 0) { return 4; }
  for (uint32_t width : {1u, 2u, 4u, 8u, 16u}) {
    std::array<uint8_t, 32> input{};
    std::array<uint8_t, 32> output{};
    input.fill(0xAB);
    memory.Write(ctx, 0x80E003, input.data(), width);
    memory.Read(ctx, 0x80E003, output.data(), width);
    if (!std::equal(input.begin(), input.begin() + width, output.begin()) || output[width]) { return 5; }
    memory.Decommit(0x80E000, 0x1000);
    bool refused = false;
    try { memory.Write(ctx, 0x80E003, input.data(), width); }
    catch (const browser::Fault&) { refused = true; }
    if (!refused) { return 6; }
    memory.Commit(0x80E000, 0x1000);
  }
  std::puts("memory-lookup-contracts-ok");
}
