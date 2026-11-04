#include "browser/synchronization-provider.h"
#include "browser/thread-provider.h"
#include "browser/file-provider.h"
#include <atomic>
#include <chrono>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
std::atomic<uint32_t> sharedHandle = 0;
std::atomic<uint32_t> sharedEvent = 0;
std::atomic<bool> eventWaiting = false;
std::atomic<bool> eventAcquired = false;
std::atomic<bool> childReady = false;
std::atomic<bool> releaseChild = false;
std::atomic<bool> initialWaiting = false;
std::atomic<bool> closedWhileWaiting = false;
uint32_t mode = 0;

const auto read = [](X86Context& ctx, uint8_t* base, uint32_t at) {
  uint32_t value = 0;
  browser::Memory::From(base).Read(ctx, at, &value, 4);
  return value;
};
const auto invoke = [](X86Context& ctx, uint8_t* base, uint32_t ordinal, std::initializer_list<uint32_t> arguments) {
  auto& runtime = browser::Runtime::From(ctx);
  const auto stack = ctx.esp;
  const auto owner = ctx.thread_handle;
  std::array<uint32_t, 12> frame{};
  std::copy(arguments.begin(), arguments.end(), frame.begin() + 1);
  const auto size = uint32_t((arguments.size() + 1) * 4);
  const auto incoming = stack - size;
  browser::Memory::From(base).Write(ctx, incoming, frame.data(), size);
  ctx.esp = incoming;
  try { runtime.Kernel(ctx, base, ordinal); }
  catch (...) {
    check(ctx.esp == incoming && ctx.thread_handle == owner, "faulting synchronization changed stack or owner");
    ctx.esp = stack;
    throw;
  }
  check(ctx.esp == stack && ctx.thread_handle == owner, "synchronization changed stack or owner context");
  return ctx.eax;
};
const auto child = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  check(invoke(ctx, base, 192, {0x10104, 0, 1}) == 0, "child initial owner create failed");
  sharedHandle = read(ctx, base, 0x10104);
  check(invoke(ctx, base, 233, {sharedHandle, 0, 0x10200}) == 0, "recursive child acquisition failed");
  childReady = true;
  if (mode == 4) {
    while (!initialWaiting) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) { std::this_thread::yield(); }
    check(invoke(ctx, base, 187, {sharedHandle}) == 0, "child concurrent close failed");
    closedWhileWaiting = true;
  }
  if (sharedEvent) {
    eventWaiting = true;
    const uint32_t awaited = sharedEvent.load();
    browser::Memory::From(base).Write(ctx, 0x10150, &awaited, 4);
    check(invoke(ctx, base, 235, {1, 0x10150, 1, 0, 0, 0}) == 0, "child blocking multiple wait failed");
    eventAcquired = true;
  }
  while (!releaseChild) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
  if (mode == 1) { runtime.Call(ctx, base, runtime.KernelTarget(258), {73}); }
  if (mode == 2) { throw browser::Fault(ctx, 0, 0, "actual child fault"); }
  if (mode == 3) { runtime.Cancel(); runtime.Checkpoint(ctx); }
  ctx.eax = 42;
  ctx.esp += 8;
};
const auto entry = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  auto& memory = browser::Memory::From(base);
  const uint64_t zero = 0;
  memory.Write(ctx, 0x10200, &zero, 8);
  const auto create = [&](uint32_t output, uint32_t attributes = 0, uint32_t initial = 0) {
    return invoke(ctx, base, 192, {output, attributes, initial});
  };
  const std::string path = "\\Device\\Harddisk0\\Partition1\\DATA";
  memory.Write(ctx, 0x10300, path.data(), path.size());
  const std::array<uint32_t, 5> attributes{uint32_t(path.size()) | (uint32_t(path.size()) << 16), 0x10300, 0, 0x10280, 0x40};
  memory.Write(ctx, 0x10280, attributes.data(), sizeof(attributes));
  check(invoke(ctx, base, 202, {0x10120, 0x100001, 0x10288, 0x10128, 3, 0x20}) == 0,
    "combined production file open failed");
  const auto file = read(ctx, base, 0x10120);
  bool fileType = false;
  try { invoke(ctx, base, 233, {file, 0, 0x10200}); }
  catch (const browser::Fault& fault) { fileType = std::string(fault.what()).find("owned file handle") != std::string::npos; }
  check(fileType, "valid file wait was mislabeled invalid or succeeded");
  bool fileExtended = false;
  try { invoke(ctx, base, 234, {file, 0, 0, 0x10200}); }
  catch (const browser::Fault& fault) { fileExtended = std::string(fault.what()).find("owned file handle") != std::string::npos; }
  check(fileExtended, "extended wait on owned file handle was mislabeled or succeeded");
  check(invoke(ctx, base, 189, {0x10124, 0, 0, 1}) == 0, "guard fixture event create failed");
  const auto guardEvent = read(ctx, base, 0x10124);
  for (const bool fileFirst : {false, true}) {
    const std::array<uint32_t, 2> guarded = fileFirst ? std::array<uint32_t, 2>{file, guardEvent} :
      std::array<uint32_t, 2>{guardEvent, file};
    memory.Write(ctx, 0x10140, guarded.data(), sizeof(guarded));
    bool fileMulti = false;
    try { invoke(ctx, base, 235, {2, 0x10140, 1, 0, 0, 0x10200}); }
    catch (const browser::Fault& fault) { fileMulti = std::string(fault.what()).find("owned file handle") != std::string::npos; }
    check(fileMulti, "multiple wait skipped an owned file handle in the array");
  }
  check(invoke(ctx, base, 187, {guardEvent}) == 0, "guard fixture event close failed");
  check(invoke(ctx, base, 221, {file, 0}) == 0xc0000008, "file handle released a mutant");
  check(invoke(ctx, base, 187, {file}) == 0 && invoke(ctx, base, 187, {file}) == 0xc0000008,
    "central NtClose did not route file exactly once");
  check(invoke(ctx, base, 233, {file, 0, 0x10200}) == 0xc0000008, "closed file remained live");
  const uint32_t sentinel = 0xdeadbeef;
  memory.Write(ctx, 0x10100, &sentinel, 4);
  for (uint32_t n = 0; n < 130; ++n) {
    bool refused = false;
    const auto stack = ctx.esp;
    try { create(0x10ffe, 0, 1); } catch (const browser::Fault&) { refused = true; }
    check(refused && ctx.esp == stack && read(ctx, base, 0x10100) == sentinel,
      "invalid handle output mutated stack, published or exhausted objects");
  }
  for (const auto flags : {2u, 0x100u, 0x101u, 0xffffff00u}) {
    check(create(0x10100, 0, flags) == 0, "mutant creation failed to use its BOOLEAN low byte");
    const auto byteHandle = read(ctx, base, 0x10100);
    check(invoke(ctx, base, 221, {byteHandle, 0}) == (flags & 255 ? 0u : 0xc0000046u),
      "mutant initial ownership disagrees with the BOOLEAN low byte");
    check(invoke(ctx, base, 187, {byteHandle}) == 0, "BOOLEAN mutant fixture leaked its handle");
  }
  memory.Write(ctx, 0x10100, &sentinel, 4);
  bool refused = false;
  try { create(0x10100, 0x10180); } catch (const browser::Fault&) { refused = true; }
  check(refused && read(ctx, base, 0x10100) == sentinel, "named attributes were accepted");
  check(create(0x10100, 0, 1) == 0, "initial thread cannot own mutant");
  const auto handle = read(ctx, base, 0x10100);
  check(handle >= 0x1000 && handle < 0x70000000, "mutant collided with thread or file range");
  const auto rejectedWait = [&](uint32_t ordinal, std::initializer_list<uint32_t> arguments, const char* text) {
    bool caught = false;
    try { invoke(ctx, base, ordinal, arguments); }
    catch (const browser::Fault& fault) { caught = std::string(fault.what()).find(text) != std::string::npos; }
    check(caught, "alertable or processor-mode wait was accepted");
  };
  for (const auto flags : {0x100u, 0xD203F500u, 0xFFFFFF00u}) {
    check(invoke(ctx, base, 233, {handle, flags, 0x10200}) == 0, "single wait must ignore BOOLEAN padding");
    check(invoke(ctx, base, 221, {handle, 0}) == 0, "padded single wait release failed");
    check(invoke(ctx, base, 234, {handle, flags | 1, flags, 0x10200}) == 0, "extended wait must ignore BOOLEAN padding");
    check(invoke(ctx, base, 221, {handle, 0}) == 0, "padded extended wait release failed");
  }
  rejectedWait(233, {handle, 1, 0x10200}, "alertable");
  rejectedWait(233, {handle, 2, 0x10200}, "unsupported wait processor mode or alertable");
  check(invoke(ctx, base, 234, {handle, 1, 0, 0x10200}) == 0, "UserMode nonalertable wait was rejected");
  check(invoke(ctx, base, 221, {handle, 0}) == 0, "UserMode wait acquisition release failed");
  rejectedWait(234, {handle, 2, 0, 0x10200}, "unsupported wait processor mode or alertable");
  rejectedWait(234, {handle, 1, 1, 0x10200}, "alertable");
  rejectedWait(234, {handle, 0, 1, 0x10200}, "alertable");
  check(invoke(ctx, base, 233, {handle, 0, 0x10200}) == 0, "initial owner recursion failed");
  refused = false;
  try { invoke(ctx, base, 221, {handle, 0x10ffe}); } catch (const browser::Fault&) { refused = true; }
  check(refused, "release accepted invalid previous-count output");
  check(invoke(ctx, base, 221, {handle, 0x10108}) == 0 && read(ctx, base, 0x10108) == 0,
    "recursive release did not publish the native zero previous count");
  check(invoke(ctx, base, 221, {handle, 0x10108}) == 0 && read(ctx, base, 0x10108) == 0,
    "final release previous count wrong");
  memory.Write(ctx, 0x10108, &sentinel, 4);
  check(invoke(ctx, base, 221, {handle, 0x10108}) == 0xc0000046 && read(ctx, base, 0x10108) == 1,
    "unowned mutant release must fail and publish the free count");
  check(invoke(ctx, base, 187, {handle}) == 0 && invoke(ctx, base, 187, {handle}) == 0xc0000008,
    "mutant close was not routed exactly once");
  memory.Write(ctx, 0x10108, &sentinel, 4);
  check(invoke(ctx, base, 221, {handle, 0x10108}) == 0xc0000008 && read(ctx, base, 0x10108) == sentinel,
    "invalid handle release wrote a previous count");
  check(invoke(ctx, base, 233, {handle, 0, 0x10200}) == 0xc0000008, "closed mutant remained usable");
  check(invoke(ctx, base, 233, {0x71000004, 0, 0x10200}) == 0xc0000008,
    "file-range handle was accepted as mutant");
  memory.Write(ctx, 0x10100, &sentinel, 4);
  const auto createEvent = [&](uint32_t output, uint32_t type, uint32_t initial, uint32_t attributes = 0) {
    return invoke(ctx, base, 189, {output, attributes, type, initial});
  };
  for (const auto type : {2u, 0x100u}) {
    bool refused = false;
    const auto stack = ctx.esp;
    try { createEvent(0x10100, type, 0); } catch (const browser::Fault&) { refused = true; }
    check(refused && ctx.esp == stack && read(ctx, base, 0x10100) == sentinel,
      "invalid event type mutated stack, published or was accepted");
  }
  bool eventRefused = false;
  try { createEvent(0x10100, 0, 0, 0x10180); } catch (const browser::Fault&) { eventRefused = true; }
  check(eventRefused && read(ctx, base, 0x10100) == sentinel, "named event attributes were accepted");
  eventRefused = false;
  try { createEvent(0, 0, 0); } catch (const browser::Fault&) { eventRefused = true; }
  check(eventRefused, "null event handle output was accepted");
  eventRefused = false;
  try { createEvent(0x10ffe, 0, 0); } catch (const browser::Fault&) { eventRefused = true; }
  check(eventRefused && read(ctx, base, 0x10100) == sentinel, "uncommitted event output published a handle");
  check(createEvent(0x10100, 1, 0) == 0, "auto-reset event create failed");
  const auto syncEvent = read(ctx, base, 0x10100);
  check(syncEvent >= 0x1000 && syncEvent < 0x70000000, "event collided with thread or file range");
  check(invoke(ctx, base, 233, {syncEvent, 0, 0x10200}) == 0x102, "fresh auto-reset event was signaled");
  memory.Write(ctx, 0x10108, &sentinel, 4);
  check(invoke(ctx, base, 225, {syncEvent, 0x10108}) == 0 && read(ctx, base, 0x10108) == 0,
    "first event set did not publish the native zero previous state");
  check(invoke(ctx, base, 225, {syncEvent, 0x10108}) == 0 && read(ctx, base, 0x10108) == 1,
    "second event set did not publish the signaled previous state");
  check(invoke(ctx, base, 233, {syncEvent, 0, 0x10200}) == 0, "signaled auto-reset event wait failed");
  check(invoke(ctx, base, 233, {syncEvent, 0, 0x10200}) == 0x102, "auto-reset event did not consume its signal");
  check(invoke(ctx, base, 225, {syncEvent, 0}) == 0, "event set without previous-state output failed");
  check(invoke(ctx, base, 186, {syncEvent}) == 0, "event clear failed");
  check(invoke(ctx, base, 233, {syncEvent, 0, 0x10200}) == 0x102, "cleared event stayed signaled");
  check(createEvent(0x10100, 0, 1) == 0, "notification event create failed");
  const auto notifyEvent = read(ctx, base, 0x10100);
  check(invoke(ctx, base, 233, {notifyEvent, 0, 0x10200}) == 0, "signaled notification event wait failed");
  check(invoke(ctx, base, 233, {notifyEvent, 0, 0x10200}) == 0, "notification event consumed its signal");
  check(invoke(ctx, base, 186, {notifyEvent}) == 0, "notification event clear failed");
  check(invoke(ctx, base, 233, {notifyEvent, 0, 0x10200}) == 0x102, "cleared notification event stayed signaled");
  check(invoke(ctx, base, 225, {notifyEvent, 0}) == 0, "notification event set failed");
  check(invoke(ctx, base, 233, {notifyEvent, 0, 0x10200}) == 0, "re-set notification event wait failed");
  const std::array<uint32_t, 2> anyPair{syncEvent, notifyEvent};
  memory.Write(ctx, 0x10140, anyPair.data(), sizeof(anyPair));
  check(invoke(ctx, base, 235, {2, 0x10140, 1, 0, 0, 0x10200}) == 1, "WaitAny did not report the signaled member index");
  check(invoke(ctx, base, 233, {notifyEvent, 0, 0x10200}) == 0, "WaitAny consumed a notification member");
  check(invoke(ctx, base, 235, {2, 0x10140, 0, 0, 0, 0x10200}) == 0x102, "WaitAll ignored an unsignaled member");
  const int64_t multiRelative = -200000;
  memory.Write(ctx, 0x10210, &multiRelative, 8);
  const auto multiBefore = runtime.TimerTime(ctx);
  check(invoke(ctx, base, 235, {1, 0x10140, 0, 0, 0, 0x10210}) == 0x102, "bounded relative multiple wait failed");
  check(runtime.TimerTime(ctx) >= multiBefore + 200000, "multiple wait relative timeout used fake or early time");
  check(invoke(ctx, base, 225, {syncEvent, 0}) == 0, "multiple wait fixture set failed");
  check(invoke(ctx, base, 235, {2, 0x10140, 0, 1, 0, 0x10200}) == 0, "user-mode WaitAll on the signaled set failed");
  check(invoke(ctx, base, 233, {syncEvent, 0, 0x10200}) == 0x102, "WaitAll did not consume the auto-reset member");
  check(invoke(ctx, base, 233, {notifyEvent, 0, 0x10200}) == 0, "WaitAll consumed the notification member");
  rejectedWait(235, {0, 0x10140, 1, 0, 0, 0x10200}, "count");
  rejectedWait(235, {65, 0x10140, 1, 0, 0, 0x10200}, "count");
  rejectedWait(235, {1, 0, 1, 0, 0, 0x10200}, "null guest handle array");
  rejectedWait(235, {1, 0x10140, 2, 0, 0, 0x10200}, "wait type");
  rejectedWait(235, {1, 0x10140, 1, 2, 0, 0x10200}, "mode");
  rejectedWait(235, {1, 0x10140, 1, 0, 1, 0x10200}, "alertable");
  rejectedWait(235, {1, 0x10140, 1, 0, 0x1FF, 0x10200}, "alertable");
  const std::array<uint32_t, 2> duplicates{notifyEvent, notifyEvent};
  memory.Write(ctx, 0x10140, duplicates.data(), sizeof(duplicates));
  rejectedWait(235, {2, 0x10140, 1, 0, 0, 0x10200}, "duplicate");
  const std::array<uint32_t, 1> single{notifyEvent};
  memory.Write(ctx, 0x10140, single.data(), sizeof(single));
  check(invoke(ctx, base, 235, {1, 0x10140, 1, 0, 0x100, 0x10200}) == 0, "BOOLEAN alertable with a zero low byte was treated as alertable");
  const std::array<uint32_t, 2> unknownPair{notifyEvent, 0x71000004};
  memory.Write(ctx, 0x10140, unknownPair.data(), sizeof(unknownPair));
  check(invoke(ctx, base, 235, {2, 0x10140, 1, 0, 0, 0x10200}) == 0xc0000008, "unknown array handle was not an invalid-handle status");
  check(createEvent(0x10100, 1, 0x100) == 0, "BOOLEAN parity event create failed");
  const auto lowZero = read(ctx, base, 0x10100);
  check(invoke(ctx, base, 233, {lowZero, 0, 0x10200}) == 0x102, "initial state 0x100 was not truncated to FALSE");
  check(createEvent(0x10100, 1, 0x1FF) == 0, "BOOLEAN parity event create failed");
  const auto lowOne = read(ctx, base, 0x10100);
  check(invoke(ctx, base, 233, {lowOne, 0, 0x10200}) == 0, "initial state 0x1FF was not truncated to TRUE");
  check(invoke(ctx, base, 225, {0x71000004, 0}) == 0xc0000008, "file-range handle accepted as event");
  check(invoke(ctx, base, 186, {0x71000004}) == 0xc0000008, "file-range handle cleared as event");
  check(invoke(ctx, base, 187, {lowZero}) == 0 && invoke(ctx, base, 187, {lowZero}) == 0xc0000008,
    "event close was not routed exactly once");
  const std::array<uint32_t, 1> closedSingle{lowZero};
  memory.Write(ctx, 0x10140, closedSingle.data(), sizeof(closedSingle));
  check(invoke(ctx, base, 235, {1, 0x10140, 1, 0, 0, 0x10200}) == 0xc0000008,
    "closed event stayed waitable through the multiple wait");
  check(invoke(ctx, base, 187, {lowOne}) == 0, "parity event close failed");
  check(createEvent(0x10100, 1, 0) == 0, "shared event create failed");
  sharedEvent = read(ctx, base, 0x10100);
  check(invoke(ctx, base, 255, {0x10110, 0, 0x10000, 0, 0, 0x11100, 0, 0, 0, 0}) == 0,
    "production child creation failed");
  const auto thread = read(ctx, base, 0x10110);
  while (!childReady) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
  check(invoke(ctx, base, 233, {thread, 0, 0x10200}) == 0x102, "running thread poll did not time out");
  const std::array<uint32_t, 2> threadGuarded{syncEvent, thread};
  memory.Write(ctx, 0x10140, threadGuarded.data(), sizeof(threadGuarded));
  check(invoke(ctx, base, 235, {2, 0x10140, 0, 0, 0, 0x10200}) == 0x102, "mixed WaitAll fabricated a running thread completion");
  bool wrongType = false;
  try { invoke(ctx, base, 225, {sharedHandle, 0}); }
  catch (const browser::Fault& fault) { wrongType = std::string(fault.what()).find("not an event") != std::string::npos; }
  check(wrongType, "mutant accepted an event set");
  wrongType = false;
  try { invoke(ctx, base, 186, {sharedHandle}); }
  catch (const browser::Fault& fault) { wrongType = std::string(fault.what()).find("not an event") != std::string::npos; }
  check(wrongType, "mutant accepted an event clear");
  check(invoke(ctx, base, 221, {sharedHandle, 0}) == 0xc0000046, "foreign owner released child mutant");
  memory.Write(ctx, 0x10108, &sentinel, 4);
  check(invoke(ctx, base, 221, {sharedHandle, 0x10108}) == 0xc0000046 && read(ctx, base, 0x10108) == 0,
    "not-owned release must publish the owned zero count");
  check(invoke(ctx, base, 233, {sharedHandle, 0, 0x10200}) == 0x102, "owned mutant poll did not time out");
  const int64_t relative = -200000;
  memory.Write(ctx, 0x10208, &relative, 8);
  const auto before = runtime.TimerTime(ctx);
  check(invoke(ctx, base, 234, {sharedHandle, 0, 0, 0x10208}) == 0x102,
    "coherent real-clock relative wait failed");
  check(runtime.TimerTime(ctx) >= before + 200000, "relative wait used fake or early time");
  const auto userBefore = runtime.TimerTime(ctx);
  check(invoke(ctx, base, 234, {sharedHandle, 1, 0, 0x10208}) == 0x102 && runtime.TimerTime(ctx) >= userBefore + 200000,
    "UserMode relative wait did not time out honestly");
  check(invoke(ctx, base, 187, {thread}) == 0, "production thread close failed");
  check(invoke(ctx, base, 233, {thread, 0, 0x10200}) == 0xc0000008,
    "closed thread was treated as live or waitable");
  check(invoke(ctx, base, 233, {sharedHandle, 0, 0x10200}) == 0x102,
    "thread handle close abandoned live mutant");
  if (mode == 4) {
    const int64_t concurrent = -3000000;
    memory.Write(ctx, 0x10208, &concurrent, 8);
    const uint32_t pinned = sharedHandle.load();
    memory.Write(ctx, 0x10160, &pinned, 4);
    initialWaiting = true;
    check(invoke(ctx, base, 235, {1, 0x10160, 1, 0, 0, 0x10208}) == 0x102,
      "concurrent close disturbed the pinned multiple wait");
    check(closedWhileWaiting, "child close did not land during the pinned wait");
  }
  const auto waitDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!eventWaiting && std::chrono::steady_clock::now() < waitDeadline) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
  check(eventWaiting, "child never reached the blocking event wait");
  check(invoke(ctx, base, 225, {sharedEvent.load(), 0}) == 0, "cross-thread event set failed");
  const auto signalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!eventAcquired && std::chrono::steady_clock::now() < signalDeadline) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
  check(eventAcquired, "cross-thread event signal did not release the child wait");
  check(invoke(ctx, base, 233, {sharedEvent.load(), 0, 0x10200}) == 0x102,
    "auto-reset signal survived the releasing wait");
  releaseChild = true;
  if (mode == 4) {
    check(invoke(ctx, base, 233, {sharedHandle, 0, 0}) == 0xc0000008,
      "closed shared mutant stayed live after retirement");
  } else if (mode < 2) {
    check(invoke(ctx, base, 233, {sharedHandle, 0, 0}) == 0x80, "actual normal/termination did not abandon mutant");
    const uint32_t abandoned = sharedHandle.load();
    memory.Write(ctx, 0x10160, &abandoned, 4);
    check(invoke(ctx, base, 235, {1, 0x10160, 1, 0, 0, 0}) == 0, "abandoned mutant was not reacquired through the multiple wait");
    check(invoke(ctx, base, 221, {sharedHandle, 0}) == 0, "multiple wait recursion release failed");
    check(invoke(ctx, base, 221, {sharedHandle, 0}) == 0, "abandoned mutant did not transfer ownership");
    check(invoke(ctx, base, 187, {sharedHandle}) == 0, "abandoned mutant close failed");
    check(invoke(ctx, base, 187, {syncEvent}) == 0 && invoke(ctx, base, 187, {notifyEvent}) == 0 &&
      invoke(ctx, base, 187, {sharedEvent.load()}) == 0, "event close routing failed");
    check(invoke(ctx, base, 225, {syncEvent, 0}) == 0xc0000008 && invoke(ctx, base, 186, {notifyEvent}) == 0xc0000008 &&
      invoke(ctx, base, 233, {sharedEvent.load(), 0, 0}) == 0xc0000008, "closed events remained usable");
  } else {
    for (;;) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
  }
  ctx.esp += 8;
};

