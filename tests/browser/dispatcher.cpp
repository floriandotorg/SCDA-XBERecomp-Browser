#include "browser/dispatcher.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <limits>
#include <optional>

using browser::Dispatcher;
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
const auto parked = [](Dispatcher& dispatcher, size_t count) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (dispatcher.ActiveWaiters() != count && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  check(dispatcher.ActiveWaiters() == count, "waiters reach independent blocking boundary");
};

Dispatcher* ownedDispatcher = nullptr;
uint32_t ownedObject = 0;
uint32_t ownedOwner = 0;
std::atomic<bool> ownedCancelled = false;
std::array<uint32_t, 2> ownedObjects{};
std::optional<Dispatcher::WaitMode> ownedMode;
bool ownedGuestWait = false;
void ownedWait(X86Context& ctx, uint8_t*) {
  try {
    if (ownedGuestWait) { ownedDispatcher->WaitGuest(ctx, ownedObject, std::nullopt, 30s, ownedOwner); }
    else if (ownedMode) { ownedDispatcher->WaitMultiple(ctx, ownedObjects, *ownedMode, 30s, ownedOwner); }
    else { ownedDispatcher->Wait(ctx, ownedObject, 30s, ownedOwner); }
  }
  catch (const browser::Cancelled&) { ownedCancelled = true; throw; }
}

void multipleContracts(Runtime& runtime, Dispatcher& dispatcher, const X86Context& ctx) {
  const auto first = dispatcher.CreateEvent(false, true);
  const auto second = dispatcher.CreateSemaphore(0, 2);
  const auto notification = dispatcher.CreateEvent(true, true);
  std::array<uint32_t, 3> objects{first, second, notification};
  const auto wait = [&](Dispatcher::WaitMode mode, std::chrono::milliseconds timeout = 0ms) {
    return dispatcher.WaitMultiple(ctx, objects, mode, timeout);
  };
  const auto expect = [](const auto& outcome, Dispatcher::WaitResult result, std::optional<size_t> index, const char* operation) {
    check(outcome.result == result && outcome.index == index, operation);
  };
  expect(wait(Dispatcher::WaitMode::All), Dispatcher::WaitResult::TimedOut, {}, "incomplete all wait polls without a selected index");
  check(dispatcher.Wait(ctx, first, 0ms) == Dispatcher::WaitResult::Signaled, "failed all poll preserves synchronization signal");
  dispatcher.SetEvent(first);
  dispatcher.ReleaseSemaphore(second, 1);
  expect(wait(Dispatcher::WaitMode::All), Dispatcher::WaitResult::Signaled, {}, "all wait succeeds atomically without a selected index");
  check(dispatcher.Wait(ctx, first, 0ms) == Dispatcher::WaitResult::TimedOut && dispatcher.Wait(ctx, second, 0ms) == Dispatcher::WaitResult::TimedOut, "all wait consumes each consumable exactly once");
  expect(wait(Dispatcher::WaitMode::Any), Dispatcher::WaitResult::Signaled, 2, "all wait preserves notification state");
  dispatcher.SetEvent(first);
  dispatcher.ReleaseSemaphore(second, 1);
  expect(wait(Dispatcher::WaitMode::Any), Dispatcher::WaitResult::Signaled, 0, "any chooses first ready object in input order");
  expect(wait(Dispatcher::WaitMode::Any), Dispatcher::WaitResult::Signaled, 1, "any leaves other permits intact");
  dispatcher.ResetEvent(notification);
  expect(wait(Dispatcher::WaitMode::Any), Dispatcher::WaitResult::TimedOut, {}, "any timeout does not select an index");
  const auto invalid = [&](std::span<const uint32_t> ids, Dispatcher::WaitMode mode, std::chrono::milliseconds timeout = 0ms) {
    fault([&] { dispatcher.WaitMultiple(ctx, ids, mode, timeout); }, "invalid wait set rejects before consumption");
  };
  dispatcher.SetEvent(first);
  invalid({}, Dispatcher::WaitMode::Any);
  invalid({}, Dispatcher::WaitMode::All);
  const std::array<uint32_t, 2> duplicate{first, first};
  invalid(duplicate, Dispatcher::WaitMode::Any);
  invalid(duplicate, Dispatcher::WaitMode::All);
  const std::array<uint32_t, 2> unknown{first, UINT32_MAX};
  invalid(unknown, Dispatcher::WaitMode::Any);
  invalid(unknown, Dispatcher::WaitMode::All);
  invalid(objects, static_cast<Dispatcher::WaitMode>(99));
  invalid(objects, Dispatcher::WaitMode::Any, -1ms);
  invalid(objects, Dispatcher::WaitMode::All, std::chrono::milliseconds::max());
  check(dispatcher.Wait(ctx, first, 0ms) == Dispatcher::WaitResult::Signaled && dispatcher.ActiveWaiters() == 0, "invalid sets preserve signals and admission budget");
  {
    browser::Memory memory;
    auto lease = memory.Borrow();
    invalid(objects, Dispatcher::WaitMode::All);
  }
  {
    const std::array<uint32_t, 2> pair{first, second};
    const auto start = std::chrono::steady_clock::now();
    std::atomic<bool> done = false;
    std::jthread unrelated([&] {
      while (!done.load()) { runtime.Wake(); std::this_thread::sleep_for(1ms); }
    });
    dispatcher.SetEvent(first);
    expect(dispatcher.WaitMultiple(ctx, pair, Dispatcher::WaitMode::All, 20ms), Dispatcher::WaitResult::TimedOut, {}, "incomplete all wait expires despite unrelated wakes");
    done = true;
    unrelated.join();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    check(elapsed >= 20ms && elapsed < 1s, "all timeout uses one deadline");
    check(dispatcher.Wait(ctx, first, 0ms) == Dispatcher::WaitResult::Signaled, "timed out all wait cannot consume a partial set");
  }
  {
    Dispatcher::WaitOutcome result{};
    std::jthread waiter([&] { result = wait(Dispatcher::WaitMode::All, 2s); });
    parked(dispatcher, 1);
    dispatcher.SetEvent(first);
    dispatcher.SetEvent(notification);
    check(dispatcher.Wait(ctx, first, 0ms) == Dispatcher::WaitResult::Signaled, "pending all wait cannot reserve a subset");
    dispatcher.SetEvent(first);
    dispatcher.ReleaseSemaphore(second, 1);
    waiter.join();
    expect(result, Dispatcher::WaitResult::Signaled, {}, "all waiter completes on final independent publication");
    dispatcher.ResetEvent(notification);
  }
  {
    std::array<Dispatcher::WaitOutcome, 2> results{};
    const std::array<uint32_t, 2> pair{first, second};
    std::jthread all([&] { results[0] = dispatcher.WaitMultiple(ctx, pair, Dispatcher::WaitMode::All, 2s); });
    std::jthread any([&] { results[1] = dispatcher.WaitMultiple(ctx, pair, Dispatcher::WaitMode::Any, 2s); });
    parked(dispatcher, 2);
    invalid(objects, Dispatcher::WaitMode::All, 1ms);
    dispatcher.ReleaseSemaphore(second, 2);
    any.join();
    expect(results[1], Dispatcher::WaitResult::Signaled, 1, "any can consume a permit while all is unsatisfied");
    dispatcher.SetEvent(first);
    all.join();
    expect(results[0], Dispatcher::WaitResult::Signaled, {}, "contending all consumes remaining permit and event atomically");
    check(dispatcher.Wait(ctx, first, 0ms) == Dispatcher::WaitResult::TimedOut && dispatcher.Wait(ctx, second, 0ms) == Dispatcher::WaitResult::TimedOut, "mixed waits never overconsume");
  }
  for (const auto mode : {Dispatcher::WaitMode::Any, Dispatcher::WaitMode::All}) {
    const auto retired = dispatcher.CreateEvent(false, false);
    std::array<uint32_t, 2> pair{first, retired};
    if (mode == Dispatcher::WaitMode::All) { dispatcher.SetEvent(first); }
    Dispatcher::WaitOutcome result{};
    std::jthread waiter([&] { result = dispatcher.WaitMultiple(ctx, pair, mode, 2s); });
    parked(dispatcher, 1);
    pair[1] = second;
    dispatcher.Destroy(retired);
    const auto replacement = dispatcher.CreateEvent(false, true);
    waiter.join();
    expect(result, Dispatcher::WaitResult::Destroyed, 1, "wait snapshots its identifiers and reports original retired index");
    if (mode == Dispatcher::WaitMode::All) {
      check(dispatcher.Wait(ctx, first, 0ms) == Dispatcher::WaitResult::Signaled, "retirement rejects before consuming ready members");
    }
    check(dispatcher.Wait(ctx, replacement, 0ms) == Dispatcher::WaitResult::Signaled, "retired set cannot consume a replacement");
    dispatcher.Destroy(replacement);
  }
  {
    const auto response = dispatcher.CreateSemaphore(0, 1);
    const std::array<uint32_t, 2> pair{first, response};
    std::jthread responder([&] {
      for (size_t n = 0; n < 200; ++n) {
        check(dispatcher.Wait(ctx, second, 2s) == Dispatcher::WaitResult::Signaled, "multiple wait request handoff does not lose a wake");
        dispatcher.ReleaseSemaphore(response, 1);
      }
    });
    for (size_t n = 0; n < 200; ++n) {
      dispatcher.ReleaseSemaphore(second, 1);
      expect(dispatcher.WaitMultiple(ctx, pair, Dispatcher::WaitMode::Any, 2s), Dispatcher::WaitResult::Signaled, 1, "multiple wait response survives racing admission");
    }
    responder.join();
    dispatcher.Destroy(response);
  }
  dispatcher.Destroy(first);
  dispatcher.Destroy(second);
  dispatcher.Destroy(notification);
  {
    std::array<uint32_t, 65> boundary{};
    for (size_t n = 0; n < boundary.size(); ++n) { boundary[n] = dispatcher.CreateSemaphore(1, 1); }
    invalid(boundary, Dispatcher::WaitMode::Any);
    invalid(boundary, Dispatcher::WaitMode::All);
    const auto maximum = std::span(boundary).first(64);
    expect(dispatcher.WaitMultiple(ctx, maximum, Dispatcher::WaitMode::All, 0ms), Dispatcher::WaitResult::Signaled, {}, "64 object all wait consumes the complete set");
    expect(dispatcher.WaitMultiple(ctx, maximum, Dispatcher::WaitMode::Any, 0ms), Dispatcher::WaitResult::TimedOut, {}, "64 object any wait observes consumed set");
    dispatcher.ReleaseSemaphore(boundary[63], 1);
    expect(dispatcher.WaitMultiple(ctx, maximum, Dispatcher::WaitMode::Any, 0ms), Dispatcher::WaitResult::Signaled, 63, "last supported index is returned exactly");
    check(dispatcher.Wait(ctx, boundary[64], 0ms) == Dispatcher::WaitResult::Signaled, "oversized waits cannot partially consume even their final object");
    for (const auto object : boundary) { dispatcher.Destroy(object); }
  }
  check(dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0, "multiple waits leave no live state");
}

