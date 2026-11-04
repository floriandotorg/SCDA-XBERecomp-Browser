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
constexpr uint32_t sentinel = 0xDEADBEEF;
const auto frame = [](Memory& memory, X86Context& ctx, std::initializer_list<uint32_t> args) {
  ctx.esp = stack;
  ctx.eax = sentinel;
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
  ctx.ebx = 0xCAFEBABE;
  const auto invoke = [&](uint32_t ordinal, std::initializer_list<uint32_t> args) {
    frame(memory, ctx, args);
    browser::InvokeDispatcherObject(handles, memory, ctx, ordinal);
    check(ctx.eax == 0 && ctx.esp == stack + (args.size() + 1) * 4 && ctx.ebx == 0xCAFEBABE,
      "semaphore ABI publishes success and exact stdcall cleanup only");
  };
  const auto reject = [&](uint32_t ordinal, std::initializer_list<uint32_t> args) {
    frame(memory, ctx, args);
    const auto active = handles.ActiveHandles();
    const auto objects = dispatcher.ActiveObjects();
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "invalid semaphore ABI rejects");
    check(ctx.eax == sentinel && ctx.esp == stack && handles.ActiveHandles() == active && dispatcher.ActiveObjects() == objects,
      "failed semaphore ABI preserves registers handles and object count");
  };
  const auto poll = [&](uint32_t handle) {
    return handles.WaitGuestStatus(memory, ctx, std::span(&handle, 1), Dispatcher::WaitMode::Any, timeout, false, 2s);
  };
  invoke(193, {output, 0, 2, 3});
  const auto semaphore = readWord(memory, ctx, output);
  const auto alias = handles.Duplicate(semaphore);
  check(poll(semaphore) == 0 && poll(alias) == 0 && poll(semaphore) == 0x102, "creation exposes exact shared permit count");
  invoke(222, {alias, 2, output});
  check(readWord(memory, ctx, output) == 0, "release returns previous zero count");
  invoke(222, {semaphore, 1, output});
  check(readWord(memory, ctx, output) == 2, "release returns full signed previous count");
  for (const auto count : {0u, UINT32_MAX, 0x80000000u, 1u, 0x7FFFFFFFu}) { reject(222, {semaphore, count, output}); }
  check(readWord(memory, ctx, output) == 2, "invalid release preserves previous output");
  check(poll(alias) == 0 && poll(alias) == 0 && poll(alias) == 0 && poll(alias) == 0x102,
    "failed release never adds or consumes permits");
  invoke(222, {alias, 1, 0});
  check(poll(semaphore) == 0 && poll(semaphore) == 0x102, "optional previous count output");
  for (const auto pointer : {0u, 0xFFFFu, 0x12FFEu, 0xFFFFFFFEu}) { reject(193, {pointer, 0, 1, 2}); }
  for (const auto pointer : {0xFFFFu, 0x12FFEu, 0xFFFFFFFEu}) { reject(222, {alias, 1, pointer}); }
  check(poll(alias) == 0x102, "invalid output cannot release a permit");
  for (const auto attributes : {1u, output, UINT32_MAX}) { reject(193, {output, attributes, 1, 2}); }
  for (const auto counts : {std::pair{UINT32_MAX, 2u}, std::pair{0x80000000u, 2u}, std::pair{3u, 2u},
    std::pair{0u, 0u}, std::pair{0u, UINT32_MAX}, std::pair{0u, 0x80000000u}}) {
    reject(193, {output, 0, counts.first, counts.second});
  }
  for (const auto handle : {0u, 4u, UINT32_MAX}) { reject(222, {handle, 1, output}); }
  const auto event = handles.CreateEvent(true, true);
  const auto mutant = handles.CreateMutant(4);
  const auto timer = handles.CreateTimer(false);
  for (const auto handle : {event, mutant, timer}) { reject(222, {handle, 1, output}); }
  check(poll(event) == 0 && handles.ReleaseMutant(mutant, 4) == 0, "wrong kind leaves unrelated signals and owner intact");
  for (const auto handle : {event, mutant, timer}) { handles.Close(handle); }
  for (const auto ordinal : {193u, 222u}) {
    frame(memory, ctx, {output, 0, 1, 2});
    auto lease = memory.Borrow();
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "inherited lease rejects semaphore ABI");
    check(ctx.eax == sentinel && ctx.esp == stack, "inherited lease preserves registers");
  }
  for (const auto ordinal : {193u, 222u}) {
    for (const auto pointer : {0xFFFFu, 0x12FF9u, 0xFFFFFFF0u}) {
      frame(memory, ctx, {output, 0, 1, 2});
      ctx.esp = pointer;
      fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "invalid semaphore frame rejects");
      check(ctx.eax == sentinel && ctx.esp == pointer, "invalid frame preserves registers");
    }
  }
  memory.Decommit(0x11000, 0x1000);
  reject(193, {output, 0, 1, 2});
  reject(222, {alias, 1, output});
  reject(193, {0x10FFE, 0, 1, 2});
  reject(222, {alias, 1, 0x10FFE});
  memory.Commit(0x11000, 0x1000);
  check(poll(alias) == 0x102, "uncommitted and partially committed outputs preserve permits");
  invoke(193, {stack + 12, 0, 1, 2});
  const auto overlapping = readWord(memory, ctx, stack + 12);
  check(poll(overlapping) == 0 && poll(overlapping) == 0x102, "creation snapshots initial count before overlapping output");
  invoke(222, {overlapping, 2, stack + 8});
  check(readWord(memory, ctx, stack + 8) == 0 && poll(overlapping) == 0 && poll(overlapping) == 0 && poll(overlapping) == 0x102,
    "release snapshots count before overlapping output");
  handles.Close(overlapping);
  memory.Map(0xFFFFF000, 0x1000);
  memory.Commit(0xFFFFF000, 0x1000);
  invoke(193, {0xFFFFFFFC, 0, 0, 0x7FFFFFFF});
  const auto maximum = readWord(memory, ctx, 0xFFFFFFFC);
  invoke(222, {maximum, 0x7FFFFFFE, 0xFFFFFFFC});
  check(readWord(memory, ctx, 0xFFFFFFFC) == 0, "last address space dword supports previous count output");
  invoke(222, {maximum, 1, output});
  check(readWord(memory, ctx, output) == 0x7FFFFFFE, "signed maximum arithmetic preserves exact previous count");
  reject(222, {maximum, 1, output});
  handles.Close(maximum);
  for (const auto ordinal : {193u, 222u}) {
    const size_t words = ordinal == 193 ? 5 : 4;
    const uint32_t address = uint32_t(0x100000000ull - words * 4);
    for (size_t n = 0; n < words; ++n) { writeWord(memory, ctx, address + n * 4, 0); }
    ctx.esp = address;
    ctx.eax = sentinel;
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "valid final byte frame cannot wrap cleanup");
    check(ctx.eax == sentinel && ctx.esp == address, "cleanup overflow preserves registers");
  }
  for (size_t n = 0; n < 100; ++n) {
    frame(memory, ctx, {alias, 1, output});
    bool completed = false;
    std::jthread mutation([&] {
      try { browser::InvokeDispatcherObject(handles, memory, ctx, 222); completed = true; }
      catch (const browser::Fault&) {}
    });
    memory.Decommit(0x11000, 0x1000);
    mutation.join();
    check(poll(alias) == (completed ? 0u : 0x102u) && poll(alias) == 0x102,
      "failed publication cannot release permits during output retirement race");
    check(ctx.eax == (completed ? 0u : sentinel) && ctx.esp == (completed ? stack + 16 : stack),
      "output retirement race publishes registers only on complete release");
    memory.Commit(0x11000, 0x1000);
  }
  std::array<uint32_t, browser::kDispatcherCapacity - 2> capacity{};
  for (auto& handle : capacity) { handle = handles.Duplicate(alias); }
  writeWord(memory, ctx, output, sentinel);
  reject(193, {output, 0, 1, 2});
  check(readWord(memory, ctx, output) == sentinel, "handle capacity rejects before creating a semaphore");
  for (const auto handle : capacity) { handles.Close(handle); }
  handles.Close(alias);
  const auto retained = dispatcher.CreateMutant(4);
  for (auto& handle : capacity) { handle = handles.CreateEvent(false, false); }
  reject(193, {output, 0, 1, 2});
  check(readWord(memory, ctx, output) == sentinel, "object capacity failure preserves output with an available handle slot");
  for (const auto handle : capacity) { handles.Close(handle); }
  dispatcher.Destroy(retained);
  handles.Close(semaphore);
  reject(222, {alias, 1, output});
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0, "semaphore contracts retire all state");
};

