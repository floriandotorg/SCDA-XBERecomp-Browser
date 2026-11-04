#include "browser/dispatcher.h"
#include <atomic>
#include <cstdio>

int main() {
  try {
    using namespace std::chrono_literals;
    const auto check = [](bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } };
    std::atomic<uint32_t> samples = 0;
    const auto origin = std::chrono::steady_clock::now();
    browser::Runtime runtime({}, {}, [&] {
      ++samples;
      return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - origin).count()) / 100;
    });
    browser::Dispatcher dispatcher(runtime, std::nullopt, true);
    browser::Memory memory;
    X86Context context{};
    context.host = &runtime;
    const auto event = dispatcher.CreateEvent(false, false);
    const auto parked = [&] {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!dispatcher.ActiveWaiters() && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
      check(dispatcher.ActiveWaiters() == 1, "dispatcher worker never reached its wait boundary");
    };
    browser::Dispatcher::WaitResult result;
    runtime.Launch(context, memory.Base(), [&](X86Context& worker, uint8_t*) {
      result = dispatcher.WaitGuest(worker, event, std::nullopt, 5s);
    });
    parked();
    std::this_thread::sleep_for(10ms);
    const auto before = samples.load();
    std::this_thread::sleep_for(50ms);
    const bool idle = samples.load() <= before + 1;
    dispatcher.SetEvent(event);
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(idle, "indefinite event waits repeatedly polled the guest clock");
    check(result == browser::Dispatcher::WaitResult::Signaled, "an indefinite event wait missed its real signal");
    runtime.Launch(context, memory.Base(), [&](X86Context& worker, uint8_t*) {
      result = dispatcher.WaitGuest(worker, event, -200000, 5s);
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(result == browser::Dispatcher::WaitResult::TimedOut, "relative event deadline stopped advancing");
    const auto timer = dispatcher.CreateTimer(false);
    dispatcher.SetTimerGuest(timer, -200000, 0);
    runtime.Launch(context, memory.Base(), [&](X86Context& worker, uint8_t*) {
      result = dispatcher.WaitGuest(worker, timer, std::nullopt, 5s);
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(result == browser::Dispatcher::WaitResult::Signaled, "indefinite timer wait stopped sampling its deadline");
    std::atomic<bool> cancelled = false;
    runtime.Launch(context, memory.Base(), [&](X86Context& worker, uint8_t*) {
      try { dispatcher.WaitGuest(worker, event, std::nullopt, 5s); }
      catch (const browser::Cancelled&) { cancelled = true; throw; }
    });
    parked();
    check(!runtime.Stop() && cancelled && !dispatcher.ActiveWaiters() && !runtime.ActiveWorkers(), "indefinite idle wait missed cancellation");
    std::puts("dispatcher-polling-contracts-ok");
  } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
