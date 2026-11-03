#include "browser/dispatcher-wait-abi.h"
#include <algorithm>
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
const auto parked = [](Dispatcher& dispatcher) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (dispatcher.ActiveWaiters() != 1 && std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
  check(dispatcher.ActiveWaiters() == 1, "ABI wait reaches independent blocking boundary");
};
const auto writeWords = [](Memory& memory, const X86Context& ctx, uint32_t address, std::span<const uint32_t> words) {
  std::array<uint8_t, 256> bytes{};
  check(words.size_bytes() <= bytes.size(), "bounded fixture words");
  for (size_t n = 0; n < words.size(); ++n) {
    for (size_t byte = 0; byte < 4; ++byte) { bytes[n * 4 + byte] = uint8_t(words[n] >> (byte * 8)); }
  }
  memory.Write(ctx, address, bytes.data(), words.size_bytes());
};
constexpr uint32_t stack = 0x10003;
constexpr uint32_t arrayAddress = 0x11003;
constexpr uint32_t timeoutAddress = 0x12003;
const auto frame = [](Memory& memory, X86Context& ctx, std::initializer_list<uint32_t> args) {
  std::array<uint32_t, 7> words{};
  words[0] = 0x12345678;
  std::copy(args.begin(), args.end(), words.begin() + 1);
  ctx.esp = stack;
  ctx.eax = 0xDEADBEEF;
  writeWords(memory, ctx, stack, std::span(words.data(), args.size() + 1));
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
  const auto first = handles.CreateEvent(false, false);
  const auto second = handles.CreateSemaphore(0, 1);
  const std::array<uint32_t, 2> set{first, second};
  writeWords(memory, ctx, arrayAddress, set);
  const auto invoke = [&](uint32_t ordinal, std::chrono::milliseconds watchdog = 2s) {
    browser::InvokeDispatcherWait(handles, memory, ctx, ordinal, watchdog);
    check(ctx.ebx == 0xCAFEBABE, "ABI preserves unrelated guest registers");
  };
  handles.SetEvent(first);
  frame(memory, ctx, {first, 0x100, timeoutAddress});
  invoke(233);
  check(ctx.eax == 0 && ctx.esp == stack + 16, "single wait decodes low BOOLEAN byte and returns with three argument stdcall cleanup");
  frame(memory, ctx, {first, 0xD203F501, 0xFFFFFF00, timeoutAddress});
  invoke(234);
  check(ctx.eax == 0x102 && ctx.esp == stack + 20, "extended single wait accepts nonalertable user mode and cleans four arguments");
  handles.ReleaseSemaphore(second, 1);
  frame(memory, ctx, {2, arrayAddress, 1, 0, 0, timeoutAddress});
  invoke(235);
  check(ctx.eax == 1 && ctx.esp == stack + 28, "multiple wait decodes WaitAny as one and preserves original index");
  handles.SetEvent(first);
  handles.ReleaseSemaphore(second, 1);
  frame(memory, ctx, {2, arrayAddress, 0, 0xD203F501, 0xFFFFFF00, timeoutAddress});
  invoke(235);
  check(ctx.eax == 0 && ctx.esp == stack + 28, "WaitAll zero consumes full set and returns success");
  {
    handles.SetEvent(first);
    const std::array<uint32_t, 2> relative{0xFFFFFFFD, UINT32_MAX};
    writeWords(memory, ctx, timeoutAddress, relative);
    frame(memory, ctx, {2, arrayAddress, 0, 0, 0, timeoutAddress});
    std::jthread waiter([&] { invoke(235); });
    parked(dispatcher);
    memory.Decommit(0x10000, 0x3000);
    handles.Close(second);
    dispatcher.AdvanceTime(3);
    waiter.join();
    check(ctx.eax == 0x102 && ctx.esp == stack + 28 && dispatcher.ActiveObjects() == 1, "stack, array and timeout backing may disappear while pending and final close releases on real guest timeout");
    memory.Commit(0x10000, 0x3000);
    frame(memory, ctx, {first, 0, timeoutAddress});
    invoke(233);
    check(ctx.eax == 0, "incomplete ABI all wait preserved first signal");
  }
  {
    const auto alias = handles.Duplicate(first);
    const auto replacement = handles.CreateEvent(false, true);
    const std::array<uint32_t, 1> original{first};
    writeWords(memory, ctx, arrayAddress, original);
    frame(memory, ctx, {1, arrayAddress, 1, 0, 0, 0});
    std::jthread waiter([&] { invoke(235); });
    parked(dispatcher);
    const std::array<uint32_t, 1> changed{replacement};
    writeWords(memory, ctx, arrayAddress, changed);
    const std::array<uint32_t, 7> changedFrame{};
    writeWords(memory, ctx, stack, changedFrame);
    handles.Close(first);
    handles.SetEvent(alias);
    handles.Close(alias);
    waiter.join();
    check(ctx.eax == 0 && ctx.esp == stack + 28 && dispatcher.ActiveObjects() == 1, "ABI snapshots stack and handles and pins original through final close");
    frame(memory, ctx, {replacement, 0, timeoutAddress});
    invoke(233);
    check(ctx.eax == 0, "replacement signal cannot satisfy or be consumed by old ABI wait");
    handles.Close(replacement);
  }
  {
    const auto event = handles.CreateEvent(false, true);
    const auto alias = handles.Duplicate(event);
    const auto reject = [&](uint32_t ordinal) {
      const auto savedStack = ctx.esp;
      fault([&] { invoke(ordinal); }, "malformed ABI request rejects");
      check(ctx.eax == 0xDEADBEEF && ctx.esp == savedStack && dispatcher.ActiveWaiters() == 0, "failed ABI preflight leaves result and stack unchanged");
    };
    for (const auto ordinal : {0u, 158u, 159u, 236u}) {
      frame(memory, ctx, {event, 0, timeoutAddress});
      reject(ordinal);
    }
    for (const auto pointer : {0xFFFFu, 0x12FF1u, 0xFFFFFFF0u}) {
      frame(memory, ctx, {event, 0, timeoutAddress});
      ctx.esp = pointer;
      reject(233);
    }
    for (const auto count : {0u, 65u, UINT32_MAX}) {
      frame(memory, ctx, {count, 0xFFFFFFFC, 1, 0, 0, timeoutAddress});
      reject(235);
    }
    for (const auto pointer : {0u, 0xFFFFu, 0x12FFDu, 0xFFFFFFFCu}) {
      frame(memory, ctx, {2, pointer, 1, 0, 0, timeoutAddress});
      reject(235);
    }
    const std::array<uint32_t, 2> aliases{event, alias};
    const std::array<uint32_t, 2> unknown{event, 0};
    for (const auto& invalid : {aliases, unknown}) {
      writeWords(memory, ctx, arrayAddress, invalid);
      frame(memory, ctx, {2, arrayAddress, 1, 0, 0, timeoutAddress});
      reject(235);
    }
    const std::array<uint32_t, 1> valid{event};
    writeWords(memory, ctx, arrayAddress, valid);
    for (const auto type : {2u, UINT32_MAX}) {
      frame(memory, ctx, {1, arrayAddress, type, 0, 0, timeoutAddress});
      reject(235);
    }
    frame(memory, ctx, {0, 0, timeoutAddress});
    reject(233);
    frame(memory, ctx, {event, 2, 0, timeoutAddress});
    reject(234);
    frame(memory, ctx, {1, arrayAddress, 1, 2, 0, timeoutAddress});
    reject(235);
    frame(memory, ctx, {event, 0x101, timeoutAddress});
    reject(233);
    frame(memory, ctx, {event, 0, 1, timeoutAddress});
    reject(234);
    frame(memory, ctx, {1, arrayAddress, 1, 0, 1, timeoutAddress});
    reject(235);
    frame(memory, ctx, {event, 0, 0xFFFFFFFC});
    reject(233);
    frame(memory, ctx, {event, 0, timeoutAddress});
    {
      auto lease = memory.Borrow();
      reject(233);
    }
    fault([&] { invoke(233, 0ms); }, "invalid watchdog preserves ready signal");
    const std::array<uint32_t, 4> singleFrame{0x12345678, event, 0, timeoutAddress};
    writeWords(memory, ctx, 0x10FF4, singleFrame);
    ctx.esp = 0x10FF4;
    memory.Decommit(0x11000, 0x1000);
    reject(233);
    memory.Commit(0x11000, 0x1000);
    const std::array<uint32_t, 2> crossing{event, alias};
    writeWords(memory, ctx, 0x11FFC, crossing);
    frame(memory, ctx, {2, 0x11FFC, 1, 0, 0, timeoutAddress});
    memory.Decommit(0x12000, 0x1000);
    reject(235);
    memory.Commit(0x12000, 0x1000);
    memory.Map(0xFFFFF000, 0x1000);
    memory.Commit(0xFFFFF000, 0x1000);
    writeWords(memory, ctx, 0xFFFFFFF0, singleFrame);
    ctx.esp = 0xFFFFFFF0;
    reject(233);
    frame(memory, ctx, {event, 0, timeoutAddress});
    writeWords(memory, ctx, 0x12FF0, singleFrame);
    ctx.esp = 0x12FF0;
    invoke(233);
    check(ctx.eax == 0 && ctx.esp == 0x13000, "every rejected ABI request preserves ready signal and exact stack extent needs no trailing backing");
    frame(memory, ctx, {event, 0, 0});
    bool watchdog = false;
    try { invoke(233, 5ms); }
    catch (const browser::WaitWatchdogExpired&) { watchdog = true; }
    check(watchdog && ctx.eax == 0xDEADBEEF && ctx.esp == stack, "ABI watchdog fault publishes neither guest timeout nor stack cleanup");
    handles.Close(event);
    handles.Close(alias);
  }
  {
    std::array<uint32_t, 64> boundary{};
    for (auto& handle : boundary) { handle = handles.CreateEvent(false, false); }
    handles.SetEvent(boundary.back());
    writeWords(memory, ctx, arrayAddress, boundary);
    frame(memory, ctx, {64, arrayAddress, 1, 0, 0, timeoutAddress});
    invoke(235);
    check(ctx.eax == 63, "guest array boundary preserves highest supported result index");
    handles.SetEvent(boundary.back());
    writeWords(memory, ctx, 0xFFFFFFFC, std::span(&boundary.back(), 1));
    frame(memory, ctx, {1, 0xFFFFFFFC, 1, 0, 0, timeoutAddress});
    invoke(235);
    check(ctx.eax == 0, "last address space dword is a valid bounded handle array");
    for (const auto handle : boundary) { handles.Close(handle); }
  }
  check(handles.ActiveHandles() == 0 && dispatcher.ActiveObjects() == 0 && dispatcher.ActiveWaiters() == 0, "ABI contracts release all state");
};