void mutantContracts(Dispatcher& dispatcher, const X86Context& ctx) {
  constexpr uint32_t firstOwner = 4;
  constexpr uint32_t secondOwner = 8;
  const auto mutant = dispatcher.CreateMutant();
  const auto event = dispatcher.CreateEvent(false, true);
  const auto semaphore = dispatcher.CreateSemaphore(1, 1);
  const auto poll = [&](uint32_t object, uint32_t owner) { return dispatcher.Wait(ctx, object, 0ms, owner); };
  fault([&] { poll(mutant, 0); }, "mutant wait requires an explicit owner");
  fault([&] { dispatcher.ReleaseMutant(mutant, 0); }, "mutant release requires an explicit owner");
  fault([&] { dispatcher.AbandonOwner(0); }, "null owner cannot abandon unowned mutants");
  fault([&] { dispatcher.ReleaseMutant(mutant, firstOwner); }, "unowned mutant cannot be released");
  fault([&] { dispatcher.SetEvent(mutant); }, "mutant is not an event");
  fault([&] { dispatcher.ResetEvent(mutant); }, "mutant cannot be reset as an event");
  fault([&] { dispatcher.ReleaseSemaphore(mutant, 1); }, "mutant is not a semaphore");
  fault([&] { dispatcher.ReleaseMutant(event, firstOwner); }, "event is not a mutant");
  check(poll(mutant, firstOwner) == Dispatcher::WaitResult::Signaled, "unowned mutant acquired");
  check(poll(mutant, firstOwner) == Dispatcher::WaitResult::Signaled, "owner recursively acquires mutant");
  check(poll(mutant, secondOwner) == Dispatcher::WaitResult::TimedOut, "different owner cannot acquire held mutant");
  fault([&] { dispatcher.ReleaseMutant(mutant, secondOwner); }, "nonowner cannot release mutant");
  check(dispatcher.ReleaseMutant(mutant, firstOwner) == -1, "recursive release reports previous signal");
  check(poll(mutant, secondOwner) == Dispatcher::WaitResult::TimedOut, "partial release preserves ownership");
  check(dispatcher.ReleaseMutant(mutant, firstOwner) == 0, "final release reports previous signal");
  check(poll(mutant, secondOwner) == Dispatcher::WaitResult::Signaled, "final release transfers availability");
  dispatcher.ReleaseMutant(mutant, secondOwner);
  {
    const auto owned = dispatcher.CreateMutant(firstOwner);
    check(poll(owned, secondOwner) == Dispatcher::WaitResult::TimedOut, "initial ownership is retained");
    dispatcher.ReleaseMutant(owned, firstOwner);
    Dispatcher::WaitResult result{};
    poll(owned, firstOwner);
    std::jthread waiter([&] { result = dispatcher.Wait(ctx, owned, 2s, secondOwner); });
    parked(dispatcher, 1);
    dispatcher.ReleaseMutant(owned, firstOwner);
    waiter.join();
    check(result == Dispatcher::WaitResult::Signaled, "independent final release wakes contender");
    dispatcher.ReleaseMutant(owned, secondOwner);
    dispatcher.Destroy(owned);
  }
  {
    const std::array<uint32_t, 3> objects{event, mutant, semaphore};
    const auto wait = [&](Dispatcher::WaitMode mode, uint32_t owner) {
      return dispatcher.WaitMultiple(ctx, objects, mode, 0ms, owner);
    };
    fault([&] { wait(Dispatcher::WaitMode::Any, 0); }, "any preflights owner even after ready event");
    fault([&] { wait(Dispatcher::WaitMode::All, 0); }, "all preflights owner before consumption");
    poll(mutant, firstOwner);
    check(wait(Dispatcher::WaitMode::All, secondOwner).result == Dispatcher::WaitResult::TimedOut, "all cannot consume another owner's mutant");
    check(poll(event, 0) == Dispatcher::WaitResult::Signaled && poll(semaphore, 0) == Dispatcher::WaitResult::Signaled, "incomplete all preserves other permits");
    dispatcher.SetEvent(event);
    dispatcher.ReleaseSemaphore(semaphore, 1);
    check(wait(Dispatcher::WaitMode::All, firstOwner).result == Dispatcher::WaitResult::Signaled, "all recursively acquires held mutant atomically");
    check(dispatcher.ReleaseMutant(mutant, firstOwner) == -1 && dispatcher.ReleaseMutant(mutant, firstOwner) == 0, "all increments recursion exactly once");
    check(poll(event, 0) == Dispatcher::WaitResult::TimedOut && poll(semaphore, 0) == Dispatcher::WaitResult::TimedOut, "successful all consumes other permits");
    poll(mutant, firstOwner);
    for (size_t n = 1; n < 65535; ++n) { poll(mutant, firstOwner); }
    dispatcher.SetEvent(event);
    dispatcher.ReleaseSemaphore(semaphore, 1);
    fault([&] { wait(Dispatcher::WaitMode::All, firstOwner); }, "recursion ceiling rejects entire ready all set");
    fault([&] { poll(mutant, firstOwner); }, "single recursion ceiling rejects without overflow");
    check(poll(event, 0) == Dispatcher::WaitResult::Signaled && poll(semaphore, 0) == Dispatcher::WaitResult::Signaled, "overflow preflight preserves earlier and later members");
    for (size_t n = 0; n < 65535; ++n) {
      check(dispatcher.ReleaseMutant(mutant, firstOwner) == int32_t(n) - 65534, "overflow did not add ownership depth");
    }
  }
  {
    const auto another = dispatcher.CreateMutant(firstOwner);
    poll(mutant, firstOwner);
    poll(mutant, firstOwner);
    const std::array<uint32_t, 3> objects{event, mutant, another};
    Dispatcher::WaitOutcome result{};
    std::jthread waiter([&] { result = dispatcher.WaitMultiple(ctx, objects, Dispatcher::WaitMode::All, 2s, secondOwner); });
    parked(dispatcher, 1);
    check(dispatcher.AbandonOwner(firstOwner) == 2 && dispatcher.AbandonOwner(firstOwner) == 0, "abandonment clears every recursive ownership once");
    check(poll(another, firstOwner) == Dispatcher::WaitResult::Abandoned, "incomplete all cannot consume abandoned flag");
    dispatcher.ReleaseMutant(another, firstOwner);
    dispatcher.SetEvent(event);
    waiter.join();
    check(result.result == Dispatcher::WaitResult::Abandoned && result.index == 1, "all consumes complete set and reports first abandoned index");
    check(poll(mutant, firstOwner) == Dispatcher::WaitResult::TimedOut && poll(another, firstOwner) == Dispatcher::WaitResult::TimedOut, "abandoned all grants ownership of every mutant");
    dispatcher.ReleaseMutant(mutant, secondOwner);
    dispatcher.ReleaseMutant(another, secondOwner);
    check(poll(mutant, firstOwner) == Dispatcher::WaitResult::Signaled, "abandoned flag consumed only once");
    dispatcher.ReleaseMutant(mutant, firstOwner);
    dispatcher.Destroy(another);
  }
  {
    poll(mutant, firstOwner);
    check(dispatcher.AbandonOwner(firstOwner) == 1, "explicit owner exit marks mutant abandoned");
    dispatcher.SetEvent(event);
    const std::array<uint32_t, 2> objects{event, mutant};
    const auto first = dispatcher.WaitMultiple(ctx, objects, Dispatcher::WaitMode::Any, 0ms, secondOwner);
    const auto second = dispatcher.WaitMultiple(ctx, objects, Dispatcher::WaitMode::Any, 0ms, secondOwner);
    check(first.result == Dispatcher::WaitResult::Signaled && first.index == 0, "any preserves input order before abandoned member");
    check(second.result == Dispatcher::WaitResult::Abandoned && second.index == 1, "any consumes and reports selected abandoned mutant");
    dispatcher.ReleaseMutant(mutant, secondOwner);
  }
  {
    const auto another = dispatcher.CreateMutant(firstOwner);
    poll(mutant, firstOwner);
    dispatcher.AbandonOwner(firstOwner);
    const std::array<uint32_t, 2> objects{another, mutant};
    const auto outcome = dispatcher.WaitMultiple(ctx, objects, Dispatcher::WaitMode::All, 0ms, secondOwner);
    check(outcome.result == Dispatcher::WaitResult::Abandoned && outcome.index == 0, "all reports lowest input index when multiple mutants are abandoned");
    check(poll(mutant, firstOwner) == Dispatcher::WaitResult::TimedOut && poll(another, firstOwner) == Dispatcher::WaitResult::TimedOut, "abandoned all acquires every member");
    dispatcher.ReleaseMutant(mutant, secondOwner);
    dispatcher.ReleaseMutant(another, secondOwner);
    check(poll(another, firstOwner) == Dispatcher::WaitResult::Signaled, "all clears every consumed abandoned flag");
    dispatcher.ReleaseMutant(another, firstOwner);
    dispatcher.Destroy(another);
  }
  {
    poll(mutant, firstOwner);
    std::array<Dispatcher::WaitResult, 2> results{};
    const auto wait = [&](size_t index, uint32_t owner) {
      results[index] = dispatcher.Wait(ctx, mutant, 2s, owner);
      dispatcher.ReleaseMutant(mutant, owner);
    };
    std::jthread first([&] { wait(0, secondOwner); });
    std::jthread second([&] { wait(1, 12); });
    parked(dispatcher, 2);
    check(dispatcher.AbandonOwner(firstOwner) == 1, "owner exit wakes contending mutant waiters");
    first.join(); second.join();
    check((results[0] == Dispatcher::WaitResult::Abandoned && results[1] == Dispatcher::WaitResult::Signaled) ||
      (results[1] == Dispatcher::WaitResult::Abandoned && results[0] == Dispatcher::WaitResult::Signaled), "only one contender consumes abandonment before final release handoff");
    check(dispatcher.AbandonOwner(secondOwner) == 0 && dispatcher.AbandonOwner(12) == 0, "contenders released all ownership");
  }
  {
    poll(mutant, firstOwner);
    Dispatcher::WaitResult result{};
    std::jthread waiter([&] { result = dispatcher.Wait(ctx, mutant, 2s, secondOwner); });
    parked(dispatcher, 1);
    dispatcher.Destroy(mutant);
    const auto replacement = dispatcher.CreateMutant();
    waiter.join();
    check(result == Dispatcher::WaitResult::Destroyed && dispatcher.AbandonOwner(firstOwner) == 0, "retirement does not abandon a replacement");
    check(poll(replacement, secondOwner) == Dispatcher::WaitResult::Signaled, "replacement starts independently unowned");
    dispatcher.Destroy(replacement);
  }
  dispatcher.Destroy(event);
  dispatcher.Destroy(semaphore);
  {
    Runtime stopping({}, {});
    Dispatcher owned(stopping);
    const auto held = owned.CreateMutant(firstOwner);
    browser::Memory memory;
    ownedDispatcher = &owned;
    ownedObject = held;
    ownedOwner = secondOwner;
    ownedCancelled = false;
    stopping.Launch({}, memory.Base(), ownedWait);
    parked(owned, 1);
    check(!stopping.Stop() && ownedCancelled && stopping.ActiveWorkers() == 0 && owned.ActiveWaiters() == 0, "Stop cancels and joins owned mutant waiter");
    ownedOwner = 0;
    fault([&] { owned.ReleaseMutant(held, firstOwner); }, "cancelled runtime rejects ownership mutation");
    fault([&] { owned.CreateMutant(); }, "cancelled runtime rejects mutant allocation");
    check(owned.AbandonOwner(firstOwner) == 1, "owner cleanup remains available after cancellation");
    owned.Destroy(held);
    check(owned.ActiveObjects() == 0, "cancelled mutant cleanup retires all state");
  }
  check(dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0, "mutant contracts leave no live state");
}

