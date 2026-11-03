#include "browser/cpu_runtime.h"
#include "cpu/x86_ops.h"
#include <array>
#include <cstdio>
#include <limits>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

void serializedSampling() {
  std::atomic<uint64_t> source = 500;
  std::atomic<uint32_t> sampling = 0;
  std::atomic<bool> hold = false;
  std::atomic<bool> sampled = false;
  std::atomic<bool> release = false;
  std::atomic<bool> childReady = false;
  std::atomic<bool> childAttempted = false;
  std::atomic<uint64_t> first = 0;
  std::atomic<uint64_t> second = 0;
  browser::Runtime runtime({}, {}, [&] {
    check(sampling.fetch_add(1) == 0, "timer source calls overlapped across workers");
    const uint64_t value = source.fetch_add(1);
    if (hold.exchange(false)) {
      sampled = true;
      while (!release.load()) { std::this_thread::yield(); }
    }
    --sampling;
    return value;
  });
  browser::Memory memory;
  X86Context context{};
  runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
    runtime.Launch(ctx, base, [&](X86Context& child, uint8_t*) {
      childReady = true;
      while (!sampled.load()) { std::this_thread::yield(); }
      childAttempted = true;
      second = runtime.TimerTime(child);
    });
    while (!childReady.load()) { std::this_thread::yield(); }
    hold = true;
    first = runtime.TimerTime(ctx);
  });
  while (!childAttempted.load()) { std::this_thread::yield(); }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  release = true;
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(first == 1 && second == 2, "delayed earlier sample published after the later worker");
}

void workerRealmClock() {
#ifdef __EMSCRIPTEN__
  browser::Runtime runtime({}, {});
  browser::Memory memory;
  X86Context context{};
  std::atomic<bool> waiting = false;
  std::atomic<bool> finished = false;
  runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
    const uint64_t before = runtime.TimerTime(ctx);
    runtime.Launch(ctx, base, [&](X86Context& child, uint8_t*) {
      EM_ASM({
        const original = globalThis.performance;
        globalThis.__timerOriginalPerformance = original;
        Object.defineProperty(globalThis, 'performance', { configurable: true, writable: true, value: {
          now: () => original.now(), timeOrigin: original.timeOrigin - 60000
        } });
      });
      try {
        check(runtime.TimerTime(child) >= before, "worker realm origin changed the session clock");
      } catch (...) {
        EM_ASM({ globalThis.performance = globalThis.__timerOriginalPerformance; });
        throw;
      }
      EM_ASM({ globalThis.performance = globalThis.__timerOriginalPerformance; });
      const auto sequence = runtime.Sequence();
      waiting = true;
      try { runtime.Wait(child, sequence, std::chrono::seconds(30)); }
      catch (const browser::Cancelled&) { finished = true; throw; }
      check(false, "cancelled clock worker wait returned success");
    });
    while (!waiting.load()) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
    check(runtime.TimerTime(ctx) >= before, "blocked guest worker prevented clock sampling");
    runtime.Cancel();
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  check(finished && !runtime.ActiveWorkers(), "clock cancellation retained a blocked worker");
#endif
}

void canonicalSourceRefusal() {
#ifdef __EMSCRIPTEN__
  MAIN_THREAD_EM_ASM({
    const original = globalThis.performance;
    globalThis.__timerCanonicalPerformance = original;
    globalThis.__timerCanonicalNow = 10000;
    Object.defineProperty(globalThis, 'performance', { configurable: true, writable: true, value: {
      now: () => globalThis.__timerCanonicalNow, timeOrigin: original.timeOrigin
    } });
  });
  browser::Runtime runtime({}, {});
  browser::Memory memory;
  X86Context context{};
  runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
    EM_ASM({
      const original = globalThis.performance;
      globalThis.__timerLocalPerformance = original;
      globalThis.__timerLocalNow = 100;
      Object.defineProperty(globalThis, 'performance', { configurable: true, writable: true, value: {
        now: () => globalThis.__timerLocalNow, timeOrigin: original.timeOrigin
      } });
    });
    const auto local = [](double value) { EM_ASM({ globalThis.__timerLocalNow = $0; }, value); };
    const auto canonical = [](double value) { MAIN_THREAD_EM_ASM({ globalThis.__timerCanonicalNow = $0; }, value); };
    const auto refuse = [&](const char* reason) {
      bool refused = false;
      try { runtime.TimerTime(ctx); }
      catch (const browser::Fault& fault) { refused = std::string(fault.what()).find(reason) != std::string::npos; }
      check(refused, "fresh canonical source failure was hidden by the cache");
    };
    check(runtime.TimerTime(ctx) == 0, "canonical clock did not retain the first worker epoch");
    canonical(10001);
    local(100.5);
    check(runtime.TimerTime(ctx) == 0, "cache extrapolated an unobserved canonical sample");
    local(101);
    check(runtime.TimerTime(ctx) == 10000, "one millisecond cadence did not refresh the actual source");
    canonical(10000.5);
    local(102);
    refuse("timer clock source moved backwards or has no epoch");
    canonical(10002);
    local(103);
    check(runtime.TimerTime(ctx) == 20000, "source refusal reset the accepted epoch");
    canonical(-1);
    local(104);
    refuse("host monotonic timer outside elapsed time range");
    canonical(std::numeric_limits<double>::quiet_NaN());
    local(105);
    refuse("host monotonic timer outside elapsed time range");
    canonical(10003);
    local(106);
    check(runtime.TimerTime(ctx) == 30000, "invalid host time poisoned the last accepted sample");
    canonical(10004);
    local(90);
    check(runtime.TimerTime(ctx) == 40000, "local cadence regression suppressed fresh canonical sampling");
    for (uint32_t n = 0; n < 8; ++n) {
      runtime.Launch(ctx, base, [&](X86Context& child, uint8_t*) {
        check(runtime.TimerTime(child) == 40000, "cadence slot eviction reset the shared session epoch");
      });
      while (runtime.ActiveWorkers() > 1) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
    }
    EM_ASM({ globalThis.performance = globalThis.__timerLocalPerformance; });
  });
  const auto failure = runtime.Join();
  MAIN_THREAD_EM_ASM({ globalThis.performance = globalThis.__timerCanonicalPerformance; });
  if (failure) { std::rethrow_exception(failure); }
