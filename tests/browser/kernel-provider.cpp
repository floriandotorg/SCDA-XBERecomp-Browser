#include "browser/kernel-provider.h"
#include <array>
#include <cstdio>

using browser::Memory;
using browser::Runtime;
using namespace std::chrono_literals;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto word = [](Memory& memory, uint32_t address) {
  uint32_t value;
  memory.Read({}, address, &value, 4);
  return value;
};
const auto put = [](Memory& memory, uint32_t address, uint32_t value) { memory.Write({}, address, &value, 4); };
const auto fault = [](auto operation) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, "invalid service request accepted");
};

const auto performanceCounterContracts = [] {
  for (uint32_t n = 0; n < 3; ++n) {
    std::atomic<uint64_t> source = 900000;
    Runtime runtime({}, {}, [&] { return source.load(); });
    Memory memory;
    memory.Map(0x10000, 0x1000);
    memory.Commit(0x10000, 0x1000);
    browser::InstallKernelProvider(runtime, memory);
    check(runtime.KernelTarget(126), "performance counter target absent");
    X86Context context{};
    context.host = &runtime;
    context.fs_base = 0x10000;
    context.esp = 0x10800;
    put(memory, context.esp, 0x12345678);
    fault([&] { runtime.Kernel(context, memory.Base(), 126); });
    runtime.Launch(context, memory.Base(), [&](X86Context& worker, uint8_t* base) {
      const auto query = [&](uint64_t elapsed) {
        source = 900000 + elapsed;
        auto request = worker;
        request.eax = 0xabcdef01;
        request.edx = 0x87654321;
        request.ecx = 0x12345678;
        request.ebx = 0x11223344;
        request.esi = 0x55667788;
        request.edi = 0x99aabbcc;
        request.ebp = 0xddeeff00;
        request.cf = 1;
        std::array<uint8_t, 0x1000> before{}, after{};
        memory.Read({}, 0x10000, before.data(), before.size());
        runtime.Kernel(request, base, 126);
        memory.Read({}, 0x10000, after.data(), after.size());
        const uint64_t expected = elapsed / 80 * 27 + elapsed % 80 * 27 / 80;
        check((uint64_t(request.edx) << 32 | request.eax) == expected, "performance counter scaling or return registers incorrect");
        check(request.esp == worker.esp + 4 && request.ecx == 0x12345678 && request.ebx == 0x11223344 &&
          request.esi == 0x55667788 && request.edi == 0x99aabbcc && request.ebp == 0xddeeff00 &&
          request.fs_base == worker.fs_base && request.cf == 1 && before == after, "performance counter changed unrelated state");
        check(runtime.TimerTime(worker) == elapsed, "performance counter did not share timer epoch");
      };
      query(0);
      query(1);
      query(79);
      query(80);
      query(12725829000ull);
      query(UINT64_MAX - 900000);
      query(UINT64_MAX - 900000);
      for (const uint32_t address : {0x10ffeu, 0x11000u, 0xfffffffcu}) {
        auto invalid = worker;
        invalid.esp = address;
        invalid.eax = 0xabcdef01;
        invalid.edx = 0x87654321;
        fault([&] { runtime.Kernel(invalid, base, 126); });
        check(invalid.esp == address && invalid.eax == 0xabcdef01 && invalid.edx == 0x87654321, "invalid counter frame published state");
      }
      auto foreign = worker;
      ++foreign.fs_base;
      fault([&] { runtime.Kernel(foreign, base, 126); });
      fault([&] { runtime.Kernel(worker, nullptr, 126); });
      {
        auto lease = memory.Borrow();
        fault([&] { runtime.Kernel(worker, base, 126); });
      }
      auto cancelled = worker;
      cancelled.eax = 0xabcdef01;
      cancelled.edx = 0x87654321;
      runtime.Cancel();
      fault([&] { runtime.Kernel(cancelled, base, 126); });
      check(cancelled.esp == worker.esp && cancelled.eax == 0xabcdef01 && cancelled.edx == 0x87654321, "cancelled counter published state");
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
};

void contracts() {
  performanceCounterContracts();
  Runtime runtime({}, {});
  Memory memory;
  memory.Map(0x10000, 0x6000);
  memory.Commit(0x10000, 0x6000);
  browser::InstallKernelProvider(runtime, memory);
  check(runtime.KernelTarget(277) && runtime.KernelTarget(291) && runtime.KernelTarget(294), "provider targets absent");
  check(runtime.KernelTarget(161), "IRQL provider target absent");
  check(runtime.KernelTarget(129), "DPC IRQL provider target absent");
  check(runtime.KernelTarget(252), "offline link provider target absent");
  check(runtime.KernelTarget(253), "offline PHY initialization target absent");
  check(runtime.KernelTarget(160), "KfRaiseIrql provider target absent");
  constexpr uint32_t cs = 0x12000;
  X86Context first{}, second{};
  first.host = &runtime;
  first.fs_base = 0x10000;
  first.esp = 0x14000;
  first.ebx = 0x12345678;
  second.host = &runtime;
  second.fs_base = 0x11000;
  second.esp = 0x15000;
  put(memory, first.fs_base + 0x28, 0x10080);
  put(memory, second.fs_base + 0x28, 0x11080);
  const auto invoke = [&](X86Context& ctx, uint32_t ordinal, uint32_t address = cs) {
    const uint32_t stack = ctx.esp;
    check(runtime.Call(ctx, memory.Base(), runtime.KernelTarget(ordinal), {address}) == 0, "service did not return success");
    check(ctx.esp == stack, "stdcall stack not restored");
  };
  check(runtime.KernelTarget(128), "system time provider target absent");
  const auto systemTime = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(128)); };
  fault([&] { systemTime(first); });
  runtime.Launch(first, memory.Base(), [&](X86Context& worker, uint8_t*) {
  auto timeRequest = worker;
  timeRequest.eax = 0xabcdef01;
  timeRequest.ecx = 0x12345678;
  timeRequest.edx = 0x87654321;
  timeRequest.esi = 0x11223344;
  timeRequest.edi = 0x55667788;
  timeRequest.ebp = 0x99aabbcc;
  timeRequest.cf = 1;
  put(memory, timeRequest.esp, 0xfffffff0);
  put(memory, timeRequest.esp + 4, 0x13004);
  std::vector<uint8_t> timeBefore(0x6000), timeAfter(0x6000);
  memory.Read({}, 0x10000, timeBefore.data(), timeBefore.size());
  const uint64_t earliest = Runtime::SystemTime();
  systemTime(timeRequest);
  const uint64_t latest = Runtime::SystemTime();
  uint64_t result;
  memory.Read({}, 0x13004, &result, sizeof(result));
  memory.Read({}, 0x10000, timeAfter.data(), timeAfter.size());
  std::memcpy(timeBefore.data() + 0x3004, &result, sizeof(result));
  check(result >= earliest && result <= latest, "system time outside wall clock bounds");
  check(timeBefore == timeAfter, "system time changed bytes outside output");
  check(timeRequest.esp == first.esp + 8 && timeRequest.eax == 0xabcdef01 &&
    timeRequest.ecx == 0x12345678 && timeRequest.edx == 0x87654321 && timeRequest.ebx == first.ebx &&
    timeRequest.esi == 0x11223344 && timeRequest.edi == 0x55667788 && timeRequest.ebp == 0x99aabbcc &&
    timeRequest.fs_base == first.fs_base && timeRequest.cf == 1, "system time clobbered state");
  memory.Map(0x19000, 0x1000);
  for (const uint32_t address : {0u, 0x15ffcu, 0x16000u, 0x19000u, 0xfffffffcu}) {
    auto invalid = first;
    invalid.eax = 0xabcdef01;
    put(memory, invalid.esp + 4, address);
    memory.Read({}, 0x10000, timeBefore.data(), timeBefore.size());
    fault([&] { systemTime(invalid); });
    memory.Read({}, 0x10000, timeAfter.data(), timeAfter.size());
    check(invalid.esp == first.esp && invalid.eax == 0xabcdef01 && timeBefore == timeAfter, "invalid time output published state");
  }
  for (const uint32_t address : {0x15ffcu, 0x16000u, 0x19000u, 0xfffffffcu}) {
    auto invalid = first;
    invalid.esp = address;
    invalid.eax = 0xabcdef01;
    fault([&] { systemTime(invalid); });
    check(invalid.esp == address && invalid.eax == 0xabcdef01, "invalid time frame published state");
  }
  Runtime foreignTime({}, {});
  auto foreignTimeContext = first;
  foreignTimeContext.host = &foreignTime;
  fault([&] { systemTime(foreignTimeContext); });
  fault([&] { runtime.Kernel(first, nullptr, 128); });
  auto overlap = worker;
  put(memory, overlap.esp, 0xfffffff0);
  put(memory, overlap.esp + 4, overlap.esp);
  const uint64_t overlapStart = Runtime::SystemTime();
  systemTime(overlap);
  const uint64_t overlapEnd = Runtime::SystemTime();
  memory.Read({}, worker.esp, &result, sizeof(result));
  check(result >= overlapStart && result <= overlapEnd && overlap.esp == worker.esp + 8, "overlapping time frame was not snapshotted");
  auto foreignKpcr = worker;
  ++foreignKpcr.fs_base;
  fault([&] { systemTime(foreignKpcr); });
  {
    auto lease = memory.Borrow();
    fault([&] { systemTime(worker); });
  }
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  {
    Runtime cancelledRuntime({}, {});
    Memory cancelledMemory;
    cancelledMemory.Map(0x10000, 0x1000);
    cancelledMemory.Commit(0x10000, 0x1000);
    browser::InstallKernelProvider(cancelledRuntime, cancelledMemory);
    X86Context cancelledContext{};
    cancelledContext.host = &cancelledRuntime;
    cancelledContext.esp = 0x10000;
    put(cancelledMemory, 0x10004, 0x10100);
    cancelledRuntime.Launch(cancelledContext, cancelledMemory.Base(), [&](X86Context& ctx, uint8_t* base) {
      std::array<uint8_t, 8> before{}, after{};
      cancelledMemory.Read(ctx, 0x10100, before.data(), before.size());
      cancelledRuntime.Cancel();
      fault([&] { cancelledRuntime.Kernel(ctx, base, 128); });
      cancelledMemory.Read(ctx, 0x10100, after.data(), after.size());
      check(before == after && ctx.esp == 0x10000, "cancelled system time published state");
    });
    if (const auto failure = cancelledRuntime.Join()) { std::rethrow_exception(failure); }
  }
  const auto link = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(252)); };
  for (const uint32_t argument : {0u, 1u, UINT32_MAX}) {
    auto query = first;
    query.eax = UINT32_MAX;
    query.ecx = 0x12345678;
    query.edx = 0x87654321;
    query.cf = 1;
    put(memory, query.esp, 0xfffffff0);
    put(memory, query.esp + 4, argument);
    link(query);
    check(query.eax == 0 && query.esp == first.esp + 8, "offline link did not return disconnected with one argument cleanup");
    check(query.ecx == 0x12345678 && query.edx == 0x87654321 && query.ebx == first.ebx && query.cf == 1, "offline link clobbered unrelated state");
    check(word(memory, first.esp) == 0xfffffff0 && word(memory, first.esp + 4) == argument, "offline link changed its input frame");
  }
  for (const uint32_t address : {0x16000u, 0x15ffcu, 0xfffffff8u, 0xfffffffcu}) {
    auto invalid = first;
    invalid.esp = address;
    invalid.eax = 0xabcdef01;
    fault([&] { link(invalid); });
    check(invalid.esp == address && invalid.eax == 0xabcdef01, "invalid link stack published a result");
  }
  memory.Map(0x18000, 0x1000);
  auto uncommittedLink = first;
  uncommittedLink.esp = 0x18000;
  fault([&] { link(uncommittedLink); });
  auto boundaryLink = first;
  boundaryLink.esp = 0x15ff8;
  put(memory, boundaryLink.esp, 0xfffffff0);
  put(memory, boundaryLink.esp + 4, 0);
  link(boundaryLink);
  check(boundaryLink.eax == 0 && boundaryLink.esp == 0x16000, "link frame ending at mapping boundary rejected");
  const auto initializePhy = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(253)); };
  for (const uint32_t reset : {0u, 1u, 0x100u, UINT32_MAX}) {
    for (const uint32_t parameter : {0u, cs, 0x18000u, UINT32_MAX}) {
      auto request = first;
      request.eax = 0xabcdef01;
      request.ecx = 0x12345678;
      request.edx = 0x87654321;
      request.esi = 0x11223344;
      request.edi = 0x55667788;
      request.ebp = 0x99aabbcc;
      request.cf = 1;
      put(memory, request.esp, 0xfffffff0);
      put(memory, request.esp + 4, reset);
      put(memory, request.esp + 8, parameter);
      std::vector<uint8_t> before(0x6000), after(0x6000);
      memory.Read({}, 0x10000, before.data(), before.size());
      initializePhy(request);
      memory.Read({}, 0x10000, after.data(), after.size());
      check(request.eax == 0x801f0001 && request.esp == first.esp + 12, "offline PHY did not return hardware unavailable with two argument cleanup");
      check(request.ecx == 0x12345678 && request.edx == 0x87654321 && request.ebx == first.ebx &&
        request.esi == 0x11223344 && request.edi == 0x55667788 && request.ebp == 0x99aabbcc &&
        request.fs_base == first.fs_base && request.cf == 1, "offline PHY clobbered unrelated state");
      check(before == after, "offline PHY changed guest memory");
      auto query = first;
      link(query);
      check(query.eax == 0, "failed PHY initialization changed offline link state");
    }
  }
  for (const uint32_t address : {0x16000u, 0x15ff8u, 0x15ffcu, 0x18000u, 0xfffffff4u, 0xfffffffcu}) {
    auto invalid = first;
    invalid.esp = address;
    invalid.eax = 0xabcdef01;
    fault([&] { initializePhy(invalid); });
    check(invalid.esp == address && invalid.eax == 0xabcdef01, "invalid PHY stack published a result");
  }
  auto boundaryPhy = first;
  boundaryPhy.esp = 0x15ff4;
  put(memory, boundaryPhy.esp, 0xfffffff0);
  put(memory, boundaryPhy.esp + 4, 1);
  put(memory, boundaryPhy.esp + 8, 0);
  initializePhy(boundaryPhy);
  check(boundaryPhy.eax == 0x801f0001 && boundaryPhy.esp == 0x16000, "PHY frame ending at mapping boundary rejected");
  const auto lower = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(161)); };
  const auto raise = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(160)); };
  const auto irql = [&](const X86Context& ctx) {
    uint8_t value;
    memory.Read(ctx, ctx.fs_base + 0x24, &value, 1);
    return value;
  };
  const auto raiseDpc = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(129)); };
  for (uint32_t n = 0; n <= 2; ++n) {
    auto request = first;
    request.eax = 0xabcdef01;
    request.ecx = 0xaabbccff;
    request.edx = 0x87654321;
    request.esi = 0x11223344;
    request.edi = 0x55667788;
    request.ebp = 0x99aabbcc;
    request.cf = 1;
    put(memory, first.fs_base + 0x24, 0xaabbcc00 + n);
    put(memory, second.fs_base + 0x24, 0x11223307);
    put(memory, request.esp, 0xfffffff0);
    put(memory, request.esp + 4, 0xffffffff);
    std::vector<uint8_t> before(0x6000), after(0x6000);
    memory.Read({}, 0x10000, before.data(), before.size());
    raiseDpc(request);
    memory.Read({}, 0x10000, after.data(), after.size());
    before[first.fs_base + 0x24 - 0x10000] = 2;
    check(request.eax == n && request.esp == first.esp + 4, "DPC raise lost previous level or zero argument cleanup");
    check(request.ecx == 0xaabbccff && request.edx == 0x87654321 && request.ebx == first.ebx &&
      request.esi == 0x11223344 && request.edi == 0x55667788 && request.ebp == 0x99aabbcc &&
      request.fs_base == first.fs_base && request.cf == 1, "DPC raise clobbered unrelated state");
    check(before == after && irql(second) == 7, "DPC raise changed bytes outside current IRQL");
    request.ecx = n;
    lower(request);
    check(irql(first) == n && request.esp == first.esp + 8, "DPC raise could not restore previous level");
  }
  for (const uint32_t level : {3u, 31u, 32u, 255u}) {
    auto invalid = first;
    invalid.eax = 0xabcdef01;
    put(memory, first.fs_base + 0x24, 0xaabbcc00 + level);
    fault([&] { raiseDpc(invalid); });
    check(invalid.eax == 0xabcdef01 && invalid.esp == first.esp &&
      word(memory, first.fs_base + 0x24) == 0xaabbcc00 + level, "invalid DPC raise published state");
  }
  put(memory, first.fs_base + 0x24, 0xaabbcc00);
  for (const uint32_t address : {0u, 0x10001u, 0x16000u, 0x18000u, 0xfffffffcu}) {
    auto invalid = first;
    invalid.fs_base = address;
    invalid.eax = 0xabcdef01;
    fault([&] { raiseDpc(invalid); });
    check(invalid.esp == first.esp && invalid.eax == 0xabcdef01 && irql(first) == 0, "invalid DPC KPCR published state");
  }
  for (const uint32_t address : {0x16000u, 0x15ffeu, 0x18000u, 0xfffffffcu}) {
    auto invalid = first;
    invalid.esp = address;
    invalid.eax = 0xabcdef01;
    fault([&] { raiseDpc(invalid); });
    check(invalid.esp == address && invalid.eax == 0xabcdef01 && irql(first) == 0, "invalid DPC stack published state");
  }
  auto boundaryDpc = first;
  boundaryDpc.esp = 0x15ffc;
  put(memory, boundaryDpc.esp, 0xfffffff0);
  raiseDpc(boundaryDpc);
  check(boundaryDpc.eax == 0 && boundaryDpc.esp == 0x16000 && irql(first) == 2, "DPC return frame at mapping boundary rejected");
  put(memory, first.fs_base + 0x24, 0xaabbcc1f);
  put(memory, second.fs_base + 0x24, 0x11223307);
  put(memory, first.esp, 0xfffffff0);
  put(memory, first.esp + 4, 31);
  first.ecx = 0xaabbcc02;
  first.edx = 0x87654321;
  first.eax = UINT32_MAX;
  first.cf = 1;
  lower(first);
  check(first.esp == 0x14004 && first.eax == 0, "fastcall return did not pop exactly one return address");
  check(first.ecx == 0xaabbcc02 && first.edx == 0x87654321 && first.ebx == 0x12345678 && first.cf == 1, "fastcall clobbered unrelated state");
  check(word(memory, first.fs_base + 0x24) == 0xaabbcc02 && irql(second) == 7, "IRQL write escaped current KPCR byte");
  first.esp = 0x14000;
  lower(first);
  check(irql(first) == 2, "same level lowering rejected");
  first.esp = 0x14000;
  first.ecx = 0;
  lower(first);
  check(irql(first) == 0, "passive level lowering failed");
  first.esp = 0x14000;
  first.ecx = 1;
  fault([&] { lower(first); });
  check(first.esp == 0x14000 && irql(first) == 0, "invalid raising changed guest state");
  first.ecx = 0;
  put(memory, first.fs_base + 0x24, 32);
  fault([&] { lower(first); });
  check(irql(first) == 32 && first.esp == 0x14000, "invalid current IRQL changed state");
  put(memory, first.fs_base + 0x24, 2);
  for (const uint32_t address : {0u, 0x10001u, 0x16000u, 0xfffffffcu}) {
    auto invalid = first;
    invalid.fs_base = address;
    fault([&] { lower(invalid); });
    check(invalid.esp == first.esp && irql(first) == 2, "invalid KPCR changed state");
  }
  for (const uint32_t address : {0x16000u, 0x15ffeu, 0xfffffffcu}) {
    auto invalid = first;
    invalid.esp = address;
    fault([&] { lower(invalid); });
    check(invalid.esp == address && irql(first) == 2, "invalid fastcall stack changed state");
  }
  for (uint32_t level = 0; level <= 31; ++level) {
    for (uint32_t target = level; target <= 31; ++target) {
      auto request = first;
      request.eax = 0xabcdef01;
      request.ecx = 0xaabbcc00 + target;
      request.edx = 0x87654321;
      request.esi = 0x11223344;
      request.edi = 0x55667788;
      request.ebp = 0x99aabbcc;
      request.cf = 1;
      put(memory, first.fs_base + 0x24, 0xaabbcc00 + level);
      put(memory, second.fs_base + 0x24, 0x11223307);
      put(memory, request.esp, 0xfffffff0);
      std::vector<uint8_t> before(0x6000), after(0x6000);
      memory.Read({}, 0x10000, before.data(), before.size());
      raise(request);
      memory.Read({}, 0x10000, after.data(), after.size());
      before[first.fs_base + 0x24 - 0x10000] = uint8_t(target);
      check(request.eax == level && request.esp == first.esp + 4, "KfRaiseIrql lost previous level or exact one word cleanup");
      check(request.ecx == 0xaabbcc00 + target && request.edx == 0x87654321 && request.ebx == first.ebx &&
        request.esi == 0x11223344 && request.edi == 0x55667788 && request.ebp == 0x99aabbcc &&
        request.fs_base == first.fs_base && request.cf == 1, "KfRaiseIrql clobbered unrelated state");
      check(before == after && irql(second) == 7, "KfRaiseIrql changed bytes outside current KPCR IRQL");
      request.esp = first.esp;
      request.ecx = level;
      lower(request);
      check(irql(first) == level, "KfRaiseIrql result could not restore previous level");
    }
  }
  put(memory, first.fs_base + 0x24, 0xaabbcc02);
  for (const uint32_t argument : {0u, 1u, 32u, 255u, 0xaabbcc20u, 0xd203f1c2u}) {
    auto invalid = first;
    invalid.eax = 0xabcdef01;
    invalid.ecx = argument;
    put(memory, invalid.esp, 0xfffffff0);
    fault([&] { raise(invalid); });
    check(invalid.eax == 0xabcdef01 && invalid.ecx == argument && invalid.esp == first.esp &&
      word(memory, first.fs_base + 0x24) == 0xaabbcc02, "invalid KfRaiseIrql published state");
  }
  put(memory, first.fs_base + 0x24, 32);
  first.ecx = 31;
  fault([&] { raise(first); });
  check(irql(first) == 32 && first.esp == 0x14000, "invalid current IRQL changed KfRaiseIrql state");
  put(memory, first.fs_base + 0x24, 2);
  for (const uint32_t address : {0u, 0x10001u, 0x16000u, 0xfffffffcu}) {
    auto invalid = first;
    invalid.fs_base = address;
    invalid.ecx = 3;
    fault([&] { raise(invalid); });
    check(invalid.esp == first.esp && irql(first) == 2, "invalid KPCR changed KfRaiseIrql state");
  }
  for (const uint32_t address : {0x16000u, 0x15ffeu, 0xfffffffcu}) {
    auto invalid = first;
    invalid.esp = address;
    invalid.ecx = 3;
    fault([&] { raise(invalid); });
    check(invalid.esp == address && irql(first) == 2, "invalid KfRaiseIrql stack changed state");
  }
  auto boundaryRaise = first;
  boundaryRaise.esp = 0x15ffc;
  boundaryRaise.ecx = 0xffffff03;
  put(memory, boundaryRaise.esp, 0xfffffff0);
  raise(boundaryRaise);
  check(boundaryRaise.eax == 2 && boundaryRaise.esp == 0x16000 && irql(first) == 3, "KfRaiseIrql return frame at mapping boundary rejected");
  put(memory, first.fs_base + 0x24, 0xaabbcc00);
  first.esp = 0x14000;
  first.ecx = 2;
  raise(first);
  check(first.eax == 0 && irql(first) == 2, "raise to DPC level failed");
  first.esp = 0x14000;
  raiseDpc(first);
  check(first.eax == 2 && irql(first) == 2, "DPC raise after KfRaiseIrql changed level");
  first.esp = 0x14000;
  first.ecx = 0;
  lower(first);
  check(irql(first) == 0, "lower after KfRaiseIrql failed");
  first.esp = 0x14000;
  put(memory, first.fs_base + 0x24, 2);
  memory.Map(0x20000, 0x1000);
  auto uncommitted = first;
  uncommitted.fs_base = 0x20000;
  fault([&] { lower(uncommitted); });
  fault([&] { raise(uncommitted); });
  Runtime foreign({}, {});
  auto foreignContext = first;
  foreignContext.host = &foreign;
  fault([&] { lower(foreignContext); });
  fault([&] { runtime.Kernel(first, nullptr, 161); });
  fault([&] { raise(foreignContext); });
  fault([&] { runtime.Kernel(first, nullptr, 160); });
  fault([&] { raiseDpc(foreignContext); });
  fault([&] { runtime.Kernel(first, nullptr, 129); });
  fault([&] { link(foreignContext); });
  fault([&] { runtime.Kernel(first, nullptr, 252); });
  fault([&] { initializePhy(foreignContext); });
  fault([&] { runtime.Kernel(first, nullptr, 253); });
  check(irql(first) == 2 && first.esp == 0x14000, "foreign session changed IRQL");
  invoke(first, 291);
  check(word(memory, cs) == 0x40001 && word(memory, cs + 8) == cs + 8 && word(memory, cs + 12) == cs + 8, "critical section header");
  check(word(memory, cs + 16) == UINT32_MAX && !word(memory, cs + 20) && !word(memory, cs + 24), "initial critical state");
  invoke(first, 277);
  invoke(first, 277);
  check(word(memory, cs + 16) == 1 && word(memory, cs + 20) == 2 && word(memory, cs + 24) == 0x10080, "recursive owner is ETHREAD");
  fault([&] { invoke(second, 294); });
  fault([&] { invoke(first, 291); });
  check(word(memory, cs + 20) == 2, "rejected mutation changed recursion");
  const auto recursiveSequence = runtime.Sequence();
  invoke(first, 294);
  check(runtime.Sequence() == recursiveSequence, "recursive critical section release woke unrelated workers");
  std::atomic<bool> acquired = false;
  std::exception_ptr failure;
  std::thread waiter([&] {
    try {
      invoke(second, 277);
      acquired.store(true);
      invoke(second, 294);
    } catch (...) { failure = std::current_exception(); }
  });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (word(memory, cs + 16) != 1 && std::chrono::steady_clock::now() < deadline) { std::this_thread::sleep_for(1ms); }
  const bool reserved = word(memory, cs + 16) == 1;
  const bool blocked = !acquired.load();
  const auto contendedSequence = runtime.Sequence();
  invoke(first, 294);
  waiter.join();
  if (failure) { std::rethrow_exception(failure); }
  check(reserved && blocked && acquired.load(), "contended enter did not wait for release");
  check(runtime.Sequence() == contendedSequence + 1, "critical section handoff must wake its real waiter exactly once");
  check(word(memory, cs + 16) == UINT32_MAX && !word(memory, cs + 20) && !word(memory, cs + 24), "handoff did not restore unlocked state");
  const auto idleSequence = runtime.Sequence();
  for (size_t n = 0; n < 100; ++n) { invoke(first, 277); invoke(first, 294); }
  check(runtime.Sequence() == idleSequence, "uncontended critical section releases woke unrelated workers");
  fault([&] { invoke(first, 294); });
  for (const uint32_t address : {0u, 0x12001u, 0x15ff0u, 0xfffffff0u}) {
    first.esp = 0x14000;
    fault([&] { invoke(first, 277, address); });
  }
  first.esp = 0x14000;
  auto wrong = first;
  Runtime other({}, {});
  wrong.host = &other;
  fault([&] { invoke(wrong, 277); });
  fault([&] { runtime.Kernel(first, nullptr, 277); });
  first.esp = 0xfffffffc;
  fault([&] { runtime.Kernel(first, memory.Base(), 277); });
  first.esp = 0x14000;
  put(memory, cs + 16, INT32_MAX);
  put(memory, cs + 20, INT32_MAX);
  put(memory, cs + 24, 0x10080);
  fault([&] { invoke(first, 277); });
  check(word(memory, cs + 16) == INT32_MAX && word(memory, cs + 20) == INT32_MAX, "overflow changed guest state");
  put(memory, cs + 16, UINT32_MAX);
  put(memory, cs + 20, 0);
  put(memory, cs + 24, 0);
  first.esp = 0x14000;
  {
    auto lease = memory.Borrow();
    fault([&] { invoke(first, 277); });
  }
  check(word(memory, cs + 16) == UINT32_MAX, "inherited lease changed critical state");
  first.esp = 0x14000;
  invoke(first, 277);
  std::atomic<bool> cancelled = false;
  second.esp = 0x15000;
  std::thread retired([&] {
    try { invoke(second, 277); } catch (const browser::Cancelled&) { cancelled.store(true); }
  });
  const auto retirementDeadline = std::chrono::steady_clock::now() + 2s;
  while (word(memory, cs + 16) != 1 && std::chrono::steady_clock::now() < retirementDeadline) { std::this_thread::sleep_for(1ms); }
  runtime.Stop();
  auto retiredLink = first;
  retiredLink.eax = 0xabcdef01;
  fault([&] { link(retiredLink); });
  check(retiredLink.esp == first.esp && retiredLink.eax == 0xabcdef01, "retired runtime published link state");
  auto retiredPhy = first;
  retiredPhy.eax = 0xabcdef01;
  fault([&] { initializePhy(retiredPhy); });
  check(retiredPhy.esp == first.esp && retiredPhy.eax == 0xabcdef01, "retired runtime published PHY initialization status");
  auto retiredDpc = first;
  retiredDpc.eax = 0xabcdef01;
  fault([&] { raiseDpc(retiredDpc); });
  check(retiredDpc.esp == first.esp && retiredDpc.eax == 0xabcdef01 && irql(first) == 2, "retired runtime published DPC IRQL");
  auto retiredRaise = first;
  retiredRaise.eax = 0xabcdef01;
  retiredRaise.ecx = 3;
  fault([&] { raise(retiredRaise); });
  check(retiredRaise.esp == first.esp && retiredRaise.eax == 0xabcdef01 && irql(first) == 2, "retired runtime published raised IRQL");
  first.ecx = 0;
  fault([&] { lower(first); });
  check(irql(first) == 2 && first.esp == 0x14000, "retired runtime changed IRQL");
  retired.join();
  check(cancelled.load() && word(memory, cs + 16) == 0 && word(memory, cs + 24) == 0x10080, "cancellation failed to retire waiter reservation");
  std::puts("kernel-provider-contracts-ok");
}

int main() {
  try { contracts(); }
  catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
