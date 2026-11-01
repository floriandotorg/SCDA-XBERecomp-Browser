#include "browser/dispatcher-handles.h"
#include <bit>
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
const auto parked = [](Dispatcher& dispatcher) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (dispatcher.ActiveWaiters() != 1 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  check(dispatcher.ActiveWaiters() == 1, "wait reaches independent blocking boundary");
};
const auto writeDue = [](Memory& memory, const X86Context& ctx, uint32_t address, int64_t due) {
  const auto bits = std::bit_cast<uint64_t>(due);
  std::array<uint8_t, 8> bytes{};
  for (size_t n = 0; n < bytes.size(); ++n) { bytes[n] = uint8_t(bits >> (n * 8)); }
  memory.Write(ctx, address, bytes.data(), bytes.size());
};

const auto contracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime, 1000);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  X86Context ctx{};
  memory.Map(0x10000, 0x3000);
  memory.Commit(0x10000, 0x2000);
  constexpr uint32_t dueAddress = 0x10003;
  constexpr uint32_t pollAddress = 0x10100;
  writeDue(memory, ctx, pollAddress, 0);
  const auto timer = handles.CreateTimer(false);
  const auto alias = handles.Duplicate(timer);
  const auto poll = [&] {
    return handles.WaitGuestStatus(memory, ctx, std::span(&alias, 1), Dispatcher::WaitMode::Any, pollAddress, false, 2s);
  };
  const auto set = [&](int64_t due, int32_t period = 0) {
    writeDue(memory, ctx, dueAddress, due);
    return handles.SetTimerGuest(memory, ctx, timer, dueAddress, period);
  };
  check(!set(-7) && poll() == 0x102, "relative signed due time creates future schedule");
  dispatcher.AdvanceTime(6);
  check(poll() == 0x102, "relative timer does not expire early");
  dispatcher.AdvanceTime(1);
  check(poll() == 0 && poll() == 0x102, "relative timer expires exactly and consumes once");
  check(!set(1017), "absolute timer uses explicit system origin");
  dispatcher.AdvanceTime(9);
  check(poll() == 0x102, "absolute timer does not use host time");
  dispatcher.AdvanceTime(1);
  check(poll() == 0, "absolute deadline matches virtual system time");
  for (const auto due : {int64_t(0), int64_t(1), int64_t(1000), int64_t(1017)}) {
    check(!set(due) && poll() == 0, "zero and past absolute due times expire immediately");
  }
  check(!set(-10) && set(-20), "relative replacement returns prior armed state");
  dispatcher.AdvanceTime(10);
  check(poll() == 0x102, "replaced due time cannot signal");
  dispatcher.SetTimerPaused(true);
  dispatcher.AdvanceTime(100000);
  check(poll() == 0x102, "guest timer respects paused virtual clock");
  dispatcher.SetTimerPaused(false);
  dispatcher.AdvanceTime(10);
  check(poll() == 0, "relative replacement retains exact deadline after pause");
  check(!set(-1, 2), "guest period is signed milliseconds");
  dispatcher.AdvanceTime(1);
  check(poll() == 0, "initial due time remains 100 ns ticks despite millisecond period");
  dispatcher.AdvanceTime(19999);
  check(poll() == 0x102, "period conversion does not fire early");
  dispatcher.AdvanceTime(1);
  check(poll() == 0, "two millisecond period is exactly 20000 ticks");
  dispatcher.AdvanceTime(100000);
  check(poll() == 0 && poll() == 0x102, "missed guest periods coalesce");
  check(handles.CancelTimer(alias), "alias cancels converted periodic schedule");
  set(0, INT32_MAX);
  check(poll() == 0, "maximum positive millisecond period accepts immediate first expiry");
  dispatcher.AdvanceTime(uint64_t(INT32_MAX) * 10000 - 1);
  check(poll() == 0x102, "maximum period multiplication does not truncate");
  dispatcher.AdvanceTime(1);
  check(poll() == 0 && handles.CancelTimer(alias), "maximum period expires exactly");
  {
    const auto event = handles.CreateEvent(false, true);
    set(-3);
    writeDue(memory, ctx, dueAddress, -1);
    fault([&] { handles.SetTimerGuest(memory, ctx, timer, dueAddress, -1); }, "negative period rejects");
    fault([&] { handles.SetTimerGuest(memory, ctx, event, dueAddress, 0); }, "guest timer rejects wrong object kind");
    fault([&] { handles.SetTimerGuest(memory, ctx, 0, dueAddress, 0); }, "guest timer rejects unknown handle");
    for (const auto address : {0u, 0x12000u, 0x11FFCu, 0xFFFFFFFCu}) {
      fault([&] { handles.SetTimerGuest(memory, ctx, timer, address, 0); }, "invalid required due pointer rejects");
    }
    {
      auto lease = memory.Borrow();
      fault([&] { handles.SetTimerGuest(memory, ctx, timer, dueAddress, 0); }, "guest timer rejects inherited backing lease");
    }
    dispatcher.AdvanceTime(2);
    check(poll() == 0x102, "invalid replacements do not change original deadline");
    dispatcher.AdvanceTime(1);
    check(poll() == 0, "original schedule survives every failed preflight");
    handles.Close(event);
  }
  {
    set(0, 1);
    writeDue(memory, ctx, dueAddress, -1);
    fault([&] { handles.SetTimerGuest(memory, ctx, timer, dueAddress, -1); }, "invalid period preserves published signal and periodic schedule");
    check(poll() == 0, "failed scheduling must not clear an existing signal");
    dispatcher.AdvanceTime(10000);
    check(poll() == 0 && handles.CancelTimer(alias), "failed scheduling must not replace the existing period");
  }
  for (size_t n = 0; n < 100; ++n) {
    writeDue(memory, ctx, dueAddress, -1);
    std::jthread clock([&] { dispatcher.AdvanceTime(1); });
    handles.SetTimerGuest(memory, ctx, timer, dueAddress);
    clock.join();
    dispatcher.AdvanceTime(1);
    check(poll() == 0 && poll() == 0x102, "relative conversion and publication serialize with concurrent clock advancement");
  }
  {
    const auto notification = handles.CreateTimer(true);
    writeDue(memory, ctx, dueAddress, -1);
    handles.SetTimerGuest(memory, ctx, notification, dueAddress);
    dispatcher.AdvanceTime(1);
    for (size_t n = 0; n < 2; ++n) {
      check(handles.WaitGuestStatus(memory, ctx, std::span(&notification, 1), Dispatcher::WaitMode::Any, pollAddress, false, 2s) == 0,
        "signed scheduling preserves notification persistence");
    }
    handles.Close(notification);
  }
  {
    memory.Map(0xFFFFF000, 0x1000);
    memory.Commit(0xFFFFF000, 0x1000);
    writeDue(memory, ctx, 0xFFFFFFF8, -1);
    handles.SetTimerGuest(memory, ctx, timer, 0xFFFFFFF8);
    dispatcher.AdvanceTime(1);
    check(poll() == 0, "due time may end exactly at the 32 bit address boundary");
  }
  {
    set(-5);
    writeDue(memory, ctx, dueAddress, 0);
    memory.Decommit(0x10000, 0x1000);
    dispatcher.AdvanceTime(4);
    memory.Commit(0x10000, 0x1000);
    check(poll() == 0x102, "schedule retains value rather than guest pointer");
    dispatcher.AdvanceTime(1);
    check(poll() == 0, "guest pointer mutation and decommit cannot redirect deadline");
  }
  {
    set(INT64_MIN);
    check(poll() == 0x102 && handles.CancelTimer(alias), "INT64_MIN magnitude is decoded without signed overflow");
    set(INT64_MAX);
    check(poll() == 0x102 && handles.CancelTimer(alias), "maximum absolute due time stays in guest clock domain");
  }
  {
    set(-5);
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] {
      status = handles.WaitGuestStatus(memory, ctx, std::span(&timer, 1), Dispatcher::WaitMode::Any, 0, false, 2s);
    });
    parked(dispatcher);
    handles.Close(timer);
    handles.Close(alias);
    memory.Decommit(0x10000, 0x1000);
    dispatcher.AdvanceTime(5);
    waiter.join();
    check(status == 0 && dispatcher.ActiveObjects() == 0, "signed schedule completes closed pinned timer without rereading guest due storage");
  }
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveWaiters() == 0, "guest scheduling contracts clean up");
};

