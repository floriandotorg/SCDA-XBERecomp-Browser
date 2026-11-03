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
const auto frame = [](Memory& memory, X86Context& ctx, uint32_t source, uint32_t target, uint32_t options) {
  ctx.esp = stack;
  ctx.eax = sentinel;
  writeWord(memory, ctx, stack, 0x12345678);
  writeWord(memory, ctx, stack + 4, source);
  writeWord(memory, ctx, stack + 8, target);
  writeWord(memory, ctx, stack + 12, options);
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
  const auto duplicate = [&](uint32_t source, uint32_t options = 0, uint32_t target = output) {
    frame(memory, ctx, source, target, options);
    browser::InvokeDispatcherObject(handles, memory, ctx, 197);
    check(ctx.eax == 0 && ctx.esp == stack + 16 && ctx.ebx == 0xCAFEBABE,
      "duplicate publishes exact stdcall success and preserves other registers");
    return readWord(memory, ctx, target);
  };
  const auto reject = [&](uint32_t source, uint32_t target, uint32_t options) {
    frame(memory, ctx, source, target, options);
    const auto active = handles.ActiveHandles();
    const auto objects = dispatcher.ActiveObjects();
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, 197); }, "invalid duplication rejects");
    check(ctx.eax == sentinel && ctx.esp == stack && handles.ActiveHandles() == active && dispatcher.ActiveObjects() == objects,
      "failed duplication preserves completion registers and budgets");
  };
  const auto poll = [&](uint32_t handle) {
    return handles.WaitGuestStatus(memory, ctx, std::span(&handle, 1), Dispatcher::WaitMode::Any, timeout, false, 2s);
  };
  const auto event = handles.CreateEvent(false, true);
  const auto alias = duplicate(event);
  check(alias != event && handles.ActiveHandles() == 2 && dispatcher.ActiveObjects() == 1, "duplication shares one object");
  check(poll(alias) == 0 && poll(event) == 0x102, "duplicated event consumes the same signal");
  handles.SetEvent(event);
  const auto moved = duplicate(event, 1);
  fault([&] { handles.SetEvent(event); }, "close source makes original identifier unavailable");
  check(handles.ActiveHandles() == 2 && dispatcher.ActiveObjects() == 1 && poll(moved) == 0,
    "close source retains aliases and the original object");
  handles.Close(alias);
  handles.Close(moved);
  const auto semaphore = handles.CreateSemaphore(2, 3);
  const auto semaphoreAlias = duplicate(semaphore, 1);
  check(poll(semaphoreAlias) == 0 && poll(semaphoreAlias) == 0 && poll(semaphoreAlias) == 0x102,
    "duplication does not copy semaphore permits");
  handles.Close(semaphoreAlias);
  const auto timer = handles.CreateTimer(false);
  handles.SetTimer(timer, 5);
  const auto timerAlias = duplicate(timer, 1);
  dispatcher.AdvanceTime(5);
  check(poll(timerAlias) == 0 && poll(timerAlias) == 0x102, "close source preserves timer schedule");
  handles.Close(timerAlias);
  ctx.thread_handle = 4;
  const auto mutant = handles.CreateMutant(4);
  const auto mutantAlias = duplicate(mutant, 1);
  check(poll(mutantAlias) == 0 && handles.ReleaseMutant(mutantAlias, 4) == -1 && handles.ReleaseMutant(mutantAlias, 4) == 0,
    "duplication retains recursive ownership rather than assigning a new owner");
  handles.Close(mutantAlias);
  const auto source = handles.CreateEvent(false, true);
  writeWord(memory, ctx, output, sentinel);
  for (const auto invalid : {0u, UINT32_MAX, event, semaphore, timer, mutant}) { reject(invalid, output, 1); }
  for (const auto options : {2u, 3u, 4u, 0x100u, UINT32_MAX}) { reject(source, output, options); }
  check(readWord(memory, ctx, output) == sentinel && poll(source) == 0, "invalid inputs preserve output and source signal");
  for (const auto address : {0u, 0xFFFFu, 0x12FFEu, 0xFFFFFFFEu}) { reject(source, address, 1); }
  memory.Decommit(0x11000, 0x1000);
  reject(source, output, 1);
  memory.Commit(0x11000, 0x1000);
  frame(memory, ctx, source, output, 1);
  {
    auto lease = memory.Borrow();
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, 197); }, "duplication rejects inherited backing lease");
  }
  for (const auto address : {0xFFFFu, 0x12FFDu, 0xFFFFFFFCu}) {
    ctx.esp = address;
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, 197); }, "duplication rejects malformed stack");
    check(ctx.eax == sentinel && ctx.esp == address, "malformed stack preserves completion registers");
  }
  for (const auto address : {stack + 4, stack + 8, stack + 12}) {
    const auto overlap = duplicate(source, 0, address);
    check(overlap != source, "output overlapping captured frame cannot redirect duplication");
    handles.Close(overlap);
  }
  memory.Map(0xFFFFF000, 0x1000);
  memory.Commit(0xFFFFF000, 0x1000);
  handles.Close(duplicate(source, 0, 0xFFFFFFFC));
  for (size_t n = 0; n < 4; ++n) { writeWord(memory, ctx, 0xFFFFFFF0 + n * 4, 0); }
  ctx.esp = 0xFFFFFFF0;
  ctx.eax = sentinel;
  fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, 197); }, "complete final frame cannot wrap cleanup");
  check(ctx.esp == 0xFFFFFFF0 && ctx.eax == sentinel, "cleanup overflow preserves registers");
  std::array<uint32_t, browser::kDispatcherCapacity - 1> capacity{};
  for (auto& handle : capacity) { handle = handles.Duplicate(source); }
  writeWord(memory, ctx, output, sentinel);
  reject(source, output, 0);
  reject(source, output, 1);
  check(readWord(memory, ctx, output) == sentinel, "capacity failure preserves output even with close source");
  handles.SetEvent(source);
  check(poll(source) == 0, "capacity failure never closes source");
  for (const auto handle : capacity) { handles.Close(handle); }
  handles.Close(source);
  const auto overlappingSource = handles.CreateEvent(false, true);
  const auto overlappingTarget = duplicate(overlappingSource, 1, stack + 4);
  fault([&] { handles.SetEvent(overlappingSource); }, "overlapping output cannot change captured close source");
  check(poll(overlappingTarget) == 0, "overlapping transfer retains source signal");
  handles.Close(overlappingTarget);
  const auto owned = handles.CreateMutant(4);
  handles.Close(owned);
  for (auto& handle : capacity) { handle = handles.CreateEvent(false, true); }
  check(dispatcher.ActiveObjects() == browser::kDispatcherCapacity && handles.ActiveHandles() == browser::kDispatcherCapacity - 1, "object capacity can be full with a free handle");
  const auto atObjectCapacity = duplicate(capacity[0]);
  check(dispatcher.ActiveObjects() == browser::kDispatcherCapacity && handles.ActiveHandles() == browser::kDispatcherCapacity && poll(atObjectCapacity) == 0,
    "duplication needs a handle slot but never allocates another object");
  handles.Close(atObjectCapacity);
  for (const auto handle : capacity) { handles.Close(handle); }
  handles.AbandonOwner(4);
  for (size_t n = 0; n < 100; ++n) {
    const auto racing = handles.CreateEvent(false, true);
    frame(memory, ctx, racing, output, 1);
    bool completed = false;
    std::jthread mutation([&] {
      try { browser::InvokeDispatcherObject(handles, memory, ctx, 197); completed = true; }
      catch (const browser::Fault&) {}
    });
    memory.Decommit(0x11000, 0x1000);
    mutation.join();
    check(handles.ActiveHandles() == 1 && dispatcher.ActiveObjects() == 1,
      "output retirement never loses or leaks a handle");
    check(ctx.eax == (completed ? 0u : sentinel) && ctx.esp == (completed ? stack + 16 : stack),
      "output retirement publishes completion only on success");
    memory.Commit(0x11000, 0x1000);
    if (completed) {
      fault([&] { handles.SetEvent(racing); }, "successful race closes original");
      const auto replacement = racing + 4;
      check(poll(replacement) == 0, "successful race retains target after output storage is retired");
      handles.Close(replacement);
    } else {
      check(poll(racing) == 0, "failed race preserves usable source");
      handles.Close(racing);
    }
  }
  for (size_t n = 0; n < 100; ++n) {
    const auto racing = handles.CreateEvent(false, true);
    frame(memory, ctx, racing, output, 1);
    bool completed = false;
    bool closed = false;
    std::jthread mutation([&] {
      try { browser::InvokeDispatcherObject(handles, memory, ctx, 197); completed = true; }
      catch (const browser::Fault&) {}
    });
    try { handles.Close(racing); closed = true; } catch (const browser::Fault&) {}
    mutation.join();
    check(completed != closed, "close and transfer have exactly one linearized winner");
    if (completed) {
      const auto replacement = readWord(memory, ctx, output);
      check(poll(replacement) == 0, "racing close cannot retire transferred object");
      handles.Close(replacement);
    }
    check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0, "close race leaves no orphan reference");
  }
  const auto stopped = handles.CreateEvent(false, true);
  runtime.Stop();
  writeWord(memory, ctx, output, sentinel);
  reject(stopped, output, 0);
  reject(stopped, output, 1);
  check(readWord(memory, ctx, output) == sentinel, "stopped duplication preserves output");
  handles.Close(stopped);
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0, "duplication fixture retires all objects");
};

