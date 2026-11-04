#include "browser/dispatcher-object-abi.h"
#include "browser/dispatcher-wait-abi.h"
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
const auto writeWord = [](Memory& memory, const X86Context& ctx, uint32_t address, uint32_t value) {
  std::array<uint8_t, 4> bytes{};
  for (size_t n = 0; n < bytes.size(); ++n) { bytes[n] = uint8_t(value >> (n * 8)); }
  memory.Write(ctx, address, bytes.data(), bytes.size());
};
const auto readWord = [](Memory& memory, const X86Context& ctx, uint32_t address) {
  std::array<uint8_t, 4> bytes{};
  memory.Read(ctx, address, bytes.data(), bytes.size());
  uint32_t value = 0;
  for (size_t n = 0; n < bytes.size(); ++n) { value |= uint32_t(bytes[n]) << (n * 8); }
  return value;
};
constexpr uint32_t stack = 0x10003;
constexpr uint32_t output = 0x11003;
constexpr uint32_t dueAddress = 0x12003;
constexpr uint32_t timeout = 0x12103;
constexpr uint32_t sentinel = 0xDEADBEEF;
const auto frame = [](Memory& memory, X86Context& ctx, std::initializer_list<uint32_t> args) {
  ctx.esp = stack;
  ctx.eax = sentinel;
  writeWord(memory, ctx, stack, 0x12345678);
  size_t n = 1;
  for (const auto value : args) { writeWord(memory, ctx, stack + 4 * n, value); ++n; }
};
const auto writeDue = [](Memory& memory, const X86Context& ctx, uint32_t address, int64_t due) {
  const auto bits = std::bit_cast<uint64_t>(due);
  writeWord(memory, ctx, address, uint32_t(bits));
  writeWord(memory, ctx, address + 4, uint32_t(bits >> 32));
};
const auto setup = [](Memory& memory) {
  memory.Map(0x10000, 0x3000);
  memory.Commit(0x10000, 0x3000);
};