#endif
}

void realSourceWorkers() {
#ifdef __EMSCRIPTEN__
  MAIN_THREAD_EM_ASM({
    const original = globalThis.performance;
    globalThis.__timerOriginalMainPerformance = original;
    globalThis.__timerMainSamples = 0;
    Object.defineProperty(globalThis, 'performance', { configurable: true, writable: true, value: {
      now: () => { ++globalThis.__timerMainSamples; return original.now(); }, timeOrigin: original.timeOrigin
    } });
  });
#endif
  browser::Runtime runtime({}, {});
  browser::Memory memory;
  X86Context context{};
  const auto read = [&](X86Context& ctx) {
    uint64_t previous = runtime.TimerTime(ctx);
    for (uint32_t n = 0; n < 1000; ++n) {
      const uint64_t now = runtime.TimerTime(ctx);
      check(now >= previous, "shared real source moved backwards within a worker");
      previous = now;
    }
  };
  const auto start = std::chrono::steady_clock::now();
  runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
    runtime.Launch(ctx, base, [&](X86Context& child, uint8_t*) { read(child); });
    read(ctx);
  });
  if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  std::printf("timer-clock-real-source-2000-samples-ms=%.3f\n", milliseconds);
#ifdef __EMSCRIPTEN__
  const uint32_t sourceSamples = MAIN_THREAD_EM_ASM_INT({
    const count = globalThis.__timerMainSamples;
    globalThis.performance = globalThis.__timerOriginalMainPerformance;
    return count;
  });
  std::printf("timer-clock-real-source-proxies=%u\n", sourceSamples);
  check(sourceSamples > 0 && sourceSamples < 1000, "clock source synchronously proxied every poll instead of bounded sampling");
#endif
}

int main() try {
  serializedSampling();
  workerRealmClock();
  canonicalSourceRefusal();
  realSourceWorkers();
  for (uint32_t n = 0; n < 3; ++n) {
    std::atomic<uint64_t> source = 0;
    std::atomic<uint32_t> samples = 0;
    browser::Runtime runtime({}, {}, [&] { ++samples; return source.load(); });
    browser::Memory memory;
    X86Context context{};
    context.host = &runtime;
    const auto refuse = [&](const auto& operation) {
      bool refused = false;
      try { operation(); } catch (const browser::Fault&) { refused = true; }
      check(refused, "invalid timer clock request succeeded");
    };
    refuse([&] { runtime.TimerTime(context); });
    check(samples == 0, "clock sampled or fabricated an epoch before an owned worker launch");
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
      check(x86_rdtsc() == 123ull * 733333333 / 10000000, "published image RDTSC did not advance at the native guest cycle frequency");
      source -= 1;
      bool backwards = false;
      try { runtime.TimerTime(ctx); }
      catch (const browser::Fault& fault) {
        backwards = std::string(fault.what()).find("timer clock source moved backwards or has no epoch") != std::string::npos;
      }
      check(backwards, "actual source regression was not explicitly refused");
      ++source;
      check(runtime.TimerTime(ctx) == 123, "refused source regression changed the accepted sample or epoch");
      source += 100;
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
  return 0;
} catch (const std::exception& fault) {
  std::fprintf(stderr, "%s\n", fault.what());
  return 1;
}