DispatcherHandles* thunkHandles = nullptr;
std::atomic<bool> cancelled = false;
const auto thunkWait = [](X86Context& ctx, uint8_t* base) {
  try { browser::InvokeDispatcherWait(*thunkHandles, Memory::From(base), ctx, 235, 2s); }
  catch (const browser::Cancelled&) { cancelled = true; throw; }
};
const auto runtimeContracts = [] {
  Runtime runtime({}, {});
  Dispatcher dispatcher(runtime);
  DispatcherHandles handles(dispatcher);
  Memory memory;
  setup(memory);
  X86Context ctx{};
  ctx.host = &runtime;
  const auto event = handles.CreateEvent(false, true);
  const std::array<uint32_t, 1> set{event};
  writeWords(memory, ctx, arrayAddress, set);
  thunkHandles = &handles;
  runtime.RegisterKernel(235, thunkWait);
  frame(memory, ctx, {1, arrayAddress, 1, 0, 0, timeoutAddress});
  runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(235));
  check(ctx.eax == 0 && ctx.esp == stack + 28, "explicit test registration traverses kernel dispatch with checked real argument frame");
  frame(memory, ctx, {1, arrayAddress, 1, 0, 0, 0});
  runtime.Launch(ctx, memory.Base(), thunkWait);
  parked(dispatcher);
  handles.Close(event);
  check(!runtime.Stop() && cancelled && runtime.ActiveWorkers() == 0 && dispatcher.ActiveWaiters() == 0 && dispatcher.ActiveObjects() == 0, "Stop unwinds ABI snapshots and final closed handle pin and joins owned worker");
  thunkHandles = nullptr;
};

int main() {
  try {
    contracts();
    runtimeContracts();
    std::puts("dispatcher-wait-abi-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
