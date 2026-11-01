#include "browser/cpu_runtime.h"
#include <atomic>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

struct ServiceFault : browser::Fault {
  using browser::Fault::Fault;
};

int main() {
  using Phase = browser::Runtime::KernelTracePhase;
  browser::Memory memory;
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime runtime({}, {});
    check(runtime.SnapshotKernelTrace().count == 0, "fresh runtime retained another session trace");
    X86Context context{};
    context.host = &runtime;
    context.fs_base = 0x12000;
    context.eax = 0x101;
    context.esp = 0x8000;
    context.last_icall = 0x401000;
    runtime.RegisterKernel(10, [&](X86Context& ctx, uint8_t* base) {
      ctx.eax = 0x202;
      ctx.esp += 4;
      runtime.Kernel(ctx, base, 11);
      ctx.eax = 0x404;
      ctx.esp += 8;
    });
    runtime.RegisterKernel(11, [](X86Context& ctx, uint8_t*) {
      ctx.eax = 0x303;
      ctx.esp += 4;
    });
    runtime.Kernel(context, memory.Base(), 10);
    const auto nested = runtime.SnapshotKernelTrace();
    check(nested.count == 4 && nested.total == 4, "nested trace event count changed");
    const uint32_t ordinals[]{10, 11, 11, 10};
    const uint32_t values[]{0x101, 0x202, 0x303, 0x404};
    const uint32_t stacks[]{0x8000, 0x8004, 0x8008, 0x8010};
    for (size_t n = 0; n < nested.count; ++n) {
      const auto& event = nested.events[n];
      check(event.sequence == n + 1 && event.ordinal == ordinals[n], "nested trace ordering changed");
      check(event.phase == (n < 2 ? Phase::Before : Phase::After), "nested trace phase changed");
      check(event.eax == values[n] && event.esp == stacks[n], "trace fabricated service registers");
      check(event.fsBase == 0x12000 && event.workerId == 0 && event.lastIcall == 0x401000, "host context attribution changed");
    }
    check(nested.events[0].callId == nested.events[3].callId && nested.events[1].callId == nested.events[2].callId && nested.events[0].callId != nested.events[1].callId, "nested service pairing lost");
    check(context.eax == 0x404 && context.esp == 0x8010, "trace changed true service return");
    for (uint32_t n = 0; n < 40; ++n) { runtime.Kernel(context, memory.Base(), 11); }
    const auto rotated = runtime.SnapshotKernelTrace();
    check(rotated.events.size() == 64 && rotated.count == 64 && rotated.total == 84, "trace did not retain its fixed bound");
    for (size_t n = 0; n < rotated.count; ++n) {
      check(rotated.events[n].sequence == 21 + n, "rotation did not preserve chronological sequence");
      check(rotated.events[n].phase == (n % 2 ? Phase::After : Phase::Before), "rotation changed phase ordering");
      check(rotated.events[n].callId == 21 + (n / 2) * 2, "rotation lost call pairing");
    }
    check(nested.events[0].eax == 0x101 && nested.total == 4, "snapshot aliased mutable ring storage");
  }
  {
    browser::Runtime runtime({}, {});
    X86Context context{};
    context.host = &runtime;
    context.fs_base = 0x22000;
    context.eax = 0x11;
    context.esp = 0x9000;
    std::exception_ptr original;
    runtime.RegisterKernel(12, [&](X86Context& ctx, uint8_t*) {
      ctx.eax = 0xC0000001;
      ctx.esp += 12;
      ctx.last_icall = 0x402000;
      original = std::make_exception_ptr(ServiceFault(ctx, 0xDEAD, 7, "original service fault"));
      std::rethrow_exception(original);
    });
    bool preserved = false;
    try { runtime.Kernel(context, memory.Base(), 12); }
    catch (const ServiceFault& fault) {
      preserved = std::current_exception() == original && fault.address == 0xDEAD && fault.size == 7 && std::string(fault.what()).starts_with("original service fault address=") && fault.context->eax == 0xC0000001 && fault.context->esp == 0x900C && fault.context->fs_base == 0x22000 && fault.context->last_icall == 0x402000;
    }
    check(preserved, "trace replaced fault type, metadata or context");
    const auto trace = runtime.SnapshotKernelTrace();
    check(trace.count == 2 && trace.events[0].phase == Phase::Before && trace.events[1].phase == Phase::Fault, "fault trace ordering changed");
    check(trace.events[0].callId == trace.events[1].callId && trace.events[1].eax == 0xC0000001 && trace.events[1].esp == 0x900C && trace.events[1].lastIcall == 0x402000, "fault trace registers or pairing changed");
  }
  {
    browser::Runtime runtime({}, {});
    X86Context context{};
    context.host = &runtime;
    context.fs_base = 0x23000;
    runtime.RegisterKernel(16, [&](X86Context& ctx, uint8_t* base) { runtime.Kernel(ctx, base, 17); });
    runtime.RegisterKernel(17, [&](X86Context& ctx, uint8_t*) {
      ctx.eax = 0x1717;
      runtime.Cancel();
      runtime.Checkpoint(ctx);
    });
    bool cancelled = false;
    try { runtime.Kernel(context, memory.Base(), 16); }
    catch (const browser::Cancelled& fault) { cancelled = fault.context->eax == 0x1717 && fault.context->fs_base == 0x23000; }
    check(cancelled, "nested cancellation lost its exception type or context");
    const auto trace = runtime.SnapshotKernelTrace();
    check(trace.count == 4 && trace.events[0].phase == Phase::Before && trace.events[1].phase == Phase::Before && trace.events[2].phase == Phase::Fault && trace.events[3].phase == Phase::Fault, "nested cancellation emitted false service returns");
    check(trace.events[0].callId == trace.events[3].callId && trace.events[1].callId == trace.events[2].callId && trace.events[2].eax == 0x1717 && trace.events[3].eax == 0x1717, "nested cancellation pairing or registers changed");
  }
  {
    browser::Runtime runtime({}, {});
    browser::Runtime foreign({}, {});
    browser::Memory foreignMemory;
    bool entered = false;
    runtime.RegisterKernel(13, [&](X86Context&, uint8_t*) { entered = true; });
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    X86Context context{};
    context.host = &foreign;
    bool rejected = false;
    try { runtime.Kernel(context, memory.Base(), 13); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected, "foreign context was not rejected");
    context.host = &runtime;
    rejected = false;
    try { runtime.Kernel(context, foreignMemory.Base(), 13); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected, "foreign memory was not rejected");
    rejected = false;
    try { runtime.Kernel(context, memory.Base(), 399); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected, "missing service was not rejected");
    runtime.Cancel();
    rejected = false;
    try { runtime.Kernel(context, memory.Base(), 13); }
    catch (const browser::Cancelled&) { rejected = true; }
    check(rejected && !entered && runtime.SnapshotKernelTrace().count == 0, "rejected calls pretended to enter a service");
  }
  {
    browser::Runtime runtime({}, {});
    std::atomic<uint32_t> stage = 0;
    runtime.RegisterKernel(14, [&](X86Context& ctx, uint8_t*) {
      stage.store(1);
      while (stage.load() != 2) { std::this_thread::yield(); }
      ctx.eax = 0x1414;
      ctx.esp += 4;
    });
    runtime.RegisterKernel(15, [&](X86Context& ctx, uint8_t*) {
      stage.store(2);
      while (stage.load() != 3) { std::this_thread::yield(); }
      ctx.eax = 0x1515;
      ctx.esp += 8;
    });
    X86Context first{};
    first.fs_base = 0x31000;
    first.eax = 14;
    first.esp = 0xA000;
    X86Context second{};
    second.fs_base = 0x32000;
    second.eax = 15;
    second.esp = 0xB000;
    runtime.Launch(first, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      runtime.Kernel(ctx, base, 14);
      stage.store(3);
    });
    while (stage.load() != 1) { std::this_thread::yield(); }
    const auto live = runtime.SnapshotKernelTrace();
    check(live.count == 1 && live.events[0].phase == Phase::Before && live.events[0].fsBase == first.fs_base, "trusted live snapshot was incoherent");
    runtime.Launch(second, memory.Base(), [&](X86Context& ctx, uint8_t* base) { runtime.Kernel(ctx, base, 15); });
    check(!runtime.Join(), "competing workers failed");
    const auto trace = runtime.SnapshotKernelTrace();
    check(trace.count == 4, "competing service trace count changed");
    const uint32_t ordinals[]{14, 15, 14, 15};
    for (size_t n = 0; n < trace.count; ++n) {
      const auto& event = trace.events[n];
      const bool firstWorker = n % 2 == 0;
      check(event.sequence == n + 1 && event.ordinal == ordinals[n] && event.phase == (n < 2 ? Phase::Before : Phase::After), "competing trace order changed");
      check(event.fsBase == (firstWorker ? first.fs_base : second.fs_base) && event.workerId != 0, "competing worker attribution lost");
      check(event.callId == (firstWorker ? 1 : 2), "competing call pairing lost");
      check(event.eax == (n < 2 ? (firstWorker ? 14 : 15) : (firstWorker ? 0x1414 : 0x1515)), "competing return value changed");
    }
    check(trace.events[0].workerId == trace.events[2].workerId && trace.events[1].workerId == trace.events[3].workerId && trace.events[0].workerId != trace.events[1].workerId, "worker identities were not stable and distinct");
  }
  std::puts("kernel-trace-contracts-ok");
}