const auto testInstallationOwnership = [] {
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime first({}, {}), second({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    if (n == 2) { browser::InstallThreadProvider(first, image); }
    else { browser::InstallSynchronizationProvider(first, image); }
    bool refused = false;
    try {
      if (n == 0) { browser::BindImageImports(image, second, {}); }
      else if (n == 1) { browser::InstallThreadProvider(second, image); }
      else { browser::InstallSynchronizationProvider(second, image); }
    } catch (const browser::Fault&) { refused = true; }
    check(refused && !image.importsBound, "provider install/binding accepted another runtime owner");
  }
};

uint32_t waitScenario = 0;
std::atomic<bool> waitChildReady = false;
std::atomic<bool> waitChildRelease = false;

const auto completionChild = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  waitChildReady = true;
  while (!waitChildRelease) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
  if (waitScenario == 2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(invoke(ctx, base, 187, {read(ctx, base, 0x10100)}) == 0, "running child handle close failed");
  }
  if (waitScenario == 1) { invoke(ctx, base, 258, {73}); }
  if (waitScenario == 3) { throw browser::Fault(ctx, 0, 0, "thread wait child fault"); }
  ctx.eax = 42;
  ctx.esp += 8;
};

const auto completionEntry = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  auto& memory = browser::Memory::From(base);
  const uint64_t zero = 0;
  memory.Write(ctx, 0x10200, &zero, 8);
  check(invoke(ctx, base, 255, {0x10100, 0, 0x10000, 0, 0, 0x12100, 0, 0, 0, 0}) == 0, "waitable child creation failed");
  const auto handle = read(ctx, base, 0x10100);
  while (!waitChildReady) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
  check(invoke(ctx, base, 234, {handle, 1, 0, 0x10200}) == 0x102, "live thread poll falsely signaled");
  check(invoke(ctx, base, 246, {handle, 0, 0x10108}) == 0, "waitable thread reference failed");
  const auto object = read(ctx, base, 0x10108);
  check(invoke(ctx, base, 189, {0x10104, 0, 1, 1}) == 0, "mixed thread event creation failed");
  const auto event = read(ctx, base, 0x10104);
  const std::array<uint32_t, 2> mixed{event, handle};
  memory.Write(ctx, 0x10140, mixed.data(), sizeof(mixed));
  check(invoke(ctx, base, 235, {2, 0x10140, 0, 1, 0, 0x10200}) == 0x102, "mixed WaitAll partially consumed a signal");
  waitChildRelease = true;
  check(invoke(ctx, base, 235, {2, 0x10140, 0, 1, 0, 0}) == 0, "genuine completion did not release mixed WaitAll");
  check(read(ctx, base, object + 4) == 1 && read(ctx, base, object + 0x120) == (waitScenario == 1 ? 73u : 42u),
    "wait completion preceded guest signal or exit status publication");
  check(invoke(ctx, base, 233, {event, 0, 0x10200}) == 0x102, "mixed WaitAll did not consume its synchronization event");
  if (waitScenario == 2) {
    check(invoke(ctx, base, 233, {handle, 0, 0x10200}) == 0xc0000008, "closed completed thread admitted a fresh wait");
  } else {
    for (uint32_t n = 0; n < 3; ++n) {
      check(invoke(ctx, base, 233, {handle, 0, 0x10200}) == 0, "thread completion was consumed by an earlier wait");
    }
    check(invoke(ctx, base, 235, {2, 0x10140, 1, 0, 0, 0x10200}) == 1, "mixed WaitAny did not select the completed thread index");
    check(invoke(ctx, base, 187, {handle}) == 0, "completed thread close failed");
  }
  check(invoke(ctx, base, 187, {event}) == 0, "mixed event close failed");
  const auto saved = ctx.ecx;
  ctx.ecx = object;
  invoke(ctx, base, 250, {});
  ctx.ecx = saved;
  ctx.eax = 0;
  ctx.esp += 8;
};

