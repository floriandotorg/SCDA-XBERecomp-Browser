#include "browser/dispatcher-object-abi.h"
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
const auto frame = [](Memory& memory, X86Context& ctx, uint32_t handle, uint32_t target) {
  ctx.esp = stack;
  ctx.eax = sentinel;
  writeWord(memory, ctx, stack, 0x12345678);
  writeWord(memory, ctx, stack + 4, handle);
  writeWord(memory, ctx, stack + 8, target);
};

DispatcherHandles* thunkHandles = nullptr;
const auto queryThunk = [](X86Context& ctx, uint8_t* base) {
  browser::InvokeDispatcherObject(*thunkHandles, Memory::From(base), ctx, 209);
};
const auto contracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  memory.Map(0x10000, 0x3000);
  memory.Commit(0x10000, 0x3000);
  X86Context ctx{};
  ctx.thread_handle = 4;
  ctx.ebx = 0xCAFEBABE;
  const auto query = [&](uint32_t ordinal, uint32_t handle, uint32_t target = output) {
    frame(memory, ctx, handle, target);
    browser::InvokeDispatcherObject(handles, memory, ctx, ordinal);
    check(ctx.eax == 0 && ctx.esp == stack + 12 && ctx.ebx == 0xCAFEBABE && ctx.thread_handle == 4,
      "query publishes exact stdcall completion and preserves other registers");
    return std::array{readWord(memory, ctx, target), readWord(memory, ctx, target + 4)};
  };
  const auto poll = [&](uint32_t handle) {
    return handles.WaitGuestStatus(memory, ctx, std::span(&handle, 1), Dispatcher::WaitMode::Any, timeout, false, 2s);
  };
  const auto event = handles.CreateEvent(false, true);
  const auto notification = handles.CreateEvent(true, false);
  const auto semaphore = handles.CreateSemaphore(2, 3);
  const auto mutant = handles.CreateMutant();
  const auto timer = handles.CreateTimer(false);
  check(query(209, event) == std::array{1u, 1u}, "synchronization event type and signal snapshot");
  check(query(209, event) == std::array{1u, 1u} && poll(event) == 0 && query(209, event)[1] == 0,
    "query never consumes an event signal");
  check(query(209, notification) == std::array{0u, 0u}, "notification event type is zero");
  handles.SetEvent(notification);
  check(query(209, notification) == std::array{0u, 1u}, "query observes event changes");
  check(query(214, semaphore) == std::array{2u, 3u} && poll(semaphore) == 0 && query(214, semaphore) == std::array{1u, 3u},
    "query observes counts without taking a permit");
  handles.ReleaseSemaphore(semaphore, 2);
  check(query(214, semaphore) == std::array{3u, 3u}, "query preserves maximum count");
  check(query(213, mutant) == std::array{1u, 0u}, "unowned mutant count and flags");
  check(poll(mutant) == 0 && query(213, mutant) == std::array{0u, 1u}, "mutant ownership belongs to context identity");
  check(poll(mutant) == 0 && query(213, mutant) == std::array{UINT32_MAX, 1u}, "recursive count is signed");
  ctx.thread_handle = 8;
  frame(memory, ctx, mutant, output);
  browser::InvokeDispatcherObject(handles, memory, ctx, 213);
  check(readWord(memory, ctx, output) == UINT32_MAX && readWord(memory, ctx, output + 4) == 0,
    "other context is not the owner");
  ctx.thread_handle = 0;
  frame(memory, ctx, mutant, output);
  browser::InvokeDispatcherObject(handles, memory, ctx, 213);
  check(readWord(memory, ctx, output + 4) == 0, "zero context identity is never an owner");
  ctx.thread_handle = 4;
  check(handles.AbandonOwner(4) == 1 && query(213, mutant) == std::array{1u, 0x100u},
    "abandonment occupies one byte and padding is zero");
  check(query(213, mutant) == std::array{1u, 0x100u} && poll(mutant) == 0x80 && query(213, mutant) == std::array{0u, 1u},
    "query does not acknowledge abandonment or acquire ownership");
  handles.ReleaseMutant(mutant, 4);
  const auto alias = handles.Duplicate(semaphore);
  handles.Close(semaphore);
  check(query(214, alias) == std::array{3u, 3u}, "query resolves shared state after source closure");
  const auto reject = [&](uint32_t ordinal, uint32_t handle, uint32_t target) {
    frame(memory, ctx, handle, target);
    const auto active = handles.ActiveHandles();
    const auto objects = dispatcher.ActiveObjects();
    fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "invalid query faults");
    check(ctx.eax == sentinel && ctx.esp == stack && handles.ActiveHandles() == active && dispatcher.ActiveObjects() == objects,
      "failed query preserves registers and object budgets");
  };
  for (const auto ordinal : {209u, 213u, 214u}) {
    for (const auto handle : {0u, UINT32_MAX, semaphore}) { reject(ordinal, handle, output); }
    for (const auto handle : {event, mutant, alias, timer}) {
      if ((ordinal == 209 && handle == event) || (ordinal == 213 && handle == mutant) || (ordinal == 214 && handle == alias)) { continue; }
      writeWord(memory, ctx, output, sentinel);
      writeWord(memory, ctx, output + 4, sentinel);
      reject(ordinal, handle, output);
      check(readWord(memory, ctx, output) == sentinel && readWord(memory, ctx, output + 4) == sentinel,
        "wrong type leaves the entire output unchanged");
    }
    const auto handle = ordinal == 209 ? event : (ordinal == 213 ? mutant : alias);
    for (const auto address : {0u, 0xFFFFu, 0x12FFCu, 0xFFFFFFFCu}) { reject(ordinal, handle, address); }
    writeWord(memory, ctx, 0x11FFC, sentinel);
    memory.Decommit(0x12000, 0x1000);
    reject(ordinal, handle, 0x11FFC);
    check(readWord(memory, ctx, 0x11FFC) == sentinel, "partial commitment cannot publish half an output");
    memory.Commit(0x12000, 0x1000);
    for (const auto address : {stack, stack + 4, stack + 8}) { query(ordinal, handle, address); }
    frame(memory, ctx, handle, output);
    {
      auto lease = memory.Borrow();
      fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "query rejects inherited lease");
    }
    for (const auto address : {0xFFFFu, 0x12FF9u, 0xFFFFFFFCu}) {
      ctx.esp = address;
      fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, ordinal); }, "query rejects malformed stack");
      check(ctx.esp == address && ctx.eax == sentinel, "malformed stack preserves completion registers");
    }
  }
  memory.Map(0xFFFFF000, 0x1000);
  memory.Commit(0xFFFFF000, 0x1000);
  query(209, event, 0xFFFFFFF8);
  for (size_t n = 0; n < 3; ++n) { writeWord(memory, ctx, 0xFFFFFFF4 + n * 4, 0); }
  ctx.esp = 0xFFFFFFF4;
  ctx.eax = sentinel;
  fault([&] { browser::InvokeDispatcherObject(handles, memory, ctx, 209); }, "query cannot wrap stdcall cleanup");
  check(ctx.esp == 0xFFFFFFF4 && ctx.eax == sentinel, "cleanup overflow preserves completion");
  thunkHandles = &handles;
  runtime.RegisterKernel(209, queryThunk);
  frame(memory, ctx, notification, output);
  runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(209));
  check(ctx.eax == 0 && ctx.esp == stack + 12 && readWord(memory, ctx, output + 4) == 1,
    "registered query thunk traverses runtime dispatch");
  thunkHandles = nullptr;
  X86Context owner{};
  owner.thread_handle = 4;
  std::jthread mutation([&](std::stop_token stop) {
    while (!stop.stop_requested()) {
      check(handles.WaitGuestStatus(memory, owner, std::span(&mutant, 1), Dispatcher::WaitMode::Any, timeout, false, 2s) == 0,
        "concurrent mutant owner acquires without contention");
      handles.ReleaseMutant(mutant, 4);
    }
  });
  for (size_t n = 0; n < 1000; ++n) {
    const auto information = query(213, mutant);
    check(information == std::array{1u, 0u} || information == std::array{0u, 1u},
      "count and ownership flags come from one locked snapshot");
  }
  mutation.request_stop();
  mutation.join();
  check(query(213, mutant) == std::array{1u, 0u}, "concurrent observation neither acquires nor strands ownership");
  for (size_t n = 0; n < 100; ++n) {
    const auto racing = handles.CreateEvent(false, true);
    frame(memory, ctx, racing, output);
    writeWord(memory, ctx, output, sentinel);
    writeWord(memory, ctx, output + 4, sentinel);
    bool completed = false;
    std::jthread reader([&] {
      try { browser::InvokeDispatcherObject(handles, memory, ctx, 209); completed = true; }
      catch (const browser::Fault&) {}
    });
    handles.Close(racing);
    reader.join();
    check(ctx.eax == (completed ? 0u : sentinel) && ctx.esp == (completed ? stack + 12 : stack),
      "concurrent close publishes completion only for a pinned query");
    check(readWord(memory, ctx, output) == (completed ? 1u : sentinel) && readWord(memory, ctx, output + 4) == (completed ? 1u : sentinel),
      "concurrent close publishes a full snapshot or nothing");
    check(handles.ActiveHandles() == 5 && dispatcher.ActiveObjects() == 5, "query pin retires after concurrent close");
  }
  for (size_t n = 0; n < 100; ++n) {
    frame(memory, ctx, event, output);
    bool completed = false;
    std::jthread reader([&] {
      try { browser::InvokeDispatcherObject(handles, memory, ctx, 209); completed = true; }
      catch (const browser::Fault&) {}
    });
    memory.Decommit(0x11000, 0x1000);
    reader.join();
    check(ctx.eax == (completed ? 0u : sentinel) && ctx.esp == (completed ? stack + 12 : stack),
      "output retirement and snapshot publication remain transactional");
    memory.Commit(0x11000, 0x1000);
  }
  runtime.Stop();
  for (const auto ordinal : {209u, 213u, 214u}) {
    reject(ordinal, ordinal == 209 ? event : (ordinal == 213 ? mutant : alias), output);
  }
  for (const auto handle : {event, notification, alias, mutant, timer}) { handles.Close(handle); }
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0,
    "query fixture retires all handles and objects");
};

int main() {
  try {
    contracts();
    std::puts("dispatcher-query-abi-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
