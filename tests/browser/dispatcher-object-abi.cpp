#include "browser/dispatcher-object-abi.h"
#include "browser/dispatcher-wait-abi.h"
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
constexpr uint32_t timeout = 0x12003;
const auto frame = [](Memory& memory, X86Context& ctx, std::initializer_list<uint32_t> args) {
  ctx.esp = stack;
  ctx.eax = 0xDEADBEEF;
  writeWord(memory, ctx, stack, 0x12345678);
  size_t n = 1;
  for (const auto value : args) { writeWord(memory, ctx, stack + 4 * n, value); ++n; }
};
const auto setup = [](Memory& memory) {
  memory.Map(0x10000, 0x3000);
  memory.Commit(0x10000, 0x3000);
};

const auto contracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  setup(memory);
  X86Context ctx{};
  ctx.thread_handle = 4;
  ctx.ebx = 0xCAFEBABE;
  const auto invoke = [&](uint32_t ordinal, std::initializer_list<uint32_t> args) {
    frame(memory, ctx, args);
    browser::InvokeDispatcherObject(handles, memory, ctx, ordinal);
    check(ctx.eax == 0 && ctx.esp == stack + (args.size() + 1) * 4 && ctx.ebx == 0xCAFEBABE,
      "successful object ABI publishes success and exact stdcall cleanup only");
  };
  const auto reject = [&](uint32_t ordinal, std::initializer_list<uint32_t> args) {
    frame(memory, ctx, args);
    const auto active = handles.ActiveHandles();
    const auto objects = dispatcher.ActiveObjects();
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "invalid object ABI rejects");
    check(ctx.eax == 0xDEADBEEF && ctx.esp == stack && handles.ActiveHandles() == active && dispatcher.ActiveObjects() == objects,
      "failed object ABI preserves registers handles and object count");
  };
  const auto poll = [&](uint32_t handle, uint32_t owner = 4) {
    auto thread = ctx;
    thread.thread_handle = owner;
    return handles.WaitGuestStatus(memory, thread, std::span(&handle, 1), Dispatcher::WaitMode::Any, timeout, false, 2s);
  };
  invoke(189, {output, 0, 0, 0x101});
  const auto notification = readWord(memory, ctx, output);
  check(poll(notification) == 0 && poll(notification) == 0, "notification event creation decodes low BOOLEAN byte");
  invoke(186, {notification});
  check(poll(notification) == 0x102, "clear event resets persistent signal");
  invoke(225, {notification, output});
  check(readWord(memory, ctx, output) == 0, "set event returns prior zero signal");
  invoke(225, {notification, output});
  check(readWord(memory, ctx, output) == 1, "set event returns prior one signal");
  invoke(189, {output, 0, 1, 0x100});
  const auto synchronization = readWord(memory, ctx, output);
  check(poll(synchronization) == 0x102, "high BOOLEAN bits do not signal creation");
  invoke(225, {synchronization, 0});
  check(poll(synchronization) == 0 && poll(synchronization) == 0x102, "optional previous output and single signal consumption");
  invoke(192, {output, 0, 0x101});
  const auto mutant = readWord(memory, ctx, output);
  const auto alias = handles.Duplicate(mutant);
  check(poll(alias, 8) == 0x102 && poll(alias) == 0, "initial ownership uses context identity and aliases recurse");
  invoke(221, {mutant, output});
  check(readWord(memory, ctx, output) == UINT32_MAX && poll(alias, 8) == 0x102, "recursive release publishes signed prior signal");
  invoke(221, {alias, output});
  check(readWord(memory, ctx, output) == 0 && poll(mutant, 8) == 0, "final release publishes prior zero and permits new owner");
  reject(221, {alias, output});
  check(poll(mutant) == 0x102, "wrong owner release leaves ownership intact");
  ctx.thread_handle = 8;
  invoke(221, {mutant, 0});
  ctx.thread_handle = 4;
  invoke(187, {mutant});
  reject(187, {mutant});
  check(poll(alias) == 0, "closing original keeps alias live");
  invoke(187, {alias});
  check(dispatcher.ActiveObjects() == 3 && handles.AbandonOwner(4) == 1, "ABI close retains owned mutant until explicit owner cleanup");

  writeWord(memory, ctx, output, 0xAABBCCDD);
  for (const auto pointer : {0u, 0xFFFFu, 0x12FFEu, 0xFFFFFFFEu}) {
    reject(189, {pointer, 0, 0, 1});
    reject(192, {pointer, 0, 1});
  }
  for (const auto pointer : {0xFFFFu, 0x12FFEu, 0xFFFFFFFEu}) {
    reject(225, {notification, pointer});
  }
  for (const auto type : {2u, UINT32_MAX}) { reject(189, {output, 0, type, 1}); }
  for (const auto attributes : {1u, output, UINT32_MAX}) {
    reject(189, {output, attributes, 0, 1});
    reject(192, {output, attributes, 1});
  }
  ctx.thread_handle = 0;
  reject(192, {output, 0, 1});
  invoke(192, {output, 0, 0x100});
  const auto unowned = readWord(memory, ctx, output);
  reject(221, {unowned, output});
  ctx.thread_handle = 4;
  handles.Close(unowned);
  writeWord(memory, ctx, output, 0xAABBCCDD);
  for (const auto ordinal : {0u, 188u, 190u, 197u, 233u}) { reject(ordinal, {notification, output}); }
  for (const auto ordinal : {186u, 187u, 221u, 225u}) { reject(ordinal, {0, output}); }
  reject(221, {notification, output});
  const auto semaphore = handles.CreateSemaphore(1, 1);
  reject(225, {semaphore, output});
  reject(186, {semaphore});
  check(poll(semaphore) == 0 && readWord(memory, ctx, output) == 0xAABBCCDD,
    "failed kind and parameter preflight preserves signal and output bytes");
  handles.Close(semaphore);

  const auto owned = handles.CreateMutant(4);
  reject(221, {owned, 0x12FFE});
  check(poll(owned, 8) == 0x102, "invalid release output cannot release ownership");
  invoke(221, {owned, output});
  handles.Close(owned);
  invoke(186, {notification});
  reject(225, {notification, 0x12FFE});
  check(poll(notification) == 0x102, "invalid set output cannot publish signal");
  for (const auto ordinal : {189u, 192u, 225u, 221u, 186u, 187u}) {
    frame(memory, ctx, {output, 0, 0, 1});
    auto lease = memory.Borrow();
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "inherited backing lease rejects object ABI");
    check(ctx.eax == 0xDEADBEEF && ctx.esp == stack, "inherited lease preserves registers");
  }
  for (const auto pointer : {0xFFFFu, 0x12FF1u, 0xFFFFFFF0u}) {
    frame(memory, ctx, {output, 0, 0, 1});
    ctx.esp = pointer;
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, 189); }, "invalid frame rejected before allocation");
    check(ctx.eax == 0xDEADBEEF && ctx.esp == pointer, "invalid frame preserves registers");
  }
  frame(memory, ctx, {output, 0, 0, 1});
  memory.Decommit(0x11000, 0x1000);
  fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, 189); }, "uncommitted output rejects before allocation");
  memory.Commit(0x11000, 0x1000);
  frame(memory, ctx, {0x10FFE, 0, 0, 1});
  memory.Decommit(0x11000, 0x1000);
  fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, 189); }, "partially committed output rejects before allocation");
  memory.Commit(0x11000, 0x1000);
  check(handles.ActiveHandles() == 2 && dispatcher.ActiveObjects() == 2, "all failed creations leave no object or handle");

  invoke(189, {stack + 8, 0, 1, 1});
  const auto overlapping = readWord(memory, ctx, stack + 8);
  check(poll(overlapping) == 0, "frame snapshot permits output overlapping attributes without changing decoded arguments");
  handles.Close(overlapping);
  memory.Map(0xFFFFF000, 0x1000);
  memory.Commit(0xFFFFF000, 0x1000);
  invoke(189, {0xFFFFFFFC, 0, 1, 1});
  const auto highest = readWord(memory, ctx, 0xFFFFFFFC);
  check(poll(highest) == 0, "output ending exactly at four GiB is valid");
  invoke(225, {highest, 0xFFFFFFFC});
  check(readWord(memory, ctx, 0xFFFFFFFC) == 0, "previous state output supports final address space dword");
  handles.Close(highest);
  frame(memory, ctx, {output, 0, 0, 1});
  for (size_t n = 0; n < 5; ++n) { writeWord(memory, ctx, 0xFFFFFFEC + n * 4, readWord(memory, ctx, stack + n * 4)); }
  ctx.esp = 0xFFFFFFEC;
  fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, 189); }, "stack cleanup cannot wrap after valid final byte frame");
  check(ctx.eax == 0xDEADBEEF && ctx.esp == 0xFFFFFFEC, "cleanup overflow leaves frame and registers untouched");

  for (size_t n = 0; n < 100; ++n) {
    invoke(186, {notification});
    writeWord(memory, ctx, output, 0xAABBCCDD);
    frame(memory, ctx, {notification, output});
    bool completed = false;
    std::jthread mutation([&] {
      try { browser::InvokeDispatcherObject(handles, memory, ctx, 225); completed = true; }
      catch (const browser::Fault&) {}
    });
    memory.Decommit(0x11000, 0x1000);
    mutation.join();
    check(poll(notification) == (completed ? 0u : 0x102u), "output retirement race never signals on failed publication");
    check(ctx.eax == (completed ? 0u : 0xDEADBEEFu) && ctx.esp == (completed ? stack + 12 : stack),
      "output retirement race publishes registers only on complete operation");
    memory.Commit(0x11000, 0x1000);
  }
  std::array<uint32_t, 126> capacity{};
  for (auto& handle : capacity) { handle = handles.CreateEvent(false, false); }
  writeWord(memory, ctx, output, 0xAABBCCDD);
  reject(189, {output, 0, 0, 1});
  reject(192, {output, 0, 1});
  check(readWord(memory, ctx, output) == 0xAABBCCDD, "capacity failures preserve output and introduce no ownership");
  for (const auto handle : capacity) { handles.Close(handle); }
  invoke(187, {notification});
  invoke(187, {synchronization});
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0, "object ABI fixtures retire all state");
};