const auto boundaries = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  const auto timer = dispatcher.CreateTimer(false);
  const auto poll = [&] { return dispatcher.Wait({}, timer, 0ms); };
  dispatcher.SetTimer(timer, 3);
  fault([&] { dispatcher.SetTimerGuest(timer, 1, 0); }, "absolute scheduling requires an explicit system time origin");
  dispatcher.AdvanceTime(3);
  check(poll() == Dispatcher::WaitResult::Signaled, "missing origin preserves original schedule");
  dispatcher.SetTimerGuest(timer, 0, 0);
  check(poll() == Dispatcher::WaitResult::Signaled, "zero due is immediate without origin");
  dispatcher.AdvanceTime(UINT64_MAX - 5 - dispatcher.TimerTime());
  dispatcher.SetTimerGuest(timer, -5, 0);
  fault([&] { dispatcher.SetTimerGuest(timer, INT64_MIN, 0); }, "relative deadline overflow rejects");
  fault([&] { dispatcher.SetTimerGuest(timer, -6, 0); }, "one tick beyond remaining clock range rejects");
  check(poll() == Dispatcher::WaitResult::TimedOut, "overflow preflight preserves unsignaled state");
  dispatcher.AdvanceTime(5);
  check(poll() == Dispatcher::WaitResult::Signaled, "largest representable deadline remains scheduled after failures");
  dispatcher.SetTimerGuest(timer, 0, 1);
  check(poll() == Dispatcher::WaitResult::Signaled && !dispatcher.CancelTimer(timer), "periodic overflow publishes final expiry and disarms");
  runtime.Stop();
  fault([&] { dispatcher.SetTimerGuest(timer, 0, 0); }, "cancelled runtime rejects signed scheduling");
  dispatcher.Destroy(timer);
};

int main() {
  try {
    contracts();
    boundaries();
    std::puts("dispatcher-guest-timer-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