const auto contracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime, 1000);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  setup(memory);
  X86Context ctx{};
  ctx.ebx = 0xCAFEBABE;
  const auto invoke = [&](uint32_t ordinal, std::initializer_list<uint32_t> args) {
    frame(memory, ctx, args);
    browser::InvokeDispatcherObject(handles, memory, ctx, ordinal);
    check(ctx.eax == 0 && ctx.esp == stack + (args.size() + 1) * 4 && ctx.ebx == 0xCAFEBABE,
      "timer ABI publishes exact stdcall success only");
  };
  const auto reject = [&](uint32_t ordinal, std::initializer_list<uint32_t> args) {
    frame(memory, ctx, args);
    const auto active = handles.ActiveHandles();
    const auto objects = dispatcher.ActiveObjects();
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "invalid timer ABI rejects");
    check(ctx.eax == sentinel && ctx.esp == stack && handles.ActiveHandles() == active && dispatcher.ActiveObjects() == objects,
      "failed timer ABI preserves registers and object budgets");
  };
  const auto poll = [&](uint32_t handle) {
    return handles.WaitGuestStatus(memory, ctx, std::span(&handle, 1), Dispatcher::WaitMode::Any, timeout, false, 2s);
  };
  invoke(194, {output, 0, 1});
  const auto timer = readWord(memory, ctx, output);
  const auto alias = handles.Duplicate(timer);
  const auto set = [&](int64_t due, uint32_t period = 0, uint32_t state = output) {
    writeDue(memory, ctx, dueAddress, due);
    invoke(229, {timer, dueAddress, 0, 0, 0, 0, period, state});
  };
  const auto state = [&](uint32_t expected) {
    check(readWord(memory, ctx, output) == ((sentinel & 0xFFFFFF00) | expected), "state output writes exactly one BOOLEAN byte");
  };
  writeWord(memory, ctx, output, sentinel);
  set(-7);
  state(0);
  set(-5);
  state(0);
  invoke(185, {alias, output});
  state(0);
  dispatcher.AdvanceTime(7);
  check(poll(timer) == 0x102, "cancel returns signal state not previous armed state and removes shared schedule");
  set(0);
  state(0);
  set(-3);
  state(1);
  check(poll(alias) == 0x102, "rearming returns prior signal then clears it");
  dispatcher.AdvanceTime(3);
  invoke(185, {timer, output});
  state(1);
  check(poll(alias) == 0 && poll(timer) == 0x102, "cancel preserves existing synchronization signal");
  set(1015, 2);
  dispatcher.AdvanceTime(4);
  check(poll(timer) == 0x102, "absolute deadline does not fire early");
  dispatcher.AdvanceTime(1);
  check(poll(timer) == 0, "absolute due pointer uses virtual system origin");
  dispatcher.AdvanceTime(19999);
  check(poll(timer) == 0x102, "period is signed milliseconds not ticks");
  dispatcher.AdvanceTime(1);
  check(poll(alias) == 0, "period conversion retains exact expiry");
  invoke(185, {timer, 0});
  invoke(194, {output, 0, 0});
  const auto notification = readWord(memory, ctx, output);
  writeDue(memory, ctx, dueAddress, 0);
  invoke(229, {notification, dueAddress, 0, 1, 0, 0x100, 0, 0});
  check(poll(notification) == 0 && poll(notification) == 0, "notification persistence and low resume BOOLEAN byte");
  handles.Close(notification);
  writeWord(memory, ctx, output, sentinel);
  for (const auto type : {2u, UINT32_MAX}) { reject(194, {output, 0, type}); }
  reject(194, {output, 1, 0});
  for (const auto address : {0u, 0xFFFFu, 0x12FFEu, 0xFFFFFFFEu}) { reject(194, {address, 0, 0}); }
  const auto event = handles.CreateEvent(true, true);
  const auto semaphore = handles.CreateSemaphore(1, 1);
  const auto mutant = handles.CreateMutant(4);
  for (const auto handle : {0u, UINT32_MAX, event, semaphore, mutant}) {
    reject(185, {handle, output});
    reject(229, {handle, dueAddress, 0, 0, 0, 0, 0, output});
  }
  check(poll(event) == 0 && poll(semaphore) == 0 && handles.ReleaseMutant(mutant, 4) == 0,
    "wrong kind preserves unrelated signals and ownership");
  for (const auto handle : {event, semaphore, mutant}) { handles.Close(handle); }
  set(-5, 1);
  for (const auto address : {0u, 0xFFFFu, 0x12FFCu, 0xFFFFFFFCu}) {
    reject(229, {timer, address, 0, 0, 0, 0, 0, output});
  }
  for (const auto address : {0xFFFFu, 0x13000u}) {
    reject(229, {timer, dueAddress, 0, 0, 0, 0, 0, address});
    reject(185, {timer, address});
  }
  for (const auto args : {
    std::array<uint32_t, 8>{timer, dueAddress, 1, 0, 0, 0, 0, output},
    std::array<uint32_t, 8>{timer, dueAddress, 0, 2, 0, 0, 0, output},
    std::array<uint32_t, 8>{timer, dueAddress, 0, 0, 1, 0, 0, output},
    std::array<uint32_t, 8>{timer, dueAddress, 0, 0, 0, 1, 0, output},
    std::array<uint32_t, 8>{timer, dueAddress, 0, 0, 0, 0, UINT32_MAX, output},
    std::array<uint32_t, 8>{timer, dueAddress, 0, 0, 0, 0, 0x80000000, output}}) {
    reject(229, {args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]});
  }
  dispatcher.AdvanceTime(4);
  check(poll(timer) == 0x102, "all failed replacements preserve original deadline");
  dispatcher.AdvanceTime(1);
  check(poll(timer) == 0, "all failed cancels preserve original schedule");
  dispatcher.AdvanceTime(10000);
  check(poll(timer) == 0, "failed replacements preserve original period");
  invoke(185, {alias, 0});
  for (const auto ordinal : {185u, 194u, 229u}) {
    frame(memory, ctx, {timer, dueAddress, 0, 0, 0, 0, 0, output});
    {
      auto lease = memory.Borrow();
      fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "timer ABI rejects inherited lease");
    }
    for (const auto address : {0xFFFFu, 0x12FFDu, 0xFFFFFFFCu}) {
      ctx.esp = address;
      fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "timer ABI rejects malformed stack");
      check(ctx.eax == sentinel && ctx.esp == address, "malformed stack preserves completion registers");
    }
  }
  memory.Decommit(0x11000, 0x1000);
  reject(194, {output, 0, 0});
  reject(185, {timer, output});
  reject(229, {timer, dueAddress, 0, 0, 0, 0, 0, output});
  memory.Commit(0x11000, 0x1000);
  invoke(194, {stack + 12, 0, 1});
  const auto overlapping = readWord(memory, ctx, stack + 12);
  handles.Close(overlapping);
  set(-2, 0, dueAddress);
  dispatcher.AdvanceTime(2);
  check(poll(timer) == 0, "state overlapping due bytes cannot redirect captured deadline");
  writeDue(memory, ctx, dueAddress, -3);
  invoke(229, {timer, dueAddress, 0, 0, 0, 0, 0, stack + 4});
  dispatcher.AdvanceTime(3);
  check(poll(timer) == 0, "state overlapping frame cannot change captured handle");
  memory.Map(0xFFFFF000, 0x1000);
  memory.Commit(0xFFFFF000, 0x1000);
  writeDue(memory, ctx, 0xFFFFFFF8, -1);
  invoke(229, {timer, 0xFFFFFFF8, 0, 0, 0, 0, 0, UINT32_MAX});
  dispatcher.AdvanceTime(1);
  invoke(185, {timer, UINT32_MAX});
  check(poll(timer) == 0, "last address byte supports BOOLEAN output and due span may end at four GiB");
  for (const auto ordinal : {185u, 194u, 229u}) {
    const size_t words = ordinal == 185 ? 3 : (ordinal == 194 ? 4 : 9);
    const uint32_t address = uint32_t(0x100000000ull - words * 4);
    for (size_t n = 0; n < words; ++n) { writeWord(memory, ctx, address + n * 4, 0); }
    ctx.esp = address;
    ctx.eax = sentinel;
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "complete final frame cannot wrap cleanup");
    check(ctx.esp == address && ctx.eax == sentinel, "overflow preserves completion registers");
  }
  for (size_t n = 0; n < 100; ++n) {
    writeDue(memory, ctx, dueAddress, 0);
    frame(memory, ctx, {timer, dueAddress, 0, 0, 0, 0, 0, output});
    bool completed = false;
    std::jthread mutation([&] {
      try { browser::InvokeDispatcherObject(handles, memory, ctx, 229); completed = true; }
      catch (const browser::Fault&) {}
    });
    memory.Decommit(0x11000, 0x1000);
    mutation.join();
    check(poll(timer) == (completed ? 0u : 0x102u) && poll(timer) == 0x102,
      "output retirement cannot produce a signal on failed schedule publication");
    check(ctx.eax == (completed ? 0u : sentinel) && ctx.esp == (completed ? stack + 36 : stack),
      "retirement race publishes exact completion only on success");
    memory.Commit(0x11000, 0x1000);
  }
  std::array<uint32_t, browser::kDispatcherCapacity - 2> capacity{};
  for (auto& handle : capacity) { handle = handles.Duplicate(alias); }
  writeWord(memory, ctx, output, sentinel);
  reject(194, {output, 0, 1});
  check(readWord(memory, ctx, output) == sentinel, "handle exhaustion preserves create output");
  for (const auto handle : capacity) { handles.Close(handle); }
  handles.Close(alias);
  const auto retained = dispatcher.CreateMutant(4);
  for (auto& handle : capacity) { handle = handles.CreateTimer(false); }
  reject(194, {output, 0, 1});
  check(readWord(memory, ctx, output) == sentinel, "object exhaustion preserves create output");
  for (const auto handle : capacity) { handles.Close(handle); }
  dispatcher.Destroy(retained);
  handles.Close(timer);
  reject(185, {timer, output});
  reject(229, {timer, dueAddress, 0, 0, 0, 0, 0, output});
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0, "timer ABI contracts retire all state");
};