DispatcherHandles* thunkHandles = nullptr;
const auto setEventThunk = [](X86Context& ctx, uint8_t* base) {
  browser::InvokeDispatcherObject(*thunkHandles, Memory::From(base), ctx, 225);
};

const auto handoffContracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  setup(memory);
  X86Context producer{};
  producer.host = &runtime;
  producer.thread_handle = 4;
  auto consumer = producer;
  consumer.thread_handle = 8;
  const auto event = handles.CreateEvent(false, false);
  const auto alias = handles.Duplicate(event);
  frame(memory, consumer, {event, 0, 0});
  std::jthread waiter([&] { browser::InvokeDispatcherWait(handles, memory, consumer, 233, 2s); });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (dispatcher.ActiveWaiters() != 1 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  check(dispatcher.ActiveWaiters() == 1, "wait ABI reaches blocking boundary before producer mutation");
  frame(memory, producer, {event});
  browser::InvokeDispatcherObject(handles, memory, producer, 187);
  thunkHandles = &handles;
  runtime.RegisterKernel(225, setEventThunk);
  frame(memory, producer, {alias, output});
  runtime.Invoke(producer, memory.Base(), runtime.KernelTarget(225));
  waiter.join();
  check(producer.eax == 0 && producer.esp == stack + 12 && readWord(memory, producer, output) == 0,
    "test registered kernel thunk returns real prior event signal and correct cleanup");
  check(consumer.eax == 0 && consumer.esp == stack + 16,
    "ABI producer wakes independently blocked checked wait despite original closure and changed frame bytes");
  frame(memory, producer, {alias});
  browser::InvokeDispatcherObject(handles, memory, producer, 187);
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0,
    "ABI handoff releases handle and pending operation lifetime");
  thunkHandles = nullptr;
};

const auto runtimeContracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  setup(memory);
  X86Context ctx{};
  const auto event = handles.CreateEvent(false, false);
  runtime.Stop();
  for (const auto ordinal : {186u, 225u}) {
    frame(memory, ctx, {event, output});
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "cancelled mutation rejects");
    check(ctx.eax == 0xDEADBEEF && ctx.esp == stack, "cancelled mutation preserves registers");
  }
  for (const auto ordinal : {189u, 192u}) {
    frame(memory, ctx, {output, 0, 0, 0});
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "cancelled creation rejects");
  }
  frame(memory, ctx, {event});
  browser::InvokeDispatcherObject(handles, memory, ctx, 187);
  check(ctx.eax == 0 && ctx.esp == stack + 8 && handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0,
    "checked close remains available for cleanup after cancellation");
};

int main() {
  try {
    contracts();
    handoffContracts();
    runtimeContracts();
    std::puts("dispatcher-object-abi-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