DispatcherHandles* thunkHandles = nullptr;
const auto releaseThunk = [](X86Context& ctx, uint8_t* base) {
  browser::InvokeDispatcherObject(*thunkHandles, Memory::From(base), ctx, 222);
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
  frame(memory, producer, {output, 0, 0, 1});
  browser::InvokeDispatcherObject(handles, memory, producer, 193);
  const auto semaphore = readWord(memory, producer, output);
  const auto alias = handles.Duplicate(semaphore);
  frame(memory, consumer, {semaphore, 0, 0});
  std::jthread waiter([&] { browser::InvokeDispatcherWait(handles, memory, consumer, 233, 2s); });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (dispatcher.ActiveWaiters() != 1 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  check(dispatcher.ActiveWaiters() == 1, "semaphore wait ABI reaches blocking boundary");
  handles.Close(semaphore);
  thunkHandles = &handles;
  runtime.RegisterKernel(222, releaseThunk);
  frame(memory, producer, {alias, 1, output});
  runtime.Invoke(producer, memory.Base(), runtime.KernelTarget(222));
  waiter.join();
  check(producer.eax == 0 && producer.esp == stack + 16 && readWord(memory, producer, output) == 0,
    "registered release thunk returns exact previous count and cleanup");
  check(consumer.eax == 0 && consumer.esp == stack + 16, "release completes independently blocked original closed handle wait");
  handles.Close(alias);
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0,
    "semaphore handoff releases all handle and operation pins");
  thunkHandles = nullptr;
};

const auto cancellationContracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  setup(memory);
  X86Context ctx{};
  const auto semaphore = handles.CreateSemaphore(0, 1);
  runtime.Stop();
  for (const auto ordinal : {193u, 222u}) {
    frame(memory, ctx, ordinal == 193 ? std::initializer_list<uint32_t>{output, 0, 1, 2}
      : std::initializer_list<uint32_t>{semaphore, 1, output});
    writeWord(memory, ctx, output, sentinel);
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "cancelled semaphore operation rejects");
    check(ctx.eax == sentinel && ctx.esp == stack && readWord(memory, ctx, output) == sentinel && handles.ActiveHandles() == 1,
      "cancellation preserves output registers and handle table");
  }
  frame(memory, ctx, {semaphore});
  browser::InvokeDispatcherObject(handles, memory, ctx, 187);
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0, "close after cancellation retires semaphore");
};

int main() {
  try {
    contracts();
    handoffContracts();
    cancellationContracts();
    std::puts("dispatcher-semaphore-abi-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
