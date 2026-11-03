#include "browser/cpu_runtime.h"
#include "browser/dispatcher.h"
#include <cassert>
#include <cstring>
#include <cstdio>

int main() {
  browser::Memory memory;
  for (const size_t capacity : {size_t(2), size_t(8), size_t(16), size_t(128)}) {
    browser::Runtime runtime({}, {}, {}, capacity);
    std::atomic<size_t> entered = 0;
    for (size_t n = 0; n < capacity; ++n) {
      runtime.Launch({}, memory.Base(), [&](X86Context& context, uint8_t*) {
        ++entered;
        for (;;) {
          const auto sequence = runtime.Sequence();
          runtime.Checkpoint(context);
          runtime.Wait(context, sequence, std::chrono::seconds(30));
        }
      });
    }
    while (entered.load() != capacity) { std::this_thread::yield(); }
    bool rejected = false;
    try { runtime.Launch({}, memory.Base(), [](X86Context&, uint8_t*) {}); }
    catch (const browser::Fault& fault) { rejected = std::strstr(fault.what(), "guest worker capacity exhausted"); }
    assert(rejected && runtime.ActiveWorkers() == capacity);
    runtime.Cancel();
    assert(!runtime.Join() && runtime.ActiveWorkers() == 0);
  }
  {
    browser::Runtime runtime({}, {}, {}, 8);
    browser::Dispatcher dispatcher(runtime);
    const auto event = dispatcher.CreateEvent(true, false);
    std::atomic<size_t> completed = 0;
    for (size_t n = 0; n < 8; ++n) {
      runtime.Launch({}, memory.Base(), [&](X86Context& context, uint8_t*) {
        if (dispatcher.Wait(context, event, std::chrono::seconds(30)) == browser::Dispatcher::WaitResult::Signaled) { ++completed; }
      });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (dispatcher.ActiveWaiters() != 8 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    const bool admitted = dispatcher.ActiveWaiters() == 8;
    dispatcher.SetEvent(event);
    const auto failure = runtime.Join();
    assert(admitted && !failure && completed == 8 && dispatcher.ActiveWaiters() == 0 && runtime.ActiveWorkers() == 0);
  }
  for (const size_t capacity : {size_t(0), size_t(129)}) {
    bool rejected = false;
    try { browser::Runtime runtime({}, {}, {}, capacity); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
  }
  for (const bool timed : {false, true}) {
    browser::Runtime runtime({}, {});
    browser::Dispatcher dispatcher(runtime, std::nullopt, false, false);
    const auto event = dispatcher.CreateEvent(true, false);
    std::thread driver([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (timed) { dispatcher.AdvanceTime(5000); }
      else { dispatcher.SetEvent(event); }
    });
    const auto result = dispatcher.WaitGuest({}, event, timed ? std::optional<int64_t>(-5000) : std::nullopt,
      std::chrono::milliseconds(1));
    driver.join();
    assert(result == (timed ? browser::Dispatcher::WaitResult::TimedOut : browser::Dispatcher::WaitResult::Signaled));
    assert(dispatcher.ActiveWaiters() == 0);
  }
  std::puts("worker-capacity-contracts-ok");
}