void timerContracts(Runtime& runtime, Dispatcher& dispatcher, const X86Context& ctx) {
  const auto notification = dispatcher.CreateTimer(true);
  const auto synchronization = dispatcher.CreateTimer(false);
  const auto event = dispatcher.CreateEvent(false, true);
  const auto poll = [&](uint32_t object) { return dispatcher.Wait(ctx, object, 0ms); };
  check(dispatcher.TimerTime() == 0, "timer clock starts at zero independently of host time");
  check(poll(notification) == Dispatcher::WaitResult::TimedOut, "new timer is unsignaled and unarmed");
  fault([&] { dispatcher.SetTimer(event, 1); }, "event cannot be armed as timer");
  fault([&] { dispatcher.CancelTimer(event); }, "event cannot be cancelled as timer");
  fault([&] { dispatcher.SetEvent(notification); }, "timer cannot be set as event");
  fault([&] { dispatcher.ResetEvent(notification); }, "timer cannot be reset as event");
  fault([&] { dispatcher.ReleaseSemaphore(notification, 1); }, "timer cannot be released as semaphore");
  check(!dispatcher.CancelTimer(notification), "unarmed cancellation returns false");
  check(!dispatcher.SetTimer(notification, 10), "first arm returns false");
  dispatcher.AdvanceTime(9);
  check(poll(notification) == Dispatcher::WaitResult::TimedOut, "timer cannot fire before virtual deadline");
  dispatcher.SetTimerPaused(true);
  dispatcher.AdvanceTime(UINT64_MAX);
  check(dispatcher.TimerTime() == 9 && poll(notification) == Dispatcher::WaitResult::TimedOut, "paused time neither advances nor accumulates catchup");
  dispatcher.SetTimerPaused(false);
  dispatcher.AdvanceTime(1);
  check(poll(notification) == Dispatcher::WaitResult::Signaled && poll(notification) == Dispatcher::WaitResult::Signaled, "notification timer persists after expiry");
  check(!dispatcher.CancelTimer(notification) && poll(notification) == Dispatcher::WaitResult::Signaled, "cancellation preserves an expired notification signal");
  check(!dispatcher.SetTimer(notification, 20), "expired timer is no longer pending");
  check(poll(notification) == Dispatcher::WaitResult::TimedOut, "rearm clears old signal");
  check(dispatcher.SetTimer(notification, 30), "replacing a pending timer returns true");
  dispatcher.AdvanceTime(20);
  check(poll(notification) == Dispatcher::WaitResult::TimedOut, "replaced deadline cannot fire");
  check(dispatcher.CancelTimer(notification) && !dispatcher.CancelTimer(notification), "cancellation removes exactly one pending schedule");
  dispatcher.AdvanceTime(10);
  check(poll(notification) == Dispatcher::WaitResult::TimedOut, "cancelled deadline cannot fire");
  dispatcher.SetTimer(synchronization, 0);
  check(poll(synchronization) == Dispatcher::WaitResult::Signaled && poll(synchronization) == Dispatcher::WaitResult::TimedOut, "zero delay fires immediately and synchronization timer consumes once");
  dispatcher.SetTimer(synchronization, 3, 5);
  dispatcher.AdvanceTime(102);
  check(poll(synchronization) == Dispatcher::WaitResult::Signaled && poll(synchronization) == Dispatcher::WaitResult::TimedOut, "missed periodic firings coalesce to one signal");
  dispatcher.AdvanceTime(1);
  check(poll(synchronization) == Dispatcher::WaitResult::Signaled, "periodic phase remains anchored to original deadline");
  dispatcher.AdvanceTime(4);
  check(poll(synchronization) == Dispatcher::WaitResult::TimedOut, "periodic timer does not drift or fire early");
  dispatcher.AdvanceTime(1);
  check(dispatcher.CancelTimer(synchronization) && poll(synchronization) == Dispatcher::WaitResult::Signaled, "cancelling periodic timer preserves already published signal");
  dispatcher.AdvanceTime(100);
  check(poll(synchronization) == Dispatcher::WaitResult::TimedOut, "cancelled periodic timer cannot republish");
  {
    const std::array<uint32_t, 3> objects{event, notification, synchronization};
    dispatcher.SetTimer(notification, 2);
    dispatcher.SetTimer(synchronization, 4);
    dispatcher.AdvanceTime(2);
    check(dispatcher.WaitMultiple(ctx, objects, Dispatcher::WaitMode::All, 0ms).result == Dispatcher::WaitResult::TimedOut, "incomplete timer all wait preserves signals");
    check(poll(event) == Dispatcher::WaitResult::Signaled && poll(notification) == Dispatcher::WaitResult::Signaled, "incomplete all does not consume event or notification timer");
    dispatcher.SetEvent(event);
    Dispatcher::WaitOutcome result{};
    std::jthread waiter([&] { result = dispatcher.WaitMultiple(ctx, objects, Dispatcher::WaitMode::All, 2s); });
    parked(dispatcher, 1);
    dispatcher.AdvanceTime(2);
    waiter.join();
    check(result.result == Dispatcher::WaitResult::Signaled && !result.index, "independent timer pump wakes atomic all waiter");
    const auto outcome = dispatcher.WaitMultiple(ctx, objects, Dispatcher::WaitMode::Any, 0ms);
    check(outcome.result == Dispatcher::WaitResult::Signaled && outcome.index == 1, "any preserves notification timer and consumes synchronization timer once");
  }
  {
    dispatcher.SetTimer(notification, 1);
    dispatcher.SetTimer(synchronization, 1);
    const std::array<uint32_t, 2> objects{notification, synchronization};
    Dispatcher::WaitOutcome result{};
    std::jthread waiter([&] { result = dispatcher.WaitMultiple(ctx, objects, Dispatcher::WaitMode::All, 2s); });
    parked(dispatcher, 1);
    dispatcher.AdvanceTime(1);
    waiter.join();
    check(result.result == Dispatcher::WaitResult::Signaled && poll(synchronization) == Dispatcher::WaitResult::TimedOut, "one clock advance publishes equal deadlines before atomic all consumption");
  }
  {
    dispatcher.SetTimer(notification, 1);
    std::array<Dispatcher::WaitResult, 2> results{};
    std::jthread first([&] { results[0] = dispatcher.Wait(ctx, notification, 2s); });
    std::jthread second([&] { results[1] = dispatcher.Wait(ctx, notification, 2s); });
    parked(dispatcher, 2);
    dispatcher.AdvanceTime(1);
    first.join(); second.join();
    check(results[0] == Dispatcher::WaitResult::Signaled && results[1] == Dispatcher::WaitResult::Signaled, "notification timer expiry broadcasts to both waiters");
  }
  {
    dispatcher.SetTimer(synchronization, 1);
    std::atomic<size_t> signaled = 0;
    const auto wait = [&] {
      if (dispatcher.Wait(ctx, synchronization, 2s) == Dispatcher::WaitResult::Signaled) { ++signaled; }
    };
    std::jthread first(wait);
    std::jthread second(wait);
    parked(dispatcher, 2);
    check(dispatcher.CancelTimer(synchronization), "pending cancellation does not retire waiters or fabricate a signal");
    check(dispatcher.ActiveWaiters() == 2 && signaled == 0, "cancelling unsignaled timer preserves pending waits");
    dispatcher.SetTimer(synchronization, 0);
    parked(dispatcher, 1);
    dispatcher.SetTimer(synchronization, 0);
    first.join(); second.join();
    check(signaled == 2 && poll(synchronization) == Dispatcher::WaitResult::TimedOut, "each immediate synchronization expiry releases exactly one contender");
  }
  {
    dispatcher.SetTimerPaused(true);
    dispatcher.SetTimer(synchronization, 1);
    const auto start = std::chrono::steady_clock::now();
    check(dispatcher.Wait(ctx, synchronization, 15ms) == Dispatcher::WaitResult::TimedOut, "paused guest timer does not disable bounded host wait budget");
    check(std::chrono::steady_clock::now() - start >= 15ms, "host watchdog budget is not guest time");
    dispatcher.SetTimerPaused(false);
    dispatcher.AdvanceTime(1);
    check(poll(synchronization) == Dispatcher::WaitResult::Signaled, "resume allows subsequent supplied time to expire timer");
  }
  {
    Dispatcher::WaitResult result{};
    dispatcher.SetTimer(synchronization, 1, 1);
    std::jthread waiter([&] { result = dispatcher.Wait(ctx, synchronization, 2s); });
    parked(dispatcher, 1);
    dispatcher.Destroy(synchronization);
    const auto replacement = dispatcher.CreateTimer(false);
    dispatcher.AdvanceTime(1);
    waiter.join();
    check(result == Dispatcher::WaitResult::Destroyed && poll(replacement) == Dispatcher::WaitResult::TimedOut, "retired timer cannot fire a replacement");
    dispatcher.Destroy(replacement);
  }
  dispatcher.Destroy(notification);
  dispatcher.Destroy(event);
  {
    Runtime bounded({}, {});
    Dispatcher timers(bounded);
    const auto timer = timers.CreateTimer(false);
    timers.AdvanceTime(UINT64_MAX - 10);
    timers.SetTimer(timer, 5, 3);
    fault([&] { timers.SetTimer(timer, 11); }, "overflowing rearm rejects before changing existing schedule");
    fault([&] { timers.AdvanceTime(11); }, "clock overflow rejects before publishing time or signals");
    check(timers.TimerTime() == UINT64_MAX - 10 && timers.Wait(ctx, timer, 0ms) == Dispatcher::WaitResult::TimedOut, "rejected clock operations preserve time and signal");
    timers.AdvanceTime(5);
    check(timers.Wait(ctx, timer, 0ms) == Dispatcher::WaitResult::Signaled, "rejected rearm preserves original schedule");
    timers.AdvanceTime(5);
    check(timers.Wait(ctx, timer, 0ms) == Dispatcher::WaitResult::Signaled && !timers.CancelTimer(timer), "final representable periodic expiry signals then disarms without wrap");
    timers.SetTimer(timer, 0, UINT64_MAX);
    check(timers.Wait(ctx, timer, 0ms) == Dispatcher::WaitResult::Signaled && !timers.CancelTimer(timer), "immediate periodic expiry at clock ceiling cannot wrap");
    fault([&] { timers.SetTimer(timer, 1); }, "deadline beyond clock ceiling rejects");
    timers.Destroy(timer);
    std::array<uint32_t, 128> objects{};
    for (size_t n = 0; n < objects.size(); ++n) { objects[n] = timers.CreateTimer(false); }
    fault([&] { timers.CreateTimer(true); }, "timers share dispatcher object budget");
    for (const auto object : objects) { timers.Destroy(object); }
  }
  {
    Runtime stopping({}, {});
    Dispatcher timers(stopping);
    browser::Memory memory;
    ownedDispatcher = &timers;
    ownedObject = timers.CreateTimer(false);
    timers.SetTimer(ownedObject, 1, 1);
    ownedCancelled = false;
    stopping.Launch({}, memory.Base(), ownedWait);
    parked(timers, 1);
    check(!stopping.Stop() && ownedCancelled && stopping.ActiveWorkers() == 0 && timers.ActiveWaiters() == 0, "Stop cancels and joins pending timer waiter without a timer thread");
    fault([&] { timers.CreateTimer(true); }, "cancelled runtime rejects timer creation");
    fault([&] { timers.SetTimer(ownedObject, 0); }, "cancelled runtime rejects timer rearm");
    fault([&] { timers.AdvanceTime(1); }, "cancelled runtime rejects timer clock advancement");
    fault([&] { timers.SetTimerPaused(true); }, "cancelled runtime rejects pause mutation");
    check(timers.CancelTimer(ownedObject), "timer cancellation remains available for shutdown cleanup");
    timers.Destroy(ownedObject);
    check(timers.ActiveObjects() == 0, "timer shutdown leaves no live objects");
  }
  check(dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0 && runtime.ActiveWorkers() == 0, "timer contracts leave no live objects, waiters or workers");
}