DispatcherHandles* thunkHandles = nullptr;
const auto setThunk = [](X86Context& ctx, uint8_t* base) {
  browser::InvokeDispatcherObject(*thunkHandles, Memory::From(base), ctx, 229);
};
const auto handoffContracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  setup(memory);
  X86Context producer{};
  producer.host = &runtime;
  X86Context consumer = producer;
  const auto timer = handles.CreateTimer(false);
  const auto alias = handles.Duplicate(timer);
  frame(memory, consumer, {timer, 0, 0});
  std::jthread waiter([&] { browser::InvokeDispatcherWait(handles, memory, consumer, 233, 2s); });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (dispatcher.ActiveWaiters() != 1 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  check(dispatcher.ActiveWaiters() == 1, "timer wait reaches independent blocking boundary");
  handles.Close(timer);
  writeDue(memory, producer, dueAddress, -5);
  frame(memory, producer, {alias, dueAddress, 0, 0, 0, 0, 0, output});
  thunkHandles = &handles;
  runtime.RegisterKernel(229, setThunk);
  runtime.Invoke(producer, memory.Base(), runtime.KernelTarget(229));
  handles.Close(alias);
  memory.Decommit(0x12000, 0x1000);
  dispatcher.AdvanceTime(5);
  waiter.join();
  check(producer.eax == 0 && producer.esp == stack + 36 && consumer.eax == 0 && consumer.esp == stack + 16,
    "registered timer thunk and closed pinned wait complete with independent cleanup");
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0,
    "closed timer completes without retaining guest due pointer or handles");
  thunkHandles = nullptr;
};

