#include "browser/cpu_runtime.h"
#include <atomic>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

int main() {
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