void threadCompletionContracts() {
  for (uint32_t n = 0; n < 4; ++n) {
    waitScenario = n;
    waitChildReady = false;
    waitChildRelease = false;
    const std::array<X86FuncMapping, 2> mappings{{{0x12000, completionEntry}, {0x12100, completionChild}}};
    browser::Runtime runtime(mappings, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    image.image.entry = 0x12000;
    const auto closeSynchronization = browser::InstallSynchronizationProvider(runtime, image);
    browser::InstallThreadProvider(runtime, image, closeSynchronization);
    browser::BindImageImports(image, runtime, {});
    browser::ImageExecution execution;
    bool failed = false;
    try { browser::ExecuteImageEntry(image, runtime, execution); }
    catch (const browser::Fault& fault) {
      failed = std::string(fault.what()).find("thread wait child fault") != std::string::npos;
      check(failed, fault.what());
    }
    check(failed == (n == 3) && runtime.ActiveWorkers() == 0, "thread wait failure or teardown was fabricated");
    check(browser::SnapshotThreadSession(image).childrenCompleted == (n == 3 ? 0u : 1u), "faulted child fabricated completion");
  }
}

int main() try {
  threadCompletionContracts();
  testInstallationOwnership();
  for (uint32_t n = 0; n < 5; ++n) {
    mode = n;
    childReady = false;
    sharedEvent = 0;
    eventWaiting = false;
    eventAcquired = false;
    releaseChild = false;
    initialWaiting = false;
    closedWhileWaiting = false;
    sharedHandle = 0;
    const std::array<X86FuncMapping, 2> mappings = {{{0x11000, entry}, {0x11100, child}}};
    std::atomic<uint32_t> retirements = 0;
    browser::Runtime runtime(mappings, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    image.image.entry = 0x11000;
    auto routes = std::make_shared<browser::SynchronizationHandleRoutes>();
    auto temporary = std::make_shared<browser::TemporaryFilesystem>();
    check(temporary->CreateFile("DATA", 0) == 0, "combined temporary fixture failed");
    auto closeFile = browser::InstallFileProvider(runtime, image, {}, temporary, nullptr, nullptr, {}, &routes->ownsFile);
    auto closeSynchronization = browser::InstallSynchronizationProvider(runtime, image, routes);
    browser::InstallThreadProvider(runtime, image, [closeFile, closeSynchronization](uint32_t handle) {
      return closeFile(handle) || closeSynchronization(handle);
    }, &routes->ownsThread);
    image.threadRetirement = [retire = image.threadRetirement, &retirements](uint32_t owner) {
      retire(owner);
      ++retirements;
    };
    check(runtime.KernelTarget(186) && runtime.KernelTarget(189) && runtime.KernelTarget(192) &&
      runtime.KernelTarget(221) && runtime.KernelTarget(225) && runtime.KernelTarget(233) && runtime.KernelTarget(234) &&
      runtime.KernelTarget(235), "production synchronization operations missing");
    check(!runtime.KernelTarget(193) && !runtime.KernelTarget(194) && !runtime.KernelTarget(209),
      "unjustified generic dispatcher operations installed");
    browser::BindImageImports(image, runtime, {});
    X86Context foreign{};
    foreign.host = &runtime;
    foreign.esp = 0x10800;
    bool refused = false;
    try { runtime.Kernel(foreign, image.memory->Base(), 192); } catch (const browser::Fault&) { refused = true; }
    check(refused && foreign.esp == 0x10800, "nonworker acquired production synchronization");
    refused = false;
    try { runtime.Kernel(foreign, image.memory->Base(), 189); } catch (const browser::Fault&) { refused = true; }
    check(refused && foreign.esp == 0x10800, "nonworker acquired production event creation");
    browser::ImageExecution execution;
    bool interrupted = false;
    try { browser::ExecuteImageEntry(image, runtime, execution); }
    catch (const browser::Fault& fault) {
      if (mode < 2) { std::fprintf(stderr, "%s\n", fault.what()); }
      check(mode >= 2, "normal execution faulted"); interrupted = true;
    }
    check(interrupted == (mode == 2 || mode == 3) && runtime.ActiveWorkers() == 0,
      "completion/cancellation leaked worker or hid failure");
    check(retirements == 2, "normal/termination/fault/cancellation missed actual thread retirement");
    check(!closeSynchronization(0x71000004) && !closeSynchronization(0x70000004), "closer claimed another provider range");
  }
  std::puts("synchronization-provider-contracts-ok");
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
