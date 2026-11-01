#include "browser/kernel-provider.h"
#include <array>
#include <cstdio>

using browser::Memory;
using browser::Runtime;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto fault = [](auto operation) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, "invalid pool request accepted");
};

int main() {
  for (uint32_t generation = 0; generation < 3; ++generation) {
    Runtime runtime({}, {});
    Memory memory;
    memory.Map(0x10000, 0x2000);
    memory.Commit(0x10000, 0x1000);
    browser::InstallKernelProvider(runtime, memory);
    for (const uint32_t ordinal : {15u, 17u, 23u}) {
      check(runtime.KernelTarget(ordinal), "pool provider target absent");
    }
    X86Context context{};
    context.host = &runtime;
    context.esp = 0x10000;
    context.ebx = 0x12345678;
    context.ecx = 0x87654321;
    context.cf = 1;
    const auto call = [&](uint32_t ordinal, uint32_t argument, uint32_t tag = 0) {
      auto request = context;
      const std::array<uint32_t, 3> frame{0xfffffff0, argument, tag};
      memory.Write({}, request.esp, frame.data(), frame.size() * 4);
      runtime.Invoke(request, memory.Base(), runtime.KernelTarget(ordinal));
      check(request.esp == context.esp + (ordinal == 15 ? 12 : 8), "pool stdcall cleanup incorrect");
      check(request.ebx == context.ebx && request.ecx == context.ecx && request.cf == 1, "pool clobbered unrelated state");
      std::array<uint32_t, 3> after;
      memory.Read({}, context.esp, after.data(), after.size() * 4);
      check(after == frame, "pool changed input frame");
      return request.eax;
    };
    check(call(23, 0xb0000000) == 0, "new session retained allocation metadata");
    const uint32_t first = call(15, 17, 0x41424344);
    const uint32_t second = call(15, 0, UINT32_MAX);
    check(first == 0xb0000000 && second == first + 32, "pool alignment or zero size policy incorrect");
    check(call(23, first) == 32 && call(23, second) == 16, "pool size did not return aligned block size");
    check(call(23, first + 1) == 0 && call(23, 0) == 0, "pool accepted interior or unknown query");
    std::array<uint8_t, 32> bytes{};
    memory.Read({}, first, bytes.data(), bytes.size());
    check(bytes == std::array<uint8_t, 32>{}, "pool did not zero new allocation");
    bytes.fill(0xa5);
    memory.Write({}, first, bytes.data(), bytes.size());
    fault([&] { call(17, first + 1); });
    check(call(23, first) == 32, "invalid free changed allocation");
    call(17, first);
    fault([&] { call(17, first); });
    fault([&] { call(17, 0); });
    check(call(23, first) == 0, "free retained allocation");
    check(call(15, 32) == first, "pool did not reuse free block");
    memory.Read({}, first, bytes.data(), bytes.size());
    check(bytes == std::array<uint8_t, 32>{}, "reused pool allocation exposed old bytes");
    call(17, first);
    call(17, second);
    constexpr uint32_t capacity = 8u << 20;
    const size_t backing = memory.BackingBytes();
    check(call(15, capacity) == first && call(23, first) == capacity, "pool did not coalesce free extents");
    check(call(15, 1) == 0 && call(15, UINT32_MAX) == 0 && call(15, capacity + 1) == 0, "pool exhaustion or overflow accepted");
    call(17, first);
    std::vector<uint32_t> allocations;
    for (uint32_t n = 0; n < 4096; ++n) { allocations.push_back(call(15, 1, n)); }
    check(allocations.back() && call(15, 1) == 0, "pool metadata bound not enforced");
    for (const uint32_t address : allocations) { call(17, address); }
    check(call(15, capacity) == first, "fragmented pool did not recover full capacity");
    call(17, first);
    const uint32_t left = call(15, capacity / 4);
    const uint32_t middle = call(15, capacity / 2);
    const uint32_t right = call(15, capacity / 4);
    call(17, left);
    call(17, right);
    check(call(15, capacity / 2) == 0 && call(23, middle) == capacity / 2, "pool ignored fragmentation or lost live allocation");
    call(17, middle);
    check(call(15, capacity) == first, "pool failed bidirectional coalescing");
    call(17, first);
    check(memory.BackingBytes() == backing, "pool grew backing during allocation");
    for (const uint32_t ordinal : {15u, 17u, 23u}) {
      for (const uint32_t address : {0x10ffcu, 0x11000u, 0xfffffffcu}) {
        auto invalid = context;
        invalid.esp = address;
        invalid.eax = 0xabcdef01;
        fault([&] { runtime.Invoke(invalid, memory.Base(), runtime.KernelTarget(ordinal)); });
        check(invalid.esp == address && invalid.eax == 0xabcdef01, "invalid pool stack published result");
      }
      auto foreign = context;
      Runtime other({}, {});
      foreign.host = &other;
      fault([&] { runtime.Invoke(foreign, memory.Base(), runtime.KernelTarget(ordinal)); });
      auto wrongBase = context;
      fault([&] { runtime.Invoke(wrongBase, nullptr, runtime.KernelTarget(ordinal)); });
      check(wrongBase.esp == context.esp, "foreign memory base changed stack");
      auto boundary = context;
      const uint32_t frameBytes = ordinal == 15 ? 12 : 8;
      boundary.esp = 0x11000 - frameBytes;
      const uint32_t block = ordinal == 17 ? call(15, 16) : 0;
      const std::array<uint32_t, 3> frame{0xfffffff0, block, UINT32_MAX};
      memory.Write({}, boundary.esp, frame.data(), frameBytes);
      runtime.Invoke(boundary, memory.Base(), runtime.KernelTarget(ordinal));
      check(boundary.esp == 0x11000, "pool rejected valid boundary frame");
      if (ordinal == 15) { call(17, boundary.eax); }
      {
        auto lease = memory.Borrow();
        fault([&] { call(ordinal, 0); });
      }
    }
    const auto exercise = [&](uint32_t stack) {
      auto request = context;
      request.esp = stack;
      for (uint32_t n = 0; n < 100; ++n) {
        const uint32_t block = runtime.Call(request, memory.Base(), runtime.KernelTarget(15), {48, n});
        check(block && runtime.Call(request, memory.Base(), runtime.KernelTarget(23), {block}) == 48, "concurrent pool allocation lost");
        runtime.Call(request, memory.Base(), runtime.KernelTarget(17), {block});
      }
    };
    std::thread worker([&] { exercise(0x10400); });
    exercise(0x10800);
    worker.join();
    check(call(15, capacity) == first, "concurrent pool leaked blocks");
    call(17, first);
    check(call(15, 16) == first, "pool did not recover after concurrent use");
    const std::array<uint32_t, 3> retiredFrame{0xfffffff0, first, 0};
    memory.Write({}, context.esp, retiredFrame.data(), retiredFrame.size() * 4);
    runtime.Cancel();
    for (const uint32_t ordinal : {15u, 17u, 23u}) {
      auto retired = context;
      retired.eax = 0xabcdef01;
      fault([&] { runtime.Invoke(retired, memory.Base(), runtime.KernelTarget(ordinal)); });
      check(retired.esp == context.esp && retired.eax == 0xabcdef01, "retired pool published result");
    }
  }
  std::puts("kernel-pool-contracts-ok");
}