DispatcherHandles* thunkHandles = nullptr;
const auto duplicateThunk = [](X86Context& ctx, uint8_t* base) {
  browser::InvokeDispatcherObject(*thunkHandles, Memory::From(base), ctx, 197);
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
  const auto event = handles.CreateEvent(false, false);
  consumer.esp = stack;
  consumer.eax = sentinel;
  writeWord(memory, consumer, stack, 0x12345678);
  writeWord(memory, consumer, stack + 4, event);
  writeWord(memory, consumer, stack + 8, 0);
  writeWord(memory, consumer, stack + 12, 0);
  std::jthread waiter([&] { browser::InvokeDispatcherWait(handles, memory, consumer, 233, 2s); });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (dispatcher.ActiveWaiters() != 1 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  check(dispatcher.ActiveWaiters() == 1, "wait blocks independently before duplication");
  frame(memory, producer, event, output, 1);
  thunkHandles = &handles;
  runtime.RegisterKernel(197, duplicateThunk);
  runtime.Invoke(producer, memory.Base(), runtime.KernelTarget(197));
  const auto alias = readWord(memory, producer, output);
  fault([&] { handles.SetEvent(event); }, "registered transfer closes waited handle");
  handles.SetEvent(alias);
  handles.Close(alias);
  waiter.join();
  check(producer.eax == 0 && producer.esp == stack + 16 && consumer.eax == 0 && consumer.esp == stack + 16,
    "registered duplication and independently pinned wait preserve their stdcall frames");
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0,
    "handoff retires target and operation pins");
  thunkHandles = nullptr;
};

int main() {
  try {
    contracts();
    handoffContracts();
    std::puts("dispatcher-duplicate-abi-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
