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
  const auto wait = [&](std::span<const uint32_t> set, uint32_t owner, Dispatcher::WaitMode mode = Dispatcher::WaitMode::Any,
    uint32_t timeout = 0x10000, std::chrono::milliseconds watchdog = 2s) {
    auto thread = ctx;
    thread.thread_handle = owner;
    return handles.WaitGuestStatus(memory, thread, set, mode, timeout, false, watchdog);
  };
  const auto poll = [&](uint32_t handle, uint32_t owner) { return wait(std::span(&handle, 1), owner); };
  {
    const auto mutant = handles.CreateMutant();
    const auto alias = handles.Duplicate(mutant);
    fault([&] { poll(mutant, 0); }, "mutant acquisition requires nonzero context owner");
    check(poll(mutant, 4) == 0 && poll(alias, 4) == 0, "aliases share recursive ownership");
    check(poll(alias, 8) == 0x102, "different owner cannot consume held mutant");
    fault([&] { handles.ReleaseMutant(alias, 8); }, "wrong owner release rejects");
    fault([&] { handles.ReleaseMutant(alias, 0); }, "zero owner release rejects");
    check(handles.ReleaseMutant(mutant, 4) == -1 && poll(alias, 8) == 0x102, "partial recursive release preserves owner");
    check(handles.ReleaseMutant(alias, 4) == 0 && poll(mutant, 8) == 0, "final release transfers ownership");
    handles.Close(mutant);
    fault([&] { handles.ReleaseMutant(mutant, 8); }, "closed identifier cannot release through alias");
    check(handles.ReleaseMutant(alias, 8) == 0, "surviving alias releases original ownership");
    handles.Close(alias);
  }
  {
    const auto mutant = handles.CreateMutant(4);
    check(poll(mutant, 8) == 0x102, "initial owner holds mutant without waiting");
    handles.Close(mutant);
    check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 1, "final close retains owned mutant after all operation pins end");
    fault([&] { handles.ReleaseMutant(mutant, 4); }, "retained ownership does not resurrect closed handle");
    check(handles.AbandonOwner(8) == 0 && handles.AbandonOwner(4) == 1 && handles.AbandonOwner(4) == 0,
      "explicit owner exit retires closed ownership exactly once");
    check(dispatcher.ActiveObjects() == 0, "closed owned object retires on abandonment");
  }
  {
    const auto mutant = handles.CreateMutant();
    check(poll(mutant, 4) == 0 && poll(mutant, 4) == 0, "recursive successful waits own beyond completion");
    handles.Close(mutant);
    check(dispatcher.ActiveObjects() == 1 && handles.AbandonOwner(4) == 1 && dispatcher.ActiveObjects() == 0,
      "acquired recursive ownership survives final closure then abandons fully");
  }
  for (size_t n = 0; n < 100; ++n) {
    const auto mutant = handles.CreateMutant(4);
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(std::span(&mutant, 1), 8, Dispatcher::WaitMode::Any, 0); });
    parked(dispatcher);
    handles.Close(mutant);
    const auto replacement = handles.CreateMutant();
    check(handles.AbandonOwner(4) == 1, "abandonment reaches pending closed original");
    waiter.join();
    check(status == 0x80 && dispatcher.ActiveObjects() == 2 && poll(replacement, 12) == 0,
      "closed wait reports abandonment and retains new ownership without touching replacement");
    check(handles.AbandonOwner(8) == 1 && dispatcher.ActiveObjects() == 1, "new owner exit retires closed acquired mutant");
    handles.ReleaseMutant(replacement, 12);
    handles.Close(replacement);
  }
  {
    const auto event = handles.CreateEvent(false, true);
    const auto mutant = handles.CreateMutant(4);
    const std::array<uint32_t, 2> set{event, mutant};
    check(wait(set, 8, Dispatcher::WaitMode::All) == 0x102 && poll(event, 0) == 0,
      "incomplete mutant all wait preserves ready event");
    handles.SetEvent(event);
    check(handles.AbandonOwner(4) == 1 && wait(set, 8, Dispatcher::WaitMode::All) == 0x81 && poll(event, 0) == 0x102,
      "all acquisition consumes event and reports abandoned mutant index");
    handles.ReleaseMutant(mutant, 8);
    handles.Close(mutant);
    handles.Close(event);
  }
  {
    const auto mutant = handles.CreateMutant(4);
    const auto alias = handles.Duplicate(mutant);
    const auto event = handles.CreateEvent(false, true);
    const std::array<uint32_t, 3> set{event, mutant, alias};
    fault([&] { wait(set, 4, Dispatcher::WaitMode::All); }, "aliased mutants reject before recursive acquisition or consumption");
    check(poll(event, 0) == 0 && handles.ReleaseMutant(mutant, 4) == 0, "rejected duplicate leaves both signals unchanged");
    handles.Close(mutant);
    handles.Close(alias);
    handles.Close(event);
  }
  {
    const auto mutant = handles.CreateMutant(4);
    std::array<uint32_t, 2> statuses{};
    const auto consume = [&](size_t index, uint32_t owner) {
      statuses[index] = wait(std::span(&mutant, 1), owner, Dispatcher::WaitMode::Any, 0);
      handles.ReleaseMutant(mutant, owner);
    };
    std::jthread first([&] { consume(0, 8); });
    std::jthread second([&] { consume(1, 12); });
    parked(dispatcher, 2);
    handles.AbandonOwner(4);
    first.join(); second.join();
    check((statuses[0] == 0x80 && statuses[1] == 0) || (statuses[1] == 0x80 && statuses[0] == 0),
      "one contender consumes abandonment and releases to next owner");
    handles.Close(mutant);
  }
  {
    const auto mutant = handles.CreateMutant(4);
    const uint64_t relative = UINT64_MAX;
    memory.Write(ctx, 0x10000, &relative, sizeof(relative));
    uint32_t status = UINT32_MAX;
    std::jthread waiter([&] { status = wait(std::span(&mutant, 1), 8); });
    parked(dispatcher);
    handles.Close(mutant);
    dispatcher.AdvanceTime(1);
    waiter.join();
    check(status == 0x102 && dispatcher.ActiveObjects() == 1 && handles.AbandonOwner(8) == 0,
      "guest timeout drops wait pin without dropping original owner or granting new ownership");
    handles.AbandonOwner(4);
    memory.Write(ctx, 0x10000, &zero, sizeof(zero));
  }
  {
    const auto mutant = handles.CreateMutant(4);
    bool expired = false;
    std::jthread waiter([&] {
      try { wait(std::span(&mutant, 1), 8, Dispatcher::WaitMode::Any, 0, 100ms); }
      catch (const browser::WaitWatchdogExpired&) { expired = true; }
    });
    parked(dispatcher);
    handles.Close(mutant);
    waiter.join();
    check(expired && dispatcher.ActiveObjects() == 1 && handles.AbandonOwner(8) == 0,
      "host watchdog neither grants ownership nor drops existing owner");
    handles.AbandonOwner(4);
  }
  {
    const auto mutant = handles.CreateMutant();
    const auto event = handles.CreateEvent(false, true);
    fault([&] { handles.ReleaseMutant(event, 4); }, "event is not a mutant");
    fault([&] { handles.SetEvent(mutant); }, "mutant is not an event");
    fault([&] { handles.ReleaseSemaphore(mutant, 1); }, "mutant is not a semaphore");
    fault([&] { handles.SetTimer(mutant, 0); }, "mutant is not a timer");
    fault([&] { wait(std::span(&mutant, 1), 4, Dispatcher::WaitMode::Any, 0xFFFFFFFC); }, "invalid timeout rejects before ownership");
    fault([&] { handles.AbandonOwner(0); }, "zero owner abandonment rejects");
    check(poll(mutant, 8) == 0 && poll(event, 0) == 0, "rejected operations preserve unrelated state");
    handles.ReleaseMutant(mutant, 8);
    const std::array<uint32_t, 4> frame{0x12345678, mutant, 0, 0x10000};
    memory.Write(ctx, 0x10101, frame.data(), sizeof(frame));
    ctx.esp = 0x10101;
    ctx.thread_handle = 12;
    browser::InvokeDispatcherWait(handles, memory, ctx, 233, 2s);
    check(ctx.eax == 0 && ctx.esp == 0x10111 && poll(mutant, 4) == 0x102,
      "checked wait ABI uses context identity and retains acquired ownership");
    handles.Close(mutant);
    check(handles.AbandonOwner(12) == 1, "ABI ownership survives final handle closure");
    handles.Close(event);
  }
  {
    const auto mutant = handles.CreateMutant();
    for (size_t n = 0; n < 65535; ++n) { check(poll(mutant, 4) == 0, "bounded recursive acquisition"); }
    const auto event = handles.CreateEvent(false, true);
    const std::array<uint32_t, 2> set{event, mutant};
    fault([&] { wait(set, 4, Dispatcher::WaitMode::All); }, "recursion ceiling rejects complete all set before consumption");
    check(poll(event, 0) == 0 && handles.AbandonOwner(4) == 1 && poll(mutant, 8) == 0x80,
      "recursion overflow preserves event and abandonment resets full recursion");
    handles.ReleaseMutant(mutant, 8);
    handles.Close(mutant);
    handles.Close(event);
  }
  {
    std::array<uint32_t, 64> mutants{};
    for (auto& handle : mutants) { handle = handles.CreateMutant(4); }
    check(handles.AbandonOwner(4) == 64 && wait(mutants, 8, Dispatcher::WaitMode::All) == 0x80,
      "atomic all acquires 64 abandoned mutants and reports lowest index");
    handles.AbandonOwner(8);
    for (size_t n = 0; n + 1 < mutants.size(); ++n) { check(poll(mutants[n], 4) == 0x80, "consume lower abandoned indices"); }
    check(wait(mutants, 8) == 0xBF, "any preserves highest abandoned index 63");
    for (const auto handle : mutants) { handles.Close(handle); }
    check(handles.AbandonOwner(4) == 63 && handles.AbandonOwner(8) == 1,
      "closed multiobject acquisition retains each distinct owner");
  }
  {
    std::array<uint32_t, 128> mutants{};
    for (auto& handle : mutants) { handle = handles.CreateMutant(4); }
    fault([&] { handles.CreateMutant(8); }, "mutant handle capacity rejects before object allocation");
    for (const auto handle : mutants) { handles.Close(handle); }
    fault([&] { handles.CreateMutant(); }, "closed ownership still consumes object budget");
    check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 128 && handles.AbandonOwner(8) == 0,
      "failed creation leaks no owner or handle");
    check(handles.AbandonOwner(4) == 128 && dispatcher.ActiveObjects() == 0, "owner exit releases full closed object budget");
    const auto replacement = handles.CreateMutant();
    handles.Close(replacement);
  }
  {
    DispatcherHandles scoped(dispatcher);
    scoped.CreateMutant(4);
  }
  check(dispatcher.ActiveObjects() == 1 && handles.AbandonOwner(4) == 1,
    "table destruction does not impersonate thread exit for owned mutants");
  for (size_t n = 0; n < 100; ++n) {
    const auto mutant = handles.CreateMutant(4);
    std::jthread exiting([&] { handles.AbandonOwner(4); });
    handles.Close(mutant);
    exiting.join();
    check(dispatcher.ActiveObjects() == 0, "owner abandonment racing final close cannot leak or resurrect object");
  }
  for (size_t n = 0; n < 100; ++n) {
    const auto mutant = handles.CreateMutant(4);
    bool released = false;
    std::jthread releasing([&] {
      try {
        handles.ReleaseMutant(mutant, 4);
        released = true;
      } catch (const browser::Fault& error) {
        check(error.address == mutant, "release loses lookup only to final closure");
      }
    });
    handles.Close(mutant);
    releasing.join();
    check(handles.AbandonOwner(4) == (released ? 0 : 1) && dispatcher.ActiveObjects() == 0,
      "release racing final close either completes or leaves explicit owner cleanup");
  }
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0,
    "mutant contracts clean up all handles objects and waits");
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
  ctx.thread_handle = 8;
  workerHandles = &handles;
  workerHandle = handles.CreateMutant(4);
  const auto cleanup = handles.CreateMutant(12);
  runtime.Launch(ctx, memory.Base(), workerWait);
  parked(dispatcher);
  handles.Close(workerHandle);
  check(!runtime.Stop() && cancelled && dispatcher.ActiveWaiters() == 0 && runtime.ActiveWorkers() == 0,
    "Stop cancels and joins actual runtime owned mutant waiter");
  check(dispatcher.ActiveObjects() == 2 && handles.AbandonOwner(8) == 0, "cancellation cannot acquire or abandon unrelated ownership");
  fault([&] { handles.CreateMutant(); }, "cancelled runtime rejects allocation");
  fault([&] { handles.ReleaseMutant(cleanup, 12); }, "cancelled runtime rejects release");
  handles.Close(cleanup);
  check(handles.AbandonOwner(4) == 1 && handles.AbandonOwner(12) == 1 && dispatcher.ActiveObjects() == 0,
    "explicit owner exit remains available after cancellation and final closure");
  workerHandles = nullptr;
};

int main() {
  try {
    contracts();
    cancellation();
    std::puts("dispatcher-mutant-handles-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
