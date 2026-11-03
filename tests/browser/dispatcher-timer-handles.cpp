#include "browser/dispatcher-wait-abi.h"
#include <atomic>
#include <cstdio>

using browser::Dispatcher;
using browser::DispatcherHandles;
using browser::Memory;
using browser::Runtime;
using namespace std::chrono_literals;

const auto check = [](bool valid, const char* operation) {
  if (!valid) { throw std::runtime_error(operation); }
};
const auto fault = [](auto operation, const char* name) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, name);
};
const auto parked = [](Dispatcher& dispatcher, size_t count = 1) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (dispatcher.ActiveWaiters() != count && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  check(dispatcher.ActiveWaiters() == count, "wait reaches independent blocking boundary");
};

const auto contracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  X86Context ctx{};
  memory.Map(0x10000, 0x1000);
  memory.Commit(0x10000, 0x1000);
  const uint64_t zero = 0;
  memory.Write(ctx, 0x10000, &zero, sizeof(zero));
  const auto wait = [&](std::span<const uint32_t> set, Dispatcher::WaitMode mode = Dispatcher::WaitMode::Any, uint32_t timeout = 0x10000, std::chrono::milliseconds watchdog = 2s) {
    return handles.WaitGuestStatus(memory, ctx, set, mode, timeout, false, watchdog);
  };
  const auto poll = [&](uint32_t handle) { return wait(std::span(&handle, 1)); };
  {
    const auto timer = handles.CreateTimer(true);
    const auto alias = handles.Duplicate(timer);
    check(poll(timer) == 0x102 && !handles.CancelTimer(alias), "new timer is unarmed and unsignaled");
    check(!handles.SetTimer(timer, 10) && handles.SetTimer(alias, 20), "aliases replace one shared schedule");
    dispatcher.AdvanceTime(10);
    check(poll(timer) == 0x102, "replaced deadline cannot signal through alias");
    dispatcher.SetTimerPaused(true);
    dispatcher.AdvanceTime(100);
    check(poll(alias) == 0x102, "timer handles follow paused virtual time");
    dispatcher.SetTimerPaused(false);
    dispatcher.AdvanceTime(10);
    check(poll(timer) == 0 && poll(alias) == 0, "notification expiry remains signaled through aliases");
    check(!handles.CancelTimer(alias) && poll(timer) == 0, "cancel preserves published notification signal");
    handles.SetTimer(alias, 10);
    check(poll(timer) == 0x102 && handles.CancelTimer(timer), "rearm clears signal and alias cancels pending schedule");
    dispatcher.AdvanceTime(10);
    check(poll(alias) == 0x102, "cancelled schedule cannot signal");
    handles.Close(timer);
    fault([&] { handles.SetTimer(timer, 0); }, "closed handle cannot arm through surviving alias");
    fault([&] { handles.CancelTimer(timer); }, "closed handle cannot cancel surviving alias");
    handles.Close(alias);
  }
  {
    const auto timer = handles.CreateTimer(false);
    const auto alias = handles.Duplicate(timer);
    handles.SetTimer(timer, 2, 5);
    dispatcher.AdvanceTime(102);
    check(poll(alias) == 0 && poll(timer) == 0x102, "periodic expirations coalesce and synchronization consumes once");
    dispatcher.AdvanceTime(5);
    check(handles.CancelTimer(alias) && poll(timer) == 0 && poll(alias) == 0x102, "cancel preserves one signal without future firings");
    dispatcher.AdvanceTime(100);
    check(poll(timer) == 0x102, "cancelled periodic alias cannot republish");
    handles.SetTimer(timer, 0);
    const std::array<uint32_t, 2> aliases{timer, alias};
    fault([&] { wait(aliases); }, "duplicate underlying timers reject any wait");
    fault([&] { wait(aliases, Dispatcher::WaitMode::All); }, "duplicate timer all wait rejects before consumption");
    check(poll(alias) == 0, "alias preflight preserves synchronization signal");
    handles.Close(timer);
    handles.Close(alias);
  }
  for (size_t n = 0; n < 100; ++n) {
    const auto timer = handles.CreateTimer(false);
    handles.SetTimer(timer, 1, n % 2 ? 5 : 0);
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(std::span(&timer, 1), Dispatcher::WaitMode::Any, 0); });
    parked(dispatcher);
    handles.Close(timer);
    check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 1, "final close preserves pending timer schedule through operation pin");
    const auto replacement = handles.CreateTimer(false);
    handles.SetTimer(replacement, 0);
    runtime.Wake();
    check(dispatcher.ActiveWaiters() == 1, "fresh signaled timer cannot complete closed original wait");
    dispatcher.AdvanceTime(1);
    waiter.join();
    check(status == 0 && dispatcher.ActiveObjects() == 1 && poll(replacement) == 0, "expiry completes original then retires its schedule without consuming replacement");
    handles.Close(replacement);
    dispatcher.AdvanceTime(10);
    check(dispatcher.ActiveObjects() == 0, "periodic scheduling cannot retain a timer after its last operation pin");
  }
  for (size_t n = 0; n < 100; ++n) {
    const auto timer = handles.CreateTimer(false);
    handles.SetTimer(timer, 1, 1);
    std::jthread clock([&] { dispatcher.AdvanceTime(1); });
    handles.Close(timer);
    clock.join();
    check(dispatcher.ActiveObjects() == 0, "expiry racing final close cannot retain or resurrect periodic schedule");
  }
  {
    std::array<uint32_t, 64> timers{};
    for (auto& handle : timers) { handle = handles.CreateTimer(false); }
    handles.SetTimer(timers.back(), 0);
    check(wait(timers) == 63, "timer handle at highest supported any index preserves status");
    for (const auto handle : timers) { handles.SetTimer(handle, 1); }
    dispatcher.AdvanceTime(1);
    check(wait(timers, Dispatcher::WaitMode::All) == 0 && wait(timers) == 0x102, "one advance publishes all 64 timer signals before atomic consumption");
    for (const auto handle : timers) { handles.Close(handle); }
  }
  {
    const auto event = handles.CreateEvent(false, true);
    const auto timer = handles.CreateTimer(false);
    handles.SetTimer(timer, 2);
    const std::array<uint32_t, 2> set{event, timer};
    check(wait(set, Dispatcher::WaitMode::All) == 0x102 && poll(event) == 0, "incomplete timer all wait preserves ready event");
    handles.SetEvent(event);
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(set, Dispatcher::WaitMode::All, 0); });
    parked(dispatcher);
    handles.Close(timer);
    dispatcher.AdvanceTime(2);
    waiter.join();
    check(status == 0 && poll(event) == 0x102 && dispatcher.ActiveObjects() == 1, "closed timer expiry completes atomic all and releases its scheduling storage");
    handles.Close(event);
  }
  {
    const auto timer = handles.CreateTimer(false);
    const auto alias = handles.Duplicate(timer);
    handles.SetTimer(timer, 1);
    std::atomic<size_t> completed = 0;
    const auto consume = [&] {
      if (wait(std::span(&timer, 1), Dispatcher::WaitMode::Any, 0) == 0) { ++completed; }
    };
    std::jthread first(consume);
    std::jthread second(consume);
    parked(dispatcher, 2);
    handles.Close(timer);
    check(handles.CancelTimer(alias), "cancel removes schedule but not pending operation pins");
    dispatcher.AdvanceTime(1);
    check(dispatcher.ActiveWaiters() == 2 && completed == 0, "cancellation does not fabricate wait completion");
    handles.SetTimer(alias, 0);
    parked(dispatcher, 1);
    handles.SetTimer(alias, 1);
    handles.Close(alias);
    dispatcher.AdvanceTime(1);
    first.join();
    second.join();
    check(completed == 2 && dispatcher.ActiveObjects() == 0, "two wait pins consume separate expirations despite final close");
  }
  {
    const auto timer = handles.CreateTimer(true);
    const auto alias = handles.Duplicate(timer);
    handles.SetTimer(timer, 1);
    std::array<uint32_t, 2> statuses{};
    std::jthread first([&] { statuses[0] = wait(std::span(&timer, 1), Dispatcher::WaitMode::Any, 0); });
    std::jthread second([&] { statuses[1] = wait(std::span(&alias, 1), Dispatcher::WaitMode::Any, 0); });
    parked(dispatcher, 2);
    handles.Close(timer);
    handles.Close(alias);
    dispatcher.AdvanceTime(1);
    first.join();
    second.join();
    check(statuses[0] == 0 && statuses[1] == 0 && dispatcher.ActiveObjects() == 0, "notification broadcasts through both closed operation pins");
  }
  {
    const auto timer = handles.CreateTimer(false);
    handles.SetTimer(timer, 100, 5);
    const uint64_t relative = UINT64_MAX;
    memory.Write(ctx, 0x10000, &relative, sizeof(relative));
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(std::span(&timer, 1)); });
    parked(dispatcher);
    handles.Close(timer);
    dispatcher.AdvanceTime(1);
    waiter.join();
    check(status == 0x102 && dispatcher.ActiveObjects() == 0, "guest timeout drops final operation pin and pending periodic schedule");
    memory.Write(ctx, 0x10000, &zero, sizeof(zero));
  }
  {
    const auto timer = handles.CreateTimer(false);
    handles.SetTimer(timer, 100, 5);
    bool expired = false;
    std::jthread waiter([&] {
      try { wait(std::span(&timer, 1), Dispatcher::WaitMode::Any, 0, 100ms); }
      catch (const browser::WaitWatchdogExpired&) { expired = true; }
    });
    parked(dispatcher);
    handles.Close(timer);
    waiter.join();
    check(expired && dispatcher.ActiveObjects() == 0, "host watchdog retires closed armed timer without a fabricated guest timeout");
  }
  {
    const auto event = handles.CreateEvent(false, true);
    const auto semaphore = handles.CreateSemaphore(0, 1);
    const auto timer = handles.CreateTimer(false);
    fault([&] { handles.SetTimer(event, 0); }, "event is not a timer");
    fault([&] { handles.CancelTimer(semaphore); }, "semaphore is not a timer");
    handles.SetTimer(timer, 1);
    fault([&] { handles.SetTimer(timer, UINT64_MAX); }, "overflowing replacement rejects without losing original deadline");
    fault([&] { handles.SetEvent(timer); }, "timer is not an event");
    fault([&] { handles.ResetEvent(timer); }, "timer cannot be reset as an event");
    fault([&] { handles.ReleaseSemaphore(timer, 1); }, "timer is not a semaphore");
    fault([&] { wait(std::span(&timer, 1), Dispatcher::WaitMode::Any, 0xFFFFFFFC); }, "invalid timeout leaves timer intact");
    dispatcher.AdvanceTime(1);
    check(poll(timer) == 0 && poll(event) == 0, "rejected operations preserve original schedule and ready event");
    handles.SetTimer(timer, 0);
    const std::array<uint32_t, 4> frame{0x12345678, timer, 0, 0x10000};
    memory.Write(ctx, 0x10101, frame.data(), sizeof(frame));
    ctx.esp = 0x10101;
    browser::InvokeDispatcherWait(handles, memory, ctx, 233, 2s);
    check(ctx.eax == 0 && ctx.esp == 0x10111 && poll(timer) == 0x102, "checked stdcall wait ABI consumes real timer handle signal");
    handles.Close(event);
    handles.Close(semaphore);
    handles.Close(timer);
  }
  {
    std::array<uint32_t, browser::kDispatcherCapacity> timers{};
    timers[0] = handles.CreateTimer(false);
    for (size_t n = 1; n < timers.size(); ++n) { timers[n] = handles.Duplicate(timers[0]); }
    fault([&] { handles.CreateTimer(true); }, "timer creation observes handle budget before object allocation");
    check(dispatcher.ActiveObjects() == 1, "failed timer allocation leaks no object");
    for (const auto handle : timers) { handles.Close(handle); }
    for (auto& handle : timers) { handle = handles.CreateTimer(false); handles.SetTimer(handle, 100, 1); }
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(std::span(&timers.back(), 1), Dispatcher::WaitMode::Any, 0); });
    parked(dispatcher);
    handles.Close(timers.back());
    fault([&] { handles.CreateTimer(false); }, "closed but pinned timer still consumes object budget");
    dispatcher.AdvanceTime(100);
    waiter.join();
    check(status == 0 && dispatcher.ActiveObjects() == browser::kDispatcherCapacity - 1, "expiry releases pinned timer capacity");
    timers.back() = handles.CreateTimer(false);
    for (const auto handle : timers) { handles.Close(handle); }
    dispatcher.AdvanceTime(100);
    check(dispatcher.ActiveObjects() == 0, "closing armed periodic timers without waits retires every schedule");
  }
  {
    DispatcherHandles scoped(dispatcher);
    scoped.SetTimer(scoped.CreateTimer(false), 10, 1);
  }
  dispatcher.AdvanceTime(10);
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0, "timer contracts clean up handles waits and schedules");
};