const auto boundaryContracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  setup(memory);
  X86Context ctx{};
  const auto timer = handles.CreateTimer(false);
  const auto rejectSet = [&](int64_t due) {
    writeDue(memory, ctx, dueAddress, due);
    frame(memory, ctx, {timer, dueAddress, 0, 0, 0, 0, 0, output});
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, 229); }, "invalid deadline rejects through ABI");
    check(ctx.eax == sentinel && ctx.esp == stack && readWord(memory, ctx, output) == sentinel,
      "deadline failure preserves output and completion registers");
  };
  writeWord(memory, ctx, output, sentinel);
  handles.SetTimer(timer, 1);
  rejectSet(1);
  dispatcher.AdvanceTime(1);
  check(handles.WaitGuestStatus(memory, ctx, std::span(&timer, 1), Dispatcher::WaitMode::Any, timeout, false, 2s) == 0,
    "missing origin preserves prior schedule");
  dispatcher.AdvanceTime(UINT64_MAX - 5 - dispatcher.TimerTime());
  handles.SetTimer(timer, 5);
  rejectSet(-6);
  rejectSet(INT64_MIN);
  dispatcher.AdvanceTime(5);
  check(handles.WaitGuestStatus(memory, ctx, std::span(&timer, 1), Dispatcher::WaitMode::Any, timeout, false, 2s) == 0,
    "overflow preserves final representable expiry");
  runtime.Stop();
  for (const auto ordinal : {185u, 194u, 229u}) {
    frame(memory, ctx, ordinal == 185 ? std::initializer_list<uint32_t>{timer, output}
      : (ordinal == 194 ? std::initializer_list<uint32_t>{output, 0, 1}
      : std::initializer_list<uint32_t>{timer, dueAddress, 0, 0, 0, 0, 0, output}));
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "stopped timer ABI rejects");
    check(ctx.eax == sentinel && ctx.esp == stack && readWord(memory, ctx, output) == sentinel && handles.ActiveHandles() == 1,
      "stopped operation preserves output registers and handles");
  }
  frame(memory, ctx, {timer});
  browser::InvokeDispatcherObject(handles, memory, ctx, 187);
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0, "close after stop cleans timer fixture");
};

int main() {
  try {
    contracts();
    handoffContracts();
    boundaryContracts();
    std::puts("dispatcher-timer-abi-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