void guestWaitContracts(const X86Context& ctx) {
  Runtime runtime({}, {});
  constexpr uint64_t epoch = 116444736000000000ull;
  Dispatcher dispatcher(runtime, epoch);
  const auto event = dispatcher.CreateEvent(false, false);
  const auto timer = dispatcher.CreateTimer(false);
  const auto poll = [&](uint32_t object, std::optional<int64_t> timeout = 0) {
    return dispatcher.WaitGuest(ctx, object, timeout, 20ms);
  };
  check(poll(event) == Dispatcher::WaitResult::TimedOut, "zero guest timeout polls without an epoch conversion");
  check(poll(event, int64_t(epoch - 1)) == Dispatcher::WaitResult::TimedOut, "past absolute guest deadline expires");
  check(poll(event, int64_t(epoch)) == Dispatcher::WaitResult::TimedOut, "current absolute guest deadline expires");
  dispatcher.SetEvent(event);
  check(poll(event) == Dispatcher::WaitResult::Signaled, "ready predicate wins over a zero guest timeout");
  {
    Dispatcher unanchored(runtime);
    const auto ready = unanchored.CreateEvent(false, true);
    fault([&] { unanchored.WaitGuest(ctx, ready, 1, 20ms); }, "absolute wait requires an explicit system time origin before consumption");
    check(unanchored.WaitGuest(ctx, ready, 0, 20ms) == Dispatcher::WaitResult::Signaled, "missing epoch rejection preserves signal");
    unanchored.Destroy(ready);
  }
  for (const auto timeout : {std::optional<int64_t>{}, std::optional<int64_t>{INT64_MIN}, std::optional<int64_t>{INT64_MAX}}) {
    const auto start = std::chrono::steady_clock::now();
    check(poll(event, timeout) == Dispatcher::WaitResult::WatchdogExpired, "indefinite and extreme finite waits never substitute host time for guest time");
    check(std::chrono::steady_clock::now() - start >= 20ms, "guest wait honors independent host watchdog");
  }
  for (const auto timeout : {-int64_t(10), int64_t(epoch + 20)}) {
    const uint64_t target = timeout < 0 ? dispatcher.TimerTime() + 10 : 20;
    Dispatcher::WaitResult result{};
    std::atomic<bool> done = false;
    std::jthread waiter([&] { result = dispatcher.WaitGuest(ctx, event, timeout, 2s); done = true; });
    parked(dispatcher, 1);
    runtime.Wake();
    dispatcher.AdvanceTime(target - dispatcher.TimerTime() - 1);
    check(!done, "unrelated wakes and time before deadline cannot expire guest wait");
    dispatcher.AdvanceTime(1);
    waiter.join();
    check(result == Dispatcher::WaitResult::TimedOut, "relative and absolute guest deadlines wake without any armed timer");
  }
  {
    dispatcher.SetTimerPaused(true);
    std::atomic<bool> done = false;
    Dispatcher::WaitResult result{};
    std::jthread waiter([&] { result = dispatcher.WaitGuest(ctx, event, -1, 2s); done = true; });
    parked(dispatcher, 1);
    dispatcher.AdvanceTime(UINT64_MAX);
    runtime.Wake();
    std::this_thread::sleep_for(5ms);
    check(!done && dispatcher.TimerTime() == 20, "paused clock cannot expire a relative wait or accumulate catchup");
    dispatcher.SetTimerPaused(false);
    dispatcher.AdvanceTime(1);
    waiter.join();
    check(result == Dispatcher::WaitResult::TimedOut, "resumed guest clock expires relative wait");
    dispatcher.SetTimerPaused(true);
    check(poll(event, -1) == Dispatcher::WaitResult::WatchdogExpired, "host watchdog remains active while guest time is paused");
    dispatcher.SetTimerPaused(false);
  }
  {
    dispatcher.SetEvent(event);
    dispatcher.SetTimer(timer, 5);
    const std::array<uint32_t, 2> objects{event, timer};
    Dispatcher::WaitOutcome outcome{};
    std::jthread waiter([&] { outcome = dispatcher.WaitMultipleGuest(ctx, objects, Dispatcher::WaitMode::All, -5, 2s); });
    parked(dispatcher, 1);
    dispatcher.AdvanceTime(5);
    waiter.join();
    check(outcome.result == Dispatcher::WaitResult::Signaled && !outcome.index, "timer publication wins over coincident guest expiry and all consumes atomically");
    dispatcher.SetEvent(event);
    std::jthread timeout([&] { outcome = dispatcher.WaitMultipleGuest(ctx, objects, Dispatcher::WaitMode::All, -2, 2s); });
    parked(dispatcher, 1);
    dispatcher.AdvanceTime(2);
    timeout.join();
    check(outcome.result == Dispatcher::WaitResult::TimedOut && !outcome.index, "guest all timeout has no selected index");
    check(poll(event) == Dispatcher::WaitResult::Signaled, "guest all timeout preserves partial signals");
    dispatcher.SetEvent(event);
    check(dispatcher.WaitMultipleGuest(ctx, objects, Dispatcher::WaitMode::All, std::nullopt, 10ms).result == Dispatcher::WaitResult::WatchdogExpired, "all watchdog is distinct from guest timeout");
    const auto any = dispatcher.WaitMultipleGuest(ctx, objects, Dispatcher::WaitMode::Any, 0, 20ms);
    check(any.result == Dispatcher::WaitResult::Signaled && any.index == 0, "watchdog preserves partial signals for later any wait");
  }
  {
    const auto retired = dispatcher.CreateEvent(false, false);
    Dispatcher::WaitResult result{};
    std::jthread waiter([&] { result = dispatcher.WaitGuest(ctx, retired, std::nullopt, 2s); });
    parked(dispatcher, 1);
    dispatcher.Destroy(retired);
    waiter.join();
    check(result == Dispatcher::WaitResult::Destroyed, "indefinite guest wait retains retirement semantics");
  }
  dispatcher.SetEvent(event);
  for (const auto budget : {0ms, -1ms, 30001ms, std::chrono::milliseconds::max()}) {
    fault([&] { dispatcher.WaitGuest(ctx, event, 0, budget); }, "guest wait requires a positive bounded host watchdog");
  }
  {
    browser::Memory memory;
    auto lease = memory.Borrow();
    fault([&] { poll(event); }, "guest poll rejects backing leases");
  }
  check(poll(event) == Dispatcher::WaitResult::Signaled && dispatcher.ActiveWaiters() == 0, "invalid guest waits preserve signals and admission");
  dispatcher.Destroy(event);
  dispatcher.Destroy(timer);
  {
    Runtime bounded({}, {});
    Dispatcher clock(bounded, UINT64_MAX);
    const auto signal = clock.CreateEvent(false, true);
    clock.AdvanceTime(UINT64_MAX - 2);
    fault([&] { clock.WaitGuest(ctx, signal, -3, 20ms); }, "relative deadline overflow rejects before consuming");
    check(clock.WaitGuest(ctx, signal, -2, 20ms) == Dispatcher::WaitResult::Signaled, "largest representable relative deadline is accepted");
    check(clock.WaitGuest(ctx, signal, INT64_MAX, 20ms) == Dispatcher::WaitResult::TimedOut, "absolute comparison never overflows epoch plus elapsed time");
    clock.Destroy(signal);
  }
  {
    Runtime extremes({}, {});
    Dispatcher clock(extremes, 0);
    const auto signal = clock.CreateEvent(false, false);
    std::array<Dispatcher::WaitResult, 2> outcomes{};
    std::jthread relative([&] { outcomes[0] = clock.WaitGuest(ctx, signal, INT64_MIN, 2s); });
    parked(clock, 1);
    std::jthread absolute([&] { outcomes[1] = clock.WaitGuest(ctx, signal, INT64_MAX, 2s); });
    parked(clock, 2);
    clock.AdvanceTime(uint64_t(INT64_MAX));
    absolute.join();
    check(outcomes[1] == Dispatcher::WaitResult::TimedOut, "INT64_MAX is a finite absolute timeout rather than an indefinite sentinel");
    clock.AdvanceTime(1);
    relative.join();
    check(outcomes[0] == Dispatcher::WaitResult::TimedOut, "INT64_MIN relative magnitude expires exactly without signed negation overflow");
    clock.Destroy(signal);
  }
  {
    Runtime stopping({}, {});
    Dispatcher owned(stopping);
    browser::Memory memory;
    ownedDispatcher = &owned;
    ownedObject = owned.CreateEvent(false, false);
    ownedGuestWait = true;
    ownedCancelled = false;
    stopping.Launch({}, memory.Base(), ownedWait);
    parked(owned, 1);
    check(!stopping.Stop() && ownedCancelled && stopping.ActiveWorkers() == 0 && owned.ActiveWaiters() == 0, "Stop cancels and joins indefinite guest wait without clock progress");
    ownedGuestWait = false;
    owned.Destroy(ownedObject);
  }
  check(dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0, "guest waits leave no live state");
}