DispatcherHandles* workerHandles = nullptr;
uint32_t workerHandle = 0;
std::atomic<bool> cancelled = false;
const auto workerWait = [](X86Context& ctx, uint8_t* base) {
  try {
    workerHandles->WaitGuestStatus(Memory::From(base), ctx, std::span(&workerHandle, 1), Dispatcher::WaitMode::Any, 0, false, 2s);
  } catch (const browser::Cancelled&) { cancelled = true; throw; }
};
const auto cancellation = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  X86Context ctx{};
  ctx.host = &runtime;
  workerHandles = &handles;
  workerHandle = handles.CreateTimer(false);
  handles.SetTimer(workerHandle, 100, 5);
  const auto cleanup = handles.CreateTimer(true);
  handles.SetTimer(cleanup, 100);
  runtime.Launch(ctx, memory.Base(), workerWait);
  parked(dispatcher);
  handles.Close(workerHandle);
  check(!runtime.Stop() && cancelled && dispatcher.ActiveWaiters() == 0 && runtime.ActiveWorkers() == 0 && dispatcher.ActiveObjects() == 1, "Stop unwinds closed timer wait and joins owned worker");
  fault([&] { handles.CreateTimer(false); }, "cancelled runtime rejects timer allocation");
  fault([&] { handles.SetTimer(cleanup, 0); }, "cancelled runtime rejects timer arming");
  fault([&] { handles.CancelTimer(cleanup); }, "cancelled runtime rejects new handle operations");
  handles.Close(cleanup);
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0, "close still retires armed timers after runtime cancellation");
  workerHandles = nullptr;
};

int main() {
  try {
    contracts();
    cancellation();
    std::puts("dispatcher-timer-handles-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
