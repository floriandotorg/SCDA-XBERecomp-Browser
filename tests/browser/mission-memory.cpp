#include "browser/virtual-memory.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

int main() {
  browser::Runtime runtime({}, {});
  browser::Memory memory;
  memory.Ensure(0x10000, 4096);
  browser::InstallVirtualMemory(runtime, memory);
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x10f00;
  runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto put = [&](uint32_t address, uint32_t value) { memory.Write(ctx, address, &value, 4); };
    const auto get = [&](uint32_t address) {
      uint32_t value = 0;
      memory.Read(ctx, address, &value, 4);
      return value;
    };
    const auto allocate = [&](uint32_t bytes, uint32_t flags = 0x3000) {
      put(0x10100, 0);
      put(0x10104, bytes);
      return runtime.Call(ctx, base, runtime.KernelTarget(184), {0x10100, 0, 0x10104, flags, 4});
    };
    check(allocate(24u << 20) == 0, "mission allocation is still limited to the menu working set");
    const auto first = get(0x10100);
    put(first + (24u << 20) - 4, 0x12345678);
    check(allocate(40u << 20) == 0, "mission arena cannot contain its bounded 64 MiB working set");
    const auto second = get(0x10100);
    check(second >= first + (24u << 20), "mission virtual allocations overlap");
    put(second + (40u << 20) - 4, 0xabcdef01);
    check(allocate(4096) == 0xc0000017 && get(0x10100) == 0 && get(0x10104) == 4096,
      "mission arena exhaustion published invented backing or partial outputs");
    put(0x10100, first);
    put(0x10104, 0);
    check(runtime.Call(ctx, base, runtime.KernelTarget(199), {0x10100, 0x10104, 0x8000}) == 0,
      "mission reservation did not release");
    check(allocate(16u << 20, 0x103000) == 0, "mission top-down allocation did not reuse its released reservation");
    check(get(get(0x10100) + (16u << 20) - 4) == 0 && get(second + (40u << 20) - 4) == 0xabcdef01,
      "mission reuse retained stale data or changed another live allocation");
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  const auto backing = memory.BackingBytes();
  bool rejected = false;
  try { memory.Map(0x20000000, 64u << 20); }
  catch (const browser::Fault&) { rejected = true; }
  check(rejected && memory.BackingBytes() == backing, "mission backing budget became unbounded");
  std::puts("mission-memory-contracts-ok");
}
