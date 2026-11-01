#include "browser/guest-wait.h"
#include <array>
#include <atomic>
#include <bit>
#include <cstdio>

using browser::Dispatcher;
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
const auto writeTimeout = [](Memory& memory, const X86Context& ctx, uint32_t address, int64_t timeout) {
  const auto bits = std::bit_cast<uint64_t>(timeout);
  std::array<uint8_t, 8> bytes{};
  for (size_t n = 0; n < bytes.size(); ++n) { bytes[n] = uint8_t(bits >> (n * 8)); }
  memory.Write(ctx, address, bytes.data(), bytes.size());
};

Dispatcher* thunkDispatcher = nullptr;
uint32_t thunkObject = 0;
std::atomic<bool> thunkCancelled = false;
const auto thunkWait = [](X86Context& ctx, uint8_t* base) {
  try {
    ctx.eax = browser::WaitGuestStatus(*thunkDispatcher, Memory::From(base), ctx, std::span(&thunkObject, 1), Dispatcher::WaitMode::Any, 0, false, 2s);
  } catch (const browser::Cancelled&) { thunkCancelled = true; throw; }
};

const auto contracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime, 1000);
  Memory memory;
  X86Context ctx{};
  memory.Map(0x10000, 0x2000);
  memory.Commit(0x10000, 0x2000);
  constexpr uint32_t timeoutAddress = 0x10003;
  const auto event = dispatcher.CreateEvent(false, true);
  const auto semaphore = dispatcher.CreateSemaphore(0, 1);
  const auto wait = [&](std::span<const uint32_t> objects, Dispatcher::WaitMode mode = Dispatcher::WaitMode::Any, uint32_t pointer = timeoutAddress, bool alertable = false, std::chrono::milliseconds watchdog = 2s, uint32_t owner = 0) {
    return browser::WaitGuestStatus(dispatcher, memory, ctx, objects, mode, pointer, alertable, watchdog, owner);
  };
  const std::array<uint32_t, 2> objects{semaphore, event};
  writeTimeout(memory, ctx, timeoutAddress, 0);
  check(wait(objects) == 1, "any wait maps the original ready index to STATUS_WAIT_0 plus index");
  check(wait(objects) == 0x102, "guest poll maps timeout to STATUS_TIMEOUT");
  dispatcher.SetEvent(event);
  dispatcher.ReleaseSemaphore(semaphore, 1);
  check(wait(objects, Dispatcher::WaitMode::All) == 0, "all wait returns STATUS_SUCCESS and consumes the complete set");
  check(wait(objects) == 0x102, "successful all wait consumes each permit once");
  {
    dispatcher.SetEvent(event);
    writeTimeout(memory, ctx, timeoutAddress, -3);
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(objects, Dispatcher::WaitMode::All); });
    parked(dispatcher);
    dispatcher.AdvanceTime(3);
    waiter.join();
    check(status == 0x102, "incomplete all wait expires on virtual time");
    writeTimeout(memory, ctx, timeoutAddress, 0);
    check(wait(std::span(&event, 1)) == 0, "all timeout preserves partial signals");
  }
  for (const auto timeout : {-int64_t(7), int64_t(1017)}) {
    writeTimeout(memory, ctx, timeoutAddress, timeout);
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(objects); });
    parked(dispatcher);
    dispatcher.AdvanceTime(7);
    waiter.join();
    check(status == 0x102, "signed relative and absolute timeout pointers expire exactly");
  }
  for (const auto timeout : {INT64_MIN, INT64_MAX}) {
    writeTimeout(memory, ctx, timeoutAddress, timeout);
    bool watchdog = false;
    try { wait(objects, Dispatcher::WaitMode::Any, timeoutAddress, false, 5ms); }
    catch (const browser::WaitWatchdogExpired&) { watchdog = true; }
    check(watchdog, "extreme finite timeout remains finite guest time with a distinct host watchdog fault");
  }
  {
    writeTimeout(memory, ctx, timeoutAddress, -5);
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(objects); });
    parked(dispatcher);
    writeTimeout(memory, ctx, timeoutAddress, 0);
    memory.Decommit(0x10000, 0x1000);
    runtime.Wake();
    dispatcher.AdvanceTime(5);
    waiter.join();
    check(status == 0x102, "timeout bytes are snapshotted and their backing need not survive the wait");
    memory.Commit(0x10000, 0x1000);
  }
  dispatcher.SetEvent(event);
  writeTimeout(memory, ctx, timeoutAddress, 0);
  for (const auto pointer : {0xFFFFu, 0x11FF9u, 0xFFFFFFFCu}) {
    fault([&] { wait(objects, Dispatcher::WaitMode::Any, pointer); }, "unmapped, crossing and wrapping timeout spans reject before consuming");
  }
  memory.Decommit(0x11000, 0x1000);
  fault([&] { wait(objects, Dispatcher::WaitMode::Any, 0x10FFCu); }, "partially uncommitted timeout rejects before consuming");
  memory.Commit(0x11000, 0x1000);
  fault([&] { wait(objects, Dispatcher::WaitMode::Any, timeoutAddress, true); }, "unsupported alertable wait rejects before consuming");
  fault([&] { wait(objects, Dispatcher::WaitMode::Any, timeoutAddress, false, 0ms); }, "invalid host watchdog cannot consume a signal");
  {
    auto lease = memory.Borrow();
    fault([&] { wait(objects); }, "guest wait rejects an inherited backing lease");
  }
  check(wait(objects) == 1 && dispatcher.ActiveWaiters() == 0, "rejected guest pointer and execution contracts preserve signals and admission");
  {
    std::array<uint32_t, 64> boundary{};
    for (auto& object : boundary) { object = dispatcher.CreateEvent(false, false); }
    dispatcher.SetEvent(boundary.back());
    check(wait(boundary) == 63, "highest supported any index maps without truncation");
    dispatcher.Destroy(boundary.back());
    boundary.back() = dispatcher.CreateMutant(4);
    dispatcher.AbandonOwner(4);
    check(wait(boundary, Dispatcher::WaitMode::Any, timeoutAddress, false, 2s, 8) == 0xBF, "highest supported abandoned index maps without truncation");
    dispatcher.ReleaseMutant(boundary.back(), 8);
    for (const auto object : boundary) { dispatcher.Destroy(object); }
  }
  {
    const auto mutant = dispatcher.CreateMutant(4);
    const std::array<uint32_t, 2> mixed{event, mutant};
    dispatcher.AbandonOwner(4);
    check(wait(mixed, Dispatcher::WaitMode::Any, timeoutAddress, false, 2s, 8) == 0x81, "abandoned any status retains the original mutant index");
    dispatcher.ReleaseMutant(mutant, 8);
    check(wait(mixed, Dispatcher::WaitMode::Any, timeoutAddress, false, 2s, 8) == 1, "abandonment is reported only once");
    dispatcher.AbandonOwner(8);
    check(wait(mixed, Dispatcher::WaitMode::All, timeoutAddress, false, 2s, 12) == 0x102, "incomplete all wait preserves abandonment");
    dispatcher.SetEvent(event);
    check(wait(mixed, Dispatcher::WaitMode::All, timeoutAddress, false, 2s, 12) == 0x81, "abandoned all status reports the lowest abandoned index after atomic acquisition");
    dispatcher.ReleaseMutant(mutant, 12);
    dispatcher.Destroy(mutant);
  }
  {
    const auto first = dispatcher.CreateMutant(4);
    const auto second = dispatcher.CreateMutant(4);
    const std::array<uint32_t, 3> mixed{event, first, second};
    dispatcher.AbandonOwner(4);
    dispatcher.SetEvent(event);
    check(wait(mixed, Dispatcher::WaitMode::All, timeoutAddress, false, 2s, 8) == 0x81, "all wait reports the lowest abandoned index when multiple mutants are abandoned");
    dispatcher.ReleaseMutant(first, 8);
    dispatcher.ReleaseMutant(second, 8);
    dispatcher.SetEvent(event);
    check(wait(mixed, Dispatcher::WaitMode::All, timeoutAddress, false, 2s, 8) == 0, "successful abandoned all wait clears every consumed abandonment flag");
    dispatcher.ReleaseMutant(first, 8);
    dispatcher.ReleaseMutant(second, 8);
    dispatcher.Destroy(first);
    dispatcher.Destroy(second);
  }
  {
    dispatcher.SetEvent(event);
    bool watchdog = false;
    try { wait(objects, Dispatcher::WaitMode::All, 0, false, 5ms); }
    catch (const browser::WaitWatchdogExpired&) { watchdog = true; }
    check(watchdog && wait(std::span(&event, 1)) == 0, "null timeout is indefinite and host watchdog cannot fabricate a guest timeout or consume partial signals");
  }
  {
    const auto retired = dispatcher.CreateEvent(false, false);
    bool rejected = false;
    std::jthread waiter([&] {
      try { wait(std::span(&retired, 1), Dispatcher::WaitMode::Any, 0); }
      catch (const browser::Fault&) { rejected = true; }
    });
    parked(dispatcher);
    dispatcher.Destroy(retired);
    waiter.join();
    check(rejected, "internal retirement is an execution fault rather than an invented guest close status");
  }
  dispatcher.Destroy(event);
  dispatcher.Destroy(semaphore);
  check(dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0, "guest adapter leaves no live state");
};

const auto runtimeContracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  Memory memory;
  thunkDispatcher = &dispatcher;
  thunkObject = dispatcher.CreateEvent(false, false);
  runtime.RegisterKernel(233, thunkWait);
  X86Context ctx{};
  ctx.host = &runtime;
  dispatcher.SetEvent(thunkObject);
  runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(233));
  check(ctx.eax == 0, "explicitly registered synthetic thunk returns a truthful guest status through kernel dispatch");
  thunkCancelled = false;
  runtime.Launch(ctx, memory.Base(), thunkWait);
  parked(dispatcher);
  check(!runtime.Stop() && thunkCancelled && runtime.ActiveWorkers() == 0 && dispatcher.ActiveWaiters() == 0, "Stop cancels and joins an indefinite adapted wait without returning guest success");
  dispatcher.Destroy(thunkObject);
  thunkDispatcher = nullptr;
};

int main() {
  try {
    contracts();
    runtimeContracts();
    std::puts("guest-wait-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