int main() {
  try {
    Runtime runtime({}, {});
    Dispatcher dispatcher(runtime);
    X86Context ctx{};
    const auto poll = [&](uint32_t object) { return dispatcher.Wait(ctx, object, 0ms); };
    const auto manual = dispatcher.CreateEvent(true, false);
    check(poll(manual) == Dispatcher::WaitResult::TimedOut, "unsignaled event polls without consuming");
    check(dispatcher.SetEvent(manual) == 0 && dispatcher.SetEvent(manual) == 1, "set returns prior signal state");
    check(poll(manual) == Dispatcher::WaitResult::Signaled && poll(manual) == Dispatcher::WaitResult::Signaled, "notification event remains signaled");
    check(dispatcher.ResetEvent(manual) == 1 && dispatcher.ResetEvent(manual) == 0, "reset returns prior state");
    const auto automatic = dispatcher.CreateEvent(false, true);
    check(poll(automatic) == Dispatcher::WaitResult::Signaled && poll(automatic) == Dispatcher::WaitResult::TimedOut, "synchronization event consumes once");
    const auto semaphore = dispatcher.CreateSemaphore(1, 3);
    check(poll(semaphore) == Dispatcher::WaitResult::Signaled && poll(semaphore) == Dispatcher::WaitResult::TimedOut, "semaphore consumes one count");
    check(dispatcher.ReleaseSemaphore(semaphore, 2) == 0, "release returns old count");
    fault([&] { dispatcher.ReleaseSemaphore(semaphore, 2); }, "overflow rejects without modifying count");
    fault([&] { dispatcher.ReleaseSemaphore(semaphore, INT32_MAX); }, "signed addition cannot overflow");
    fault([&] { dispatcher.ReleaseSemaphore(semaphore, 0); }, "zero release rejected");
    fault([&] { dispatcher.ReleaseSemaphore(semaphore, -1); }, "negative release rejected");
    check(poll(semaphore) == Dispatcher::WaitResult::Signaled && poll(semaphore) == Dispatcher::WaitResult::Signaled && poll(semaphore) == Dispatcher::WaitResult::TimedOut, "rejected releases preserve count");
    fault([&] { dispatcher.SetEvent(semaphore); }, "event operations reject semaphore");
    fault([&] { dispatcher.ResetEvent(semaphore); }, "reset rejects semaphore");
    fault([&] { dispatcher.ReleaseSemaphore(manual, 1); }, "release rejects event");
    fault([&] { dispatcher.CreateSemaphore(-1, 1); }, "negative initial count rejected");
    fault([&] { dispatcher.CreateSemaphore(2, 1); }, "initial count bounded by limit");
    fault([&] { dispatcher.CreateSemaphore(0, 0); }, "positive limit required");
    fault([&] { dispatcher.CreateSemaphore(0, -1); }, "negative limit rejected");
    fault([&] { dispatcher.Wait(ctx, manual, -1ms); }, "negative host budget rejected");
    fault([&] { dispatcher.Wait(ctx, manual, 30001ms); }, "unbounded host budget rejected");
    fault([&] { dispatcher.Wait(ctx, manual, std::chrono::milliseconds::max()); }, "extreme duration rejected before deadline arithmetic");
    fault([&] { dispatcher.Wait(ctx, 0, 0ms); }, "null object rejected");
    fault([&] { dispatcher.Wait(ctx, UINT32_MAX, 0ms); }, "unknown object rejected");
    const auto start = std::chrono::steady_clock::now();
    check(dispatcher.Wait(ctx, manual, 15ms) == Dispatcher::WaitResult::TimedOut, "finite wait expires");
    check(std::chrono::steady_clock::now() - start >= 15ms, "timeout cannot complete early");
    {
      std::array<Dispatcher::WaitResult, 2> results{};
      std::jthread first([&] { results[0] = dispatcher.Wait(ctx, manual, 2s); });
      std::jthread second([&] { results[1] = dispatcher.Wait(ctx, manual, 2s); });
      parked(dispatcher, 2);
      fault([&] { dispatcher.Wait(ctx, automatic, 1ms); }, "waiter budget enforced");
      dispatcher.SetEvent(manual);
      first.join(); second.join();
      check(results[0] == Dispatcher::WaitResult::Signaled && results[1] == Dispatcher::WaitResult::Signaled, "manual event releases both waiters");
      check(poll(manual) == Dispatcher::WaitResult::Signaled, "manual event remains signaled after contention");
      dispatcher.ResetEvent(manual);
    }
    {
      const auto counting = dispatcher.CreateSemaphore(0, 2);
      std::array<Dispatcher::WaitResult, 2> results{};
      std::jthread first([&] { results[0] = dispatcher.Wait(ctx, counting, 2s); });
      std::jthread second([&] { results[1] = dispatcher.Wait(ctx, counting, 2s); });
      parked(dispatcher, 2);
      dispatcher.ReleaseSemaphore(counting, 2);
      first.join(); second.join();
      check(results[0] == Dispatcher::WaitResult::Signaled && results[1] == Dispatcher::WaitResult::Signaled && poll(counting) == Dispatcher::WaitResult::TimedOut, "two semaphore permits consumed exactly once");
      dispatcher.Destroy(counting);
    }
    {
      std::atomic<size_t> signaled = 0;
      const auto wait = [&] { if (dispatcher.Wait(ctx, automatic, 2s) == Dispatcher::WaitResult::Signaled) { ++signaled; } };
      std::jthread first(wait);
      std::jthread second(wait);
      parked(dispatcher, 2);
      dispatcher.SetEvent(automatic);
      parked(dispatcher, 1);
      check(signaled.load() <= 1, "one auto event set releases at most one waiter");
      dispatcher.Destroy(automatic);
      first.join(); second.join();
      check(signaled == 1, "destroy wakes remaining auto event waiter without inventing a signal");
      fault([&] { dispatcher.SetEvent(automatic); }, "stale object cannot be signaled");
      fault([&] { dispatcher.Destroy(automatic); }, "double destroy rejected");
    }
    {
      const auto retired = dispatcher.CreateEvent(false, false);
      Dispatcher::WaitResult result{};
      std::jthread waiter([&] { result = dispatcher.Wait(ctx, retired, 2s); });
      parked(dispatcher, 1);
      dispatcher.Destroy(retired);
      const auto replacement = dispatcher.CreateEvent(false, true);
      check(replacement != retired, "replacement object has a fresh identifier");
      waiter.join();
      check(result == Dispatcher::WaitResult::Destroyed && poll(replacement) == Dispatcher::WaitResult::Signaled, "retired waiter cannot consume replacement state");
      dispatcher.Destroy(replacement);
    }
    {
      std::atomic<bool> done = false;
      std::jthread unrelated([&] {
        while (!done.load()) { runtime.Wake(); std::this_thread::sleep_for(1ms); }
      });
      const auto start = std::chrono::steady_clock::now();
      const auto result = dispatcher.Wait(ctx, manual, 20ms);
      done = true;
      unrelated.join();
      check(result == Dispatcher::WaitResult::TimedOut && std::chrono::steady_clock::now() - start < 1s, "unrelated wakes neither satisfy nor renew the deadline");
    }
    {
      const auto request = dispatcher.CreateSemaphore(0, 1);
      const auto response = dispatcher.CreateSemaphore(0, 1);
      std::atomic<size_t> completed = 0;
      std::jthread responder([&] {
        for (size_t n = 0; n < 200; ++n) {
          check(dispatcher.Wait(ctx, request, 2s) == Dispatcher::WaitResult::Signaled, "racing request handoff does not lose a wake");
          ++completed;
          dispatcher.ReleaseSemaphore(response, 1);
        }
      });
      for (size_t n = 0; n < 200; ++n) {
        dispatcher.ReleaseSemaphore(request, 1);
        check(dispatcher.Wait(ctx, response, 2s) == Dispatcher::WaitResult::Signaled && completed == n + 1, "response is published before completion is consumed");
      }
      responder.join();
      dispatcher.Destroy(request);
      dispatcher.Destroy(response);
    }
    {
      const auto large = dispatcher.CreateSemaphore(INT32_MAX, INT32_MAX);
      check(poll(large) == Dispatcher::WaitResult::Signaled && dispatcher.ReleaseSemaphore(large, 1) == INT32_MAX - 1, "maximum signed semaphore count remains exact");
      fault([&] { dispatcher.ReleaseSemaphore(large, 1); }, "maximum signed count cannot overflow");
      dispatcher.Destroy(large);
    }
    {
      browser::Memory memory;
      auto lease = memory.Borrow();
      fault([&] { dispatcher.Wait(ctx, manual, 0ms); }, "even polling under a backing lease is rejected");
    }
    dispatcher.Destroy(manual);
    dispatcher.Destroy(semaphore);
    check(dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0, "completed operations leave no live state");
    {
      std::array<uint32_t, 128> objects{};
      for (size_t n = 0; n < objects.size(); ++n) { objects[n] = dispatcher.CreateEvent(false, false); }
      fault([&] { dispatcher.CreateEvent(false, false); }, "live object ceiling enforced");
      for (const auto object : objects) { dispatcher.Destroy(object); }
      for (size_t n = 0; n < 1000; ++n) {
        const auto object = dispatcher.CreateEvent(false, true);
        check(object > objects.back(), "retired identifiers never reused");
        dispatcher.Destroy(object);
      }
      check(dispatcher.ActiveObjects() == 0, "repeated creation and retirement remains bounded");
    }
    {
      Runtime stopping({}, {});
      Dispatcher owned(stopping);
      browser::Memory memory;
      ownedDispatcher = &owned;
      ownedObject = owned.CreateEvent(false, false);
      stopping.Launch({}, memory.Base(), ownedWait);
      parked(owned, 1);
      check(!stopping.Stop() && ownedCancelled, "runtime Stop cancels and joins an object waiter");
      check(owned.ActiveWaiters() == 0 && stopping.ActiveWorkers() == 0, "cancellation retires waiter bookkeeping");
      fault([&] { owned.SetEvent(ownedObject); }, "cancelled runtime rejects signaling");
      fault([&] { owned.CreateEvent(false, false); }, "cancelled runtime rejects allocation");
      fault([&] { owned.Wait(ctx, ownedObject, 0ms); }, "cancelled runtime rejects consumption");
      owned.Destroy(ownedObject);
      check(owned.ActiveObjects() == 0, "cleanup remains available after cancellation");
    }
    multipleContracts(runtime, dispatcher, ctx);
    mutantContracts(dispatcher, ctx);
    timerContracts(runtime, dispatcher, ctx);
    guestWaitContracts(ctx);
    for (const auto mode : {Dispatcher::WaitMode::Any, Dispatcher::WaitMode::All}) {
      Runtime stopping({}, {});
      Dispatcher owned(stopping);
      browser::Memory memory;
      ownedDispatcher = &owned;
      ownedObjects = {owned.CreateEvent(false, false), owned.CreateSemaphore(0, 1)};
      ownedMode = mode;
      ownedCancelled = false;
      stopping.Launch({}, memory.Base(), ownedWait);
      parked(owned, 1);
      check(!stopping.Stop() && ownedCancelled, "Stop cancels and joins an owned multiple waiter");
      check(owned.ActiveWaiters() == 0 && stopping.ActiveWorkers() == 0, "cancelled multiple wait releases admission");
      fault([&] { owned.WaitMultiple(ctx, ownedObjects, mode, 0ms); }, "cancelled runtime rejects multiple polls");
      for (const auto object : ownedObjects) { owned.Destroy(object); }
      check(owned.ActiveObjects() == 0, "multiple wait retirement remains available after cancellation");
      ownedMode.reset();
    }
    std::printf("dispatcher-contracts-ok\n");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
