#include "browser/cpu_runtime.h"
#include <array>
#include <cstdio>
#include <limits>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    std::atomic<uint64_t> source = 0;
    browser::Runtime runtime({}, {}, [&] { return source.load(); });
    browser::Memory memory;
    X86Context context{};
    context.host = &runtime;
    const auto refuse = [&](const auto& operation) {
      bool refused = false;
      try { operation(); } catch (const browser::Fault&) { refused = true; }
      check(refused, "invalid timer clock request succeeded");
    };
    refuse([&] { runtime.TimerTime(context); });
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    source = 900000;
    std::atomic<uint64_t> childTime = 0;
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      check(runtime.TimerTime(ctx) == 0, "timer epoch includes prelaunch host time");
      source += 123;
      check(runtime.TimerTime(ctx) == 123, "timer clock lost 100 ns resolution");
      check(runtime.RelativeTimerDeadline(ctx, -216000000000ll) == 216000000123ull,
        "observed six hour relative timer used the wrong unit or origin");
      check(runtime.RelativeTimerDeadline(ctx, -1) == 124, "relative timer lost its smallest interval");
      check(runtime.RelativeTimerDeadline(ctx, std::numeric_limits<int64_t>::min()) == (1ull << 63) + 123,
        "minimum signed relative timer overflowed");
      refuse([&] { runtime.RelativeTimerDeadline(ctx, 0); });
      refuse([&] { runtime.RelativeTimerDeadline(ctx, 1); });
      auto foreign = ctx;
      foreign.host = nullptr;
      refuse([&] { runtime.TimerTime(foreign); });
      runtime.ticks = 999999;
      check(runtime.TimerTime(ctx) == 123, "timer clock was coupled to synthetic CPU ticks");
      source -= 1;
      refuse([&] { runtime.TimerTime(ctx); });
      source += 101;
      runtime.Launch(ctx, base, [&](X86Context& child, uint8_t*) {
        childTime = runtime.TimerTime(child);
      });
    });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(childTime == 223 && runtime.ActiveWorkers() == 0, "descendant did not share the session timer epoch");
    source = UINT64_MAX;
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t*) {
      check(runtime.TimerTime(ctx) == UINT64_MAX - 900000, "large monotonic timer sample wrapped");
      refuse([&] { runtime.RelativeTimerDeadline(ctx, std::numeric_limits<int64_t>::min()); });
      runtime.Cancel();
      refuse([&] { runtime.TimerTime(ctx); });
    });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    refuse([&] { runtime.TimerTime(context); });
  }
  browser::Runtime real({}, {});
  browser::Memory memory;
  X86Context context{};
  context.host = &real;
  real.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t*) {
    const auto before = real.TimerTime(ctx);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(real.TimerTime(ctx) >= before + 10000, "production monotonic timer source did not advance");
  });
  if (auto failure = real.Join()) { std::rethrow_exception(failure); }
  std::puts("timer-clock-contracts-ok");
}
