#include "browser/cpu_runtime.h"
#include "cpu/x86_ops.h"
#include <atomic>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

int main() {
  {
    browser::Memory memory;
    const std::array<X86FuncMapping, 1> mappings = {{{0x10000, [](X86Context&, uint8_t*) {}}}};
    browser::Runtime runtime(mappings, {});
    const auto nativeTarget = runtime.HostThunk([](X86Context&, uint8_t*) {});
    std::atomic<uint32_t> deliveries = 0;
    runtime.InstallExpiryDriver([&](X86Context&, uint8_t*) { ++deliveries; });
    runtime.RegisterKernel(1, [](X86Context&, uint8_t*) {});
    runtime.InstallSdkObserver(memory, [](X86Context&, uint32_t, uint64_t) { return true; });
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    X86Context context{};
    context.host = &runtime;
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      for (uint32_t n = 0; n < 10000; ++n) { X86_BARRIER(); }
      check(deliveries == 40, "loop barriers must bound timer work to one delivery check per 256 backedges");
      check(runtime.ProgressAddress()->load() == 10000, "sampled timer work lost guest progress");
      runtime.Poll(ctx, base);
      for (uint32_t n = 0; n < 10000; ++n) { runtime.Invoke(ctx, base, 0x10000); }
      check(deliveries == 81, "guest indirect calls repeated full timer servicing");
      runtime.Invoke(ctx, base, nativeTarget);
      check(deliveries == 82, "native thunk boundary did not force timer work");
      runtime.Kernel(ctx, base, 1);
      check(deliveries == 83, "kernel boundary did not force timer work");
      check(browser::Runtime::ObserveSdkEntry(ctx, base, 1) && deliveries == 84, "SDK boundary did not force timer work");
      runtime.Poll(ctx, base);
      check(deliveries == 85, "explicit poll did not force timer work");
      X86_BARRIER();
      check(deliveries == 86, "first backedge after an explicit boundary did not refresh timers");
      runtime.Launch(ctx, base, [&](X86Context& ctx, uint8_t* base) { X86_BARRIER(); });
      while (runtime.ActiveWorkers() > 1) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
      check(deliveries == 87, "child worker inherited the parent timer budget");
      runtime.Cancel();
      bool cancelled = false;
      try { X86_BARRIER(); } catch (const browser::Cancelled&) { cancelled = true; }
      check(cancelled, "sampled loop barrier skipped cancellation");
    });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
  browser::Memory memory;
  browser::Runtime runtime({}, {});
  auto* progress = runtime.ProgressAddress();
  check(progress && progress->is_lock_free() && progress->load() == 0, "progress must be a fresh lock free word");
  X86Context context{};
  context.host = &runtime;
  runtime.Checkpoint(context);
  runtime.Wake();
  runtime.Poll(context, memory.Base());
  check(progress->load() == 0, "host callbacks and unowned polling are not guest progress");
  std::atomic<bool> waiting = false;
  std::atomic<bool> resumed = false;
  runtime.Launch(context, memory.Base(), [&](X86Context& guest, uint8_t* base) {
    for (uint32_t n = 0; n < 1000; ++n) { runtime.Poll(guest, base); }
    waiting = true;
    const auto sequence = runtime.Sequence();
    while (!resumed.load()) { runtime.Wait(guest, sequence, std::chrono::milliseconds(1)); }
    runtime.Poll(guest, base);
  });
  while (!waiting.load()) { std::this_thread::yield(); }
  const auto work = progress->load();
  check(work >= 1000, "owned runtime execution must advance progress");
  for (uint32_t n = 0; n < 20; ++n) {
    runtime.Wake();
    runtime.Checkpoint(context);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(progress->load() == work, "wait retries, host callbacks and wake noise cannot fake execution");
  resumed = true;
  runtime.Wake();
  check(!runtime.Join() && progress->load() > work, "resumed execution must advance progress");
  runtime.Stop();
  const auto stopped = progress->load();
  bool cancelled = false;
  try { runtime.Poll(context, memory.Base()); }
  catch (const browser::Cancelled&) { cancelled = true; }
  check(cancelled && progress->load() == stopped, "retired runtime cannot publish new progress");
  browser::Runtime replacement({}, {});
  check(replacement.ProgressAddress()->load() == 0, "replacement cannot inherit retired progress");
  std::puts("runtime-progress-contracts-ok");
}
