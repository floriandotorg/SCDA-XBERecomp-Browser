#include "browser/dispatcher-handles.h"
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
  Memory memory;
  X86Context ctx{};
  memory.Map(0x10000, 0x1000);
  memory.Commit(0x10000, 0x1000);
  const uint64_t zero = 0;
  memory.Write(ctx, 0x10000, &zero, sizeof(zero));
  DispatcherHandles handles(dispatcher);
  const auto wait = [&](std::span<const uint32_t> set, Dispatcher::WaitMode mode = Dispatcher::WaitMode::Any, uint32_t timeout = 0x10000, std::chrono::milliseconds watchdog = 2s) {
    return handles.WaitGuestStatus(memory, ctx, set, mode, timeout, false, watchdog);
  };
  {
    const auto event = handles.CreateEvent(false, false);
    const auto alias = handles.Duplicate(event);
    check(event != alias && handles.ActiveHandles() == 2 && dispatcher.ActiveObjects() == 1, "duplicate shares one object under distinct handles");
    check(handles.SetEvent(alias) == 0, "alias signals original event");
    check(wait(std::span(&event, 1)) == 0 && wait(std::span(&alias, 1)) == 0x102, "aliases share single consumption");
    handles.SetEvent(event);
    check(handles.ResetEvent(alias) == 1 && wait(std::span(&event, 1)) == 0x102, "reset through alias clears original signal");
    handles.SetEvent(event);
    const std::array<uint32_t, 2> duplicate{event, alias};
    fault([&] { wait(duplicate); }, "aliased any set rejects duplicate underlying object");
    fault([&] { wait(duplicate, Dispatcher::WaitMode::All); }, "aliased all set rejects without partial consumption");
    check(wait(std::span(&alias, 1)) == 0, "alias preflight preserves signal");
    handles.Close(event);
    fault([&] { handles.SetEvent(event); }, "closed identifier cannot signal through its surviving alias");
    fault([&] { handles.Duplicate(event); }, "closed identifier cannot duplicate");
    fault([&] { handles.Close(event); }, "double close rejects");
    handles.Close(alias);
    check(dispatcher.ActiveObjects() == 0, "last reference retires the object");
  }
  for (size_t n = 0; n < 100; ++n) {
    const auto event = handles.CreateEvent(false, false);
    const auto alias = handles.Duplicate(event);
    std::array<uint32_t, 1> set{event};
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(set, Dispatcher::WaitMode::Any, 0); });
    parked(dispatcher);
    handles.Close(event);
    const auto replacement = handles.CreateEvent(false, true);
    set[0] = replacement;
    runtime.Wake();
    check(dispatcher.ActiveWaiters() == 1, "replacement cannot satisfy snapshotted original wait");
    handles.SetEvent(alias);
    handles.Close(alias);
    waiter.join();
    check(status == 0 && dispatcher.ActiveObjects() == 1, "close preserves pending operation and final completion releases its pin");
    check(wait(std::span(&replacement, 1)) == 0, "replacement signal was not consumed by old wait");
    handles.Close(replacement);
  }
  {
    const auto first = handles.CreateEvent(false, true);
    const auto second = handles.CreateSemaphore(0, 1);
    const std::array<uint32_t, 2> set{first, second};
    bool watchdog = false;
    std::jthread waiter([&] {
      try { wait(set, Dispatcher::WaitMode::All, 0, 100ms); }
      catch (const browser::WaitWatchdogExpired&) { watchdog = true; }
    });
    parked(dispatcher);
    handles.Close(first);
    handles.Close(second);
    check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 2, "pending all wait pins every closed member without retirement");
    waiter.join();
    check(watchdog && dispatcher.ActiveObjects() == 0, "host watchdog remains distinct after last close and releases all pins");
  }
  {
    const auto semaphore = handles.CreateSemaphore(0, 2);
    const auto alias = handles.Duplicate(semaphore);
    std::atomic<size_t> completed = 0;
    const auto consume = [&] {
      if (wait(std::span(&semaphore, 1), Dispatcher::WaitMode::Any, 0) == 0) { ++completed; }
    };
    std::jthread first(consume);
    std::jthread second(consume);
    parked(dispatcher, 2);
    handles.Close(semaphore);
    check(handles.ReleaseSemaphore(alias, 2) == 0, "alias releases permits to both retained waits");
    handles.Close(alias);
    first.join();
    second.join();
    check(completed == 2 && dispatcher.ActiveObjects() == 0, "two pending references survive final close and consume exactly two permits");
  }
  {
    const auto event = handles.CreateEvent(false, true);
    const auto semaphore = handles.CreateSemaphore(0, 1);
    const std::array<uint32_t, 2> invalid{event, 0};
    fault([&] { wait(invalid); }, "invalid final handle rejects before consuming first signal");
    fault([&] { handles.ResetEvent(semaphore); }, "wrong object type remains a fault");
    fault([&] { handles.ReleaseSemaphore(event, 1); }, "wrong release type does not alter an event");
    fault([&] { handles.ReleaseSemaphore(semaphore, 2); }, "overflowing release does not invent permits");
    fault([&] { handles.CreateSemaphore(-1, 1); }, "invalid construction publishes no handle or object");
    fault([&] { wait(std::span(&event, 1), Dispatcher::WaitMode::Any, 0xFFFFFFFC); }, "invalid timeout releases handle pins");
    {
      auto lease = memory.Borrow();
      fault([&] { wait(std::span(&event, 1)); }, "backing lease cannot cross a handle wait");
    }
    check(wait(std::span(&event, 1)) == 0 && handles.ActiveHandles() == 2 && dispatcher.ActiveObjects() == 2, "failed preflight preserves signals and budgets");
    handles.Close(event);
    handles.Close(semaphore);
  }
  {
    std::array<uint32_t, 128> aliases{};
    aliases[0] = handles.CreateEvent(true, false);
    for (size_t n = 1; n < aliases.size(); ++n) { aliases[n] = handles.Duplicate(aliases[0]); }
    fault([&] { handles.Duplicate(aliases[0]); }, "handle capacity rejects duplication");
    fault([&] { handles.CreateEvent(false, false); }, "handle capacity rejects creation without an object leak");
    check(dispatcher.ActiveObjects() == 1 && handles.ActiveHandles() == 128, "handle capacity is independent of object capacity");
    for (const auto handle : aliases) { handles.Close(handle); }
    std::array<uint32_t, 128> objects{};
    for (auto& handle : objects) { handle = handles.CreateEvent(false, false); }
    const uint64_t relative = UINT64_MAX;
    memory.Write(ctx, 0x10000, &relative, sizeof(relative));
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(std::span(&objects.back(), 1)); });
    parked(dispatcher);
    handles.Close(objects.back());
    fault([&] { handles.CreateEvent(false, false); }, "pinned object capacity rejects creation despite a free handle slot");
    check(handles.ActiveHandles() == 127 && dispatcher.ActiveObjects() == 128, "failed object allocation leaks neither a handle nor an extra object");
    dispatcher.AdvanceTime(1);
    waiter.join();
    check(status == 0x102 && dispatcher.ActiveObjects() == 127, "closed object survives until genuine guest timeout then releases its capacity");
    memory.Write(ctx, 0x10000, &zero, sizeof(zero));
    objects.back() = handles.CreateSemaphore(0, 1);
    for (const auto handle : objects) { handles.Close(handle); }
  }
  {
    std::array<uint32_t, 64> set{};
    for (auto& handle : set) { handle = handles.CreateEvent(false, false); }
    handles.SetEvent(set.back());
    check(wait(set) == 63, "highest supported index survives handle resolution");
    handles.SetEvent(set.front());
    const std::array<uint32_t, 0> empty{};
    fault([&] { wait(empty); }, "empty set rejects");
    std::array<uint32_t, 65> excessive{};
    fault([&] { wait(excessive); }, "oversized set rejects before resolution");
    check(wait(set) == 0, "failed admission preserves ready signal");
    for (const auto handle : set) { handles.Close(handle); }
  }
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0, "all handle contracts clean up");
  {
    DispatcherHandles scoped(dispatcher);
    scoped.CreateEvent(false, false);
    scoped.CreateSemaphore(0, 1);
  }
  check(dispatcher.ActiveObjects() == 0, "table destruction releases remaining handles");
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
  workerHandle = handles.CreateEvent(false, false);
  const auto cleanup = handles.CreateEvent(false, false);
  runtime.Launch(ctx, memory.Base(), workerWait);
  parked(dispatcher);
  handles.Close(workerHandle);
  check(!runtime.Stop() && cancelled && dispatcher.ActiveObjects() == 1 && dispatcher.ActiveWaiters() == 0 && runtime.ActiveWorkers() == 0, "Stop unwinds closed pending handle references and joins owned worker");
  check(dispatcher.ActiveObjects() == 1, "unrelated open handle survives cancellation for explicit cleanup");
  fault([&] { handles.CreateEvent(false, false); }, "cancelled table cannot create new objects");
  fault([&] { handles.Duplicate(cleanup); }, "cancelled table cannot duplicate handles");
  fault([&] { handles.SetEvent(cleanup); }, "cancelled table cannot signal through handles");
  handles.Close(cleanup);
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0, "close remains available after cancellation");
  workerHandles = nullptr;
};

int main() {
  try {
    contracts();
    cancellation();
    std::puts("dispatcher-handles-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
