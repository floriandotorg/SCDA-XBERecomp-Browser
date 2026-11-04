#include "cpu/x86_ops.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

const auto check = [](bool valid, const char* message) {
  if (!valid) { throw std::runtime_error(message); }
};

int main() {
  browser::Memory memory;
  memory.Ensure(0x10000, 8192);
  memory.Ensure(0x14000, 8192);
  std::array<uint8_t, 8192> initial{}, expected{}, actual{};
  for (uint32_t n = 0; n < initial.size(); ++n) { initial[n] = uint8_t(n * 173 + n / 7); }
  const auto execute = [&](X86Context& context, uint32_t width, bool fill, bool optimized) {
    if (optimized) {
      if (fill) { memory.StoreRepeated(context, width); }
      else { memory.MoveRepeated(context, width); }
    } else {
      if (fill) { x86_rep_stos(context, memory.Base(), width); }
      else { x86_rep_movs(context, memory.Base(), width); }
    }
  };
  const auto fault = [&](X86Context& context, uint32_t width, bool fill, bool optimized) {
    try { execute(context, width, fill, optimized); }
    catch (const browser::Fault& failure) { return std::optional<uint32_t>(failure.address); }
    return std::optional<uint32_t>();
  };
  const auto registers = [&](const X86Context& original, const X86Context& optimized, uint32_t delta) {
    check(original.esi + delta == optimized.esi && original.edi + delta == optimized.edi && original.ecx == optimized.ecx &&
      original.eax == optimized.eax && original.df == optimized.df && original.cf == optimized.cf && original.ebx == optimized.ebx,
      "repeated memory changed registers or completed count");
  };
  for (const bool fill : {false, true}) {
    for (const uint32_t width : {1u, 2u, 4u}) {
      for (const uint32_t direction : {0u, 1u}) {
        for (const int32_t delta : {-4090, -129, -7, -4, -1, 0, 1, 3, 4, 7, 129, 3000}) {
          for (const uint32_t count : {0u, 1u, 7u, 65u, 257u}) {
            if (direction && delta < -1000) { continue; }
            memory.Write({}, 0x10000, initial.data(), initial.size());
            memory.Write({}, 0x14000, initial.data(), initial.size());
            X86Context original{};
            original.esi = 0x10ffd; original.edi = uint32_t(int64_t(original.esi) + delta);
            original.ecx = count; original.eax = 0x8172fe43; original.df = direction;
            original.cf = 1; original.ebx = 0x11223344;
            auto optimized = original;
            optimized.esi += 0x4000; optimized.edi += 0x4000;
            const auto first = fault(original, width, fill, false), second = fault(optimized, width, fill, true);
            check(first.has_value() == second.has_value() && (!first || *first + 0x4000 == *second),
              "repeated memory changed fault address");
            registers(original, optimized, 0x4000);
            memory.Read({}, 0x10000, expected.data(), expected.size());
            memory.Read({}, 0x14000, actual.data(), actual.size());
            check(actual == expected, "repeated memory changed overlapping, unaligned or backward bytes");
          }
        }
      }
    }
  }
  for (const bool fill : {false, true}) {
    for (const bool sourceFault : {false, true}) {
      if (fill && sourceFault) { continue; }
      for (const uint32_t width : {1u, 2u, 4u}) {
        memory.Write({}, 0x10000, initial.data(), initial.size());
        memory.Write({}, 0x14000, initial.data(), initial.size());
        memory.Decommit(0x11000, 4096); memory.Decommit(0x15000, 4096);
        X86Context original{};
        original.esi = sourceFault ? 0x10ff0 : 0x10080;
        original.edi = sourceFault ? 0x10100 : 0x10ff0;
        original.ecx = 65; original.eax = 0xaabbccdd;
        auto optimized = original;
        optimized.esi += 0x4000; optimized.edi += 0x4000;
        const auto first = fault(original, width, fill, false), second = fault(optimized, width, fill, true);
        check(first && second && *first + 0x4000 == *second, "repeated memory lost partial page fault");
        registers(original, optimized, 0x4000);
        memory.Read({}, 0x10000, expected.data(), 4096);
        memory.Read({}, 0x14000, actual.data(), 4096);
        check(std::equal(expected.begin(), expected.begin() + 4096, actual.begin()), "repeated memory lost committed prefix at fault");
        memory.Commit(0x11000, 4096); memory.Commit(0x15000, 4096);
      }
    }
  }
  for (const uint32_t width : {1u, 2u, 4u}) {
    for (const uint32_t direction : {0u, 1u}) {
      for (const int32_t delta : {-129, -3, 0, 1, 7, 129}) {
        memory.Write({}, 0x80018000, initial.data(), initial.size());
        X86Context original{};
        original.esi = 0x80018ffd; original.edi = uint32_t(int64_t(0xf0018ffd) + delta);
        original.ecx = 257; original.df = direction;
        auto optimized = original;
        execute(original, width, false, false);
        memory.Read({}, 0x80018000, expected.data(), expected.size());
        memory.Write({}, 0x80018000, initial.data(), initial.size());
        const auto before = memory.PhysicalWriteVersion({}, 0x80018000, 8192);
        const auto neighbor = memory.PhysicalWriteVersion({}, 0x8001a000, 4096);
        execute(optimized, width, false, true);
        memory.Read({}, 0x80018000, actual.data(), actual.size());
        check(actual == expected, "repeated move missed interfering physical aliases");
        check(memory.PhysicalWriteVersion({}, 0xf0018000, 8192) > before &&
          memory.PhysicalWriteVersion({}, 0x8001a000, 4096) == neighbor, "repeated move lost physical write versions");
        registers(original, optimized, 0);
      }
    }
  }
  memory.Ensure(0xfffff000, 4096);
  const uint32_t tail = 0x12345678;
  memory.Write({}, 0xfffffffc, &tail, 4);
  for (const bool fill : {false, true}) {
    X86Context original{};
    original.esi = 0xfffffffc; original.edi = fill ? 0xfffffffc : 0x10000;
    original.ecx = 2; original.eax = tail;
    auto optimized = original;
    const auto first = fault(original, 4, fill, false), second = fault(optimized, 4, fill, true);
    check(first && second && *first == 0 && *second == 0, "repeated memory lost address wrap fault");
    registers(original, optimized, 0);
  }
  for (const bool fill : {false, true}) {
    X86Context context{};
    check(fault(context, 3, fill, true).has_value(), "repeated memory admitted invalid width with zero count");
    context.ecx = 1;
    context.esi = context.edi = 0x10000;
    {
      auto lease = memory.Borrow();
      check(fault(context, 4, fill, true).has_value() && context.ecx == 1, "repeated memory admitted an outer lease");
    }
    browser::Runtime runtime({}, {});
    runtime.Cancel(); context.host = &runtime;
    check(fault(context, 4, fill, true).has_value() && context.ecx == 1, "repeated memory ignored cancellation before its first store");
  }
  for (const bool fill : {false, true}) {
    browser::Runtime runtime({}, {});
    std::atomic<bool> entered = false, cancelled = false;
    X86Context stopped{};
    const auto version = memory.PhysicalWriteVersion({}, 0x80020000, 4096);
    runtime.Launch({}, memory.Base(), [&](X86Context& context, uint8_t*) {
      entered = true;
      try {
        for (;;) {
          context.esi = 0x80040000; context.edi = 0x80020000;
          context.ecx = 65536; context.eax = 0x1973abde;
          execute(context, 4, fill, true);
        }
      } catch (const browser::Cancelled&) {
        stopped = context;
        cancelled = true;
        throw;
      }
    });
    for (uint32_t n = 0; n < 5000 && (!entered || memory.PhysicalWriteVersion({}, 0x80020000, 4096) == version); ++n) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    runtime.Cancel();
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(cancelled && !runtime.ActiveWorkers() && stopped.ecx <= 65536 &&
      stopped.edi == 0x80020000 + (65536 - stopped.ecx) * 4 &&
      stopped.esi == 0x80040000 + (fill ? 0 : (65536 - stopped.ecx) * 4),
      "repeated memory lost cancellation progress or its worker");
    auto lease = memory.Borrow();
    check(memory.Load<uint32_t>({}, 0x80020000) != 0 || !fill, "cancelled fill retained no completed prefix");
  }
  std::puts("repeated-memory-contracts-ok");
}
