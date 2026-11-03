#include "browser/thread-provider.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
constexpr uint32_t initialTls = 0xaabbccdd;
constexpr uint32_t sentinel = 0xdeadbeef;
std::atomic<bool> parentReturned = false;
std::atomic<bool> releaseSecond = false;
std::atomic<bool> releaseThird = false;
std::atomic<uint32_t> entered = 0;
uint32_t mode = 0;
uint32_t parentKpcr = 0;
constexpr uint32_t threadType = 0xc0005000;
std::array<uint32_t, 3> retainedObjects{};
browser::PreparedImage* activeImage = nullptr;

const auto read = [](X86Context& ctx, uint8_t* base, uint32_t address) {
  uint32_t value = 0;
  browser::Memory::From(base).Read(ctx, address, &value, 4);
  return value;
};
const auto awaitReady = [](X86Context& ctx, const auto& ready) {
  auto& runtime = browser::Runtime::From(ctx);
  while (!ready()) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
};
const auto create = [](X86Context& ctx, uint8_t* base, uint32_t slot, uint32_t extension = 0,
    uint32_t tls = 20, uint32_t suspended = 0, uint32_t system = 0x11100, uint32_t output = 0, uint32_t debugger = 0) {
  auto& runtime = browser::Runtime::From(ctx);
  X86Context call = ctx;
  return runtime.Call(call, base, runtime.KernelTarget(255),
    {output ? output : 0x10400 + slot * 8, extension, 0x10000, tls, 0x10404 + slot * 8,
      0x11200, slot, suspended, debugger, system});
};
const auto close = [](X86Context& ctx, uint8_t* base, uint32_t handle) {
  auto& runtime = browser::Runtime::From(ctx);
  return runtime.Call(ctx, base, runtime.KernelTarget(187), {handle});
};
const auto reference = [](X86Context& ctx, uint8_t* base, uint32_t handle, uint32_t output, uint32_t type = threadType) {
  auto& runtime = browser::Runtime::From(ctx);
  X86Context call = ctx;
  return runtime.Call(call, base, runtime.KernelTarget(246), {handle, type, output});
};
const auto dereference = [](X86Context& ctx, uint8_t* base, uint32_t object) {
  auto& runtime = browser::Runtime::From(ctx);
  X86Context call = ctx;
  call.ecx = object;
  return runtime.Call(call, base, runtime.KernelTarget(250), {});
};
const auto expectUnpublished = [](X86Context& ctx, uint8_t* base, const auto& action, const char* reason) {
  auto& memory = browser::Memory::From(base);
  const uint32_t esp = ctx.esp;
  const size_t backing = memory.BackingBytes();
  bool rejected = false;
  try { action(); }
  catch (const browser::Fault& fault) { rejected = std::strstr(fault.what(), reason) != nullptr; }
  check(rejected && ctx.esp == esp, "failed admission lost its bounded fault or parent stack");
  check(read(ctx, base, 0x10418) == sentinel && read(ctx, base, 0x1041c) == sentinel,
    "failed admission partially published outputs");
  check(memory.BackingBytes() == backing, "failed admission retained backing storage");
};
const auto child = [](X86Context& ctx, uint8_t* base) {
  auto& memory = browser::Memory::From(base);
  auto& runtime = browser::Runtime::From(ctx);
  const uint32_t slot = read(ctx, base, ctx.esp + 4);
  const uint32_t handle = read(ctx, base, 0x10400 + slot * 8);
  const uint32_t id = read(ctx, base, 0x10404 + slot * 8);
  const uint32_t stackTop = read(ctx, base, ctx.fs_base + 4);
  const uint32_t tls = read(ctx, base, stackTop - 20);
  check(slot < browser::kImageThreadCapacity - 1 && id == 0x14 + slot * 4 && handle == 0x70000004 + slot * 4 && ctx.thread_handle == handle,
    "child entered before its own outputs or reused identity");
  check(ctx.fs_base != parentKpcr && read(ctx, base, tls) == initialTls,
    "child inherited another thread's KPCR or mutable TLS");
  const uint32_t object = ctx.fs_base + 0x400;
  check(reference(ctx, base, 0xfffffffe, 0x10800 + slot * 4) == 0 &&
    read(ctx, base, 0x10800 + slot * 4) == object, "current child pseudo handle resolved the wrong object");
  dereference(ctx, base, object);
  check(read(ctx, base, object + 4) == 0 && read(ctx, base, object + 0x120) == 0x103,
    "running thread was already signaled or lacked pending exit status");
  const uint32_t value = 0x76543210 + slot;
  memory.Write(ctx, tls, &value, 4);
  if (slot < 32) { entered.fetch_or(1u << slot); }
  if (slot >= 3) {
    runtime.Call(ctx, base, runtime.KernelTarget(258), {73 + slot});
    check(false, "thread termination returned to guest code");
  }
  if (slot == 0) {
    awaitReady(ctx, [&] { return parentReturned.load() && runtime.ActiveWorkers() == 1; });
    check(create(ctx, base, 1, 0, 20, 0, mode == 4 ? 0 : 0x11100) == 0, "second owned child creation failed");
    const uint32_t secondHandle = read(ctx, base, 0x10408);
    check(reference(ctx, base, secondHandle, 0x10044) == 0, "second thread reference failed");
    retainedObjects[1] = read(ctx, base, 0x10044);
    check(secondHandle != handle && close(ctx, base, secondHandle) == 0 && close(ctx, base, secondHandle) == 0xc0000008,
      "second handle did not close exactly once");
    X86Context impostor = ctx;
    impostor.fs_base += 0x02000000;
    bool rejected = false;
    try { runtime.Call(impostor, base, runtime.KernelTarget(258), {99}); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected, "worker terminated a different managed child using a forged KPCR");
    check(reference(ctx, base, secondHandle, 0x10044) == 0xc0000008,
      "closed second handle admitted another reference");
    check(runtime.Call(ctx, base, runtime.KernelTarget(143), {retainedObjects[1], 7}) == 0 &&
      runtime.Call(ctx, base, runtime.KernelTarget(143), {retainedObjects[1], 0xfffffff0}) == 7,
      "thread priority did not preserve signed old priority after close");
    releaseSecond = true;
    if (mode == 1 || mode == 2 || mode == 5) {
      awaitReady(ctx, [&] { return (entered.load() & 2) != 0; });
      if (mode == 5) { runtime.Cancel(); }
      for (;;) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
    }
    awaitReady(ctx, [&] { return runtime.ActiveWorkers() == 1; });
    check(read(ctx, base, retainedObjects[1] + 4) == 1 &&
      read(ctx, base, retainedObjects[1] + 0x120) == (mode == 4 ? 43u : 74u),
      "second thread did not publish its own real return or termination status");
    dereference(ctx, base, retainedObjects[1]);
    bool staleRejected = false;
    try { dereference(ctx, base, retainedObjects[1]); }
    catch (const browser::Fault&) { staleRejected = true; }
    check(staleRejected, "thread reference released more than once");
    check(create(ctx, base, 2) == 0, "third retained child creation failed");
    const uint32_t thirdHandle = read(ctx, base, 0x10410);
    check(thirdHandle != handle && thirdHandle != secondHandle && close(ctx, base, thirdHandle) == 0,
      "retired or closed thread handle was reused");
    releaseThird = true;
    awaitReady(ctx, [&] { return runtime.ActiveWorkers() == 1; });
    check(close(ctx, base, thirdHandle) == 0xc0000008 && close(ctx, base, handle) == 0xc0000008,
      "closed handles became live after later creations");
    if (mode == 0) {
      for (uint32_t extra = 3; extra < browser::kImageThreadCapacity - 1; ++extra) {
        check(create(ctx, base, extra) == 0, "bounded session storage child creation failed");
        check(read(ctx, base, 0x10400 + extra * 8) == 0x70000004 + extra * 4 &&
          read(ctx, base, 0x10404 + extra * 8) == 0x14 + extra * 4, "extra child outputs were not published");
        awaitReady(ctx, [&] { return runtime.ActiveWorkers() == 1; });
      }
      const uint32_t esp = ctx.esp;
      const uint32_t outputBefore = read(ctx, base, 0x107f8);
      const uint32_t idBefore = read(ctx, base, 0x107fc);
      const size_t backing = memory.BackingBytes();
      bool capacityRejected = false;
      try { create(ctx, base, browser::kImageThreadCapacity - 1); }
      catch (const browser::Fault& fault) { capacityRejected = std::strstr(fault.what(), "session thread storage capacity exhausted") != nullptr; }
      check(capacityRejected && ctx.esp == esp, "session capacity did not reject without changing the caller stack");
      check(read(ctx, base, 0x107f8) == outputBefore && read(ctx, base, 0x107fc) == idBefore && memory.BackingBytes() == backing,
        "session capacity rejection published outputs or retained backing");
      check(activeImage->threads.size() == browser::kImageThreadCapacity, "session retained distinct thread storage");
      const auto session = browser::SnapshotThreadSession(*activeImage);
      check(session.childrenCreated == 127 && session.childrenCompleted == 126 && session.handlesClosed == 3 &&
        session.childrenSuspended == 0 && session.objectReferences == 0 &&
        session.requestedStackBytes == 127u * 0x10000u && session.committedBytes == 127u * 0x12000u,
        "session counters do not match the genuine child history");
    }
  } else if (slot == 1) {
    awaitReady(ctx, [&] { return releaseSecond.load(); });
    if (mode == 1) { runtime.Invoke(ctx, base, runtime.KernelTarget(349)); }
    if (mode == 2) { runtime.Cancel(); runtime.Checkpoint(ctx); }
    if (mode == 5) { for (;;) { runtime.Checkpoint(ctx); std::this_thread::yield(); } }
  } else {
    awaitReady(ctx, [&] { return releaseThird.load(); });
  }
  check(read(ctx, base, tls) == value, "live thread TLS was overwritten by later creation");
  if (mode != 4) {
    runtime.Call(ctx, base, runtime.KernelTarget(258), {73 + slot});
    check(false, "thread termination returned to guest code");
  }
  ctx.eax = 42 + slot;
  ctx.esp += 8;
};
const auto systemRoutine = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  const uint32_t start = read(ctx, base, ctx.esp + 4);
  const uint32_t argument = read(ctx, base, ctx.esp + 8);
  check(start == 0x11200, "system routine received wrong start routine");
  ctx.eax = runtime.Call(ctx, base, start, {argument});
  ctx.esp += 12;
};
const auto parent = [](X86Context& ctx, uint8_t* base) {
  parentKpcr = ctx.fs_base;
  auto& runtime = browser::Runtime::From(ctx);
  auto& memory = browser::Memory::From(base);
  for (uint32_t n = 0; n < 5; ++n) {
    bool rejected = false;
    try { create(ctx, base, 0, n == 1 ? 1 : 0, n == 2 ? 24 : 20, 0,
      n == 4 ? 0x11900 : 0x11100, n == 0 ? 0x20000 : 0, n == 3 ? 1 : 0); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected && read(ctx, base, 0x10400) == 0 && entered.load() == 0,
      "unsupported creation mode launched a child or published outputs");
  }
  bool rejected = false;
  try { create(ctx, base, 0, 0, 20, 0, 0x11100, 0x10403); }
  catch (const browser::Fault&) { rejected = true; }
  check(rejected && read(ctx, base, 0x10400) == 0, "overlapping outputs were published");
  rejected = false;
  try {
    runtime.Call(ctx, base, runtime.KernelTarget(255), {0x10400, 0, 0x10000, 20, 0x10ffe, 0x11200, 0, 0, 0, 0});
  } catch (const browser::Fault&) { rejected = true; }
  check(rejected && read(ctx, base, 0x10400) == 0 && entered.load() == 0,
    "invalid ID output partially published a handle");
  check(activeImage != nullptr, "thread fixture is absent");
  const size_t backing = memory.BackingBytes();
  bool rolledBack = false;
  try {
    browser::LaunchImageThread(*activeImage, runtime, ctx, 0x14, 0x10000,
      [&](const browser::ImageThread&) { throw browser::Fault(ctx, 0, 0, "refused admission"); });
  } catch (const browser::Fault&) { rolledBack = true; }
  check(rolledBack && activeImage->threads.size() == 1 && memory.BackingBytes() == backing,
    "failed child admission retained thread storage");
  rejected = false;
  try { runtime.Call(ctx, base, runtime.KernelTarget(258), {99}); }
  catch (const browser::Fault&) { rejected = true; }
  check(rejected, "initial thread was accepted as a managed child");
  for (uint32_t n = 0; n < 8; ++n) {
    check(read(ctx, base, threadType + n * 4) == 0, "thread object type metadata is not native-zero");
  }
  check(reference(ctx, base, 0xfffffffe, 0x10048) == 0 && read(ctx, base, 0x10048) == parentKpcr + 0x400,
    "initial thread pseudo handle did not reference its ETHREAD");
  dereference(ctx, base, parentKpcr + 0x400);
  check(create(ctx, base, 0) == 0, "first child creation failed");
  awaitReady(ctx, [&] { return entered.load() == 1; });
  expectUnpublished(ctx, base, [&] { create(ctx, base, 3); }, "guest worker capacity exhausted");
  check(activeImage->threads.size() == 2, "runtime worker rejection consumed retained storage");
  const uint32_t handle = read(ctx, base, 0x10400);
  check(reference(ctx, base, handle, 0x10040) == 0, "first thread reference failed");
  retainedObjects[0] = read(ctx, base, 0x10040);
  const uint32_t before = read(ctx, base, 0x10040);
  check(reference(ctx, base, handle, 0x10040, threadType + 4) == 0xc0000024 &&
    read(ctx, base, 0x10040) == before, "wrong object type published a reference");
  check(reference(ctx, base, 0x60000004, 0x10040) == 0xc0000008 &&
    read(ctx, base, 0x10040) == before, "file handle was accepted as a thread");
  rejected = false;
  try { reference(ctx, base, handle, 0x10ffe); }
  catch (const browser::Fault&) { rejected = true; }
  check(rejected, "invalid reference output was accepted");
  for (uint32_t n = 1; n < 1024; ++n) {
    check(reference(ctx, base, handle, 0x10040, 0) == 0, "untyped reference did not share the owned thread object");
  }
  memory.Write(ctx, 0x10060, &sentinel, 4);
  const uint32_t referenceEsp = ctx.esp;
  rejected = false;
  try { reference(ctx, base, handle, 0x10060); }
  catch (const browser::Fault& fault) { rejected = std::strstr(fault.what(), "thread reference budget exhausted") != nullptr; }
  check(rejected && ctx.esp == referenceEsp && read(ctx, base, 0x10060) == sentinel,
    "thread reference bound partially published or modified the caller stack");
  for (uint32_t n = 0; n < 1024; ++n) { dereference(ctx, base, retainedObjects[0]); }
  rejected = false;
  try { dereference(ctx, base, retainedObjects[0]); }
  catch (const browser::Fault&) { rejected = true; }
  check(rejected, "failed reference output leaked an object reference");
  check(close(ctx, base, handle) == 0 && close(ctx, base, handle) == 0xc0000008,
    "first thread handle did not close exactly once");
  check(close(ctx, base, 0x60000004) == 0 && close(ctx, base, 0x60000004) == 0xc0000008,
    "file handle close delegation changed");
  parentReturned = true;
  ctx.esp += 8;
};

uint32_t suspMode = 0;
const auto resume = [](X86Context& ctx, uint8_t* base, uint32_t handle, uint32_t previousOut) {
  auto& runtime = browser::Runtime::From(ctx);
  X86Context call = ctx;
  return runtime.Call(call, base, runtime.KernelTarget(224), {handle, previousOut});
};
const auto suspAssertParked = [](X86Context& ctx, uint8_t* base, uint32_t slot) {
  auto& runtime = browser::Runtime::From(ctx);
  const uint32_t bit = 1u << slot;
  const uint32_t object = activeImage->threads[slot + 1].ethread;
  for (uint32_t n = 0; n < 50000; ++n) {
    runtime.Checkpoint(ctx);
    std::this_thread::yield();
    check((entered.load() & bit) == 0, "suspended child entered before its first resume");
    check(read(ctx, base, object + 4) == 0 && read(ctx, base, object + 0x120) == 0x103,
      "parked child was signaled before genuine completion");
  }
};
const auto suspChild = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  const uint32_t slot = read(ctx, base, ctx.esp + 4);
  const uint32_t handle = read(ctx, base, 0x10400 + slot * 8);
  const uint32_t id = read(ctx, base, 0x10404 + slot * 8);
  check(slot < 2 && id == 0x14 + slot * 4 && handle == 0x70000004 + slot * 4 && ctx.thread_handle == handle,
    "resumed child entered before its own outputs or reused identity");
  if (slot < 32) { entered.fetch_or(1u << slot); }
  runtime.Call(ctx, base, runtime.KernelTarget(258), {73 + slot});
  check(false, "thread termination returned to guest code");
};
const auto suspSystem = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  const uint32_t start = read(ctx, base, ctx.esp + 4);
  const uint32_t argument = read(ctx, base, ctx.esp + 8);
  check(start == 0x11200, "system routine received wrong start routine");
  ctx.eax = runtime.Call(ctx, base, start, {argument});
  ctx.esp += 12;
};
const auto suspParent = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  auto& memory = browser::Memory::From(base);
  if (suspMode == 0) {
    check(create(ctx, base, 0, 0, 20, 1) == 0, "suspended child creation failed");
    const uint32_t handle = read(ctx, base, 0x10400);
    check(handle == 0x70000004 && read(ctx, base, 0x10404) == 0x14,
      "suspended creation did not publish handle and id before guest start");
    suspAssertParked(ctx, base, 0);
    const uint32_t espBefore = ctx.esp;
    bool rejected = false;
    try { resume(ctx, base, handle, 0x10ffe); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected && ctx.esp == espBefore, "invalid previous-count output was accepted");
    check(resume(ctx, base, 0x60000004, 0) == 0xc0000008, "file-owned handle resumed as a thread");
    check(resume(ctx, base, 0x7ffffff0, 0) == 0xc0000008, "unknown handle resumed as a thread");
    check(resume(ctx, base, 0xfffffffe, 0) == 0xc0000008, "pseudo handle resumed as a thread");
    memory.Write(ctx, 0x10040, &sentinel, 4);
    check(resume(ctx, base, handle, 0x10040) == 0 && read(ctx, base, 0x10040) == 1,
      "first resume did not report the pre-decrement suspend count");
    check(resume(ctx, base, handle, 0x10040) == 0 && read(ctx, base, 0x10040) == 0,
      "second resume did not report the raw decremented suspend count");
    awaitReady(ctx, [&] { return entered.load() == 1; });
    awaitReady(ctx, [&] { return runtime.ActiveWorkers() == 1; });
    check(close(ctx, base, handle) == 0 && resume(ctx, base, handle, 0) == 0xc0000008,
      "closed thread handle resumed as a thread");
  } else if (suspMode == 1) {
    check(create(ctx, base, 0, 0, 20, 0x100) == 0, "low-byte-zero suspended dword was rejected");
    awaitReady(ctx, [&] { return entered.load() == 1; });
    awaitReady(ctx, [&] { return runtime.ActiveWorkers() == 1; });
    check(create(ctx, base, 1, 0, 20, 0x101) == 0, "low-byte-nonzero suspended dword was rejected");
    const uint32_t handle = read(ctx, base, 0x10408);
    suspAssertParked(ctx, base, 1);
    check(resume(ctx, base, handle, 0x10044) == 0 && read(ctx, base, 0x10044) == 1,
      "low-byte suspend count was not 1");
    awaitReady(ctx, [&] { return (entered.load() & 2) != 0; });
    awaitReady(ctx, [&] { return runtime.ActiveWorkers() == 1; });
  } else if (suspMode == 2) {
    check(create(ctx, base, 0, 0, 20, 1) == 0, "suspended child creation failed");
    runtime.Cancel();
  } else if (suspMode == 3) {
    check(create(ctx, base, 0, 0, 20, 1) == 0, "suspended child creation failed");
    const uint32_t handle = read(ctx, base, 0x10400);
    check(close(ctx, base, handle) == 0 && close(ctx, base, handle) == 0xc0000008,
      "parked thread handle did not close exactly once");
    check(resume(ctx, base, handle, 0) == 0xc0000008, "closed parked handle resumed as a thread");
    suspAssertParked(ctx, base, 0);
    runtime.Cancel();
  } else {
    check(create(ctx, base, 0, 0, 20, 1) == 0, "suspended child creation failed");
    const uint32_t handle = read(ctx, base, 0x10400);
    const uint32_t espBefore = ctx.esp;
    bool rejected = false;
    try { create(ctx, base, 1); }
    catch (const browser::Fault& fault) { rejected = std::strstr(fault.what(), "guest worker capacity exhausted") != nullptr; }
    check(rejected && ctx.esp == espBefore && read(ctx, base, 0x10408) == sentinel &&
      read(ctx, base, 0x1040c) == sentinel, "capacity-bound creation was not a bounded fault or partially published");
    check(activeImage->threads.size() == 2, "capacity-bound creation retained thread storage");
    check(resume(ctx, base, handle, 0) == 0, "parked child was not resumable after capacity rollback");
    awaitReady(ctx, [&] { return entered.load() == 1; });
    awaitReady(ctx, [&] { return runtime.ActiveWorkers() == 1; });
  }
  ctx.esp += 8;
};

int main() try {
  for (uint32_t n = 0; n < 18; ++n) {
    mode = n % 6;
    parentReturned = false;
    releaseSecond = false;
    releaseThird = false;
    entered = 0;
    const std::array<X86FuncMapping, 3> mappings = {{{0x11000, parent}, {0x11100, systemRoutine}, {0x11200, child}}};
    browser::Runtime runtime(mappings, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Map(0x10000, 4096);
    image.memory->Commit(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    image.image.entry = 0x11000;
    image.image.tls_addr = 0x10080;
    image.image.tls.index_addr = 0x100e0;
    image.tlsTemplate.resize(16);
    std::memcpy(image.tlsTemplate.data(), &initialTls, 4);
    const uint32_t import = 0x8000015d;
    image.imports = {{0x10000, 349}};
    image.memory->Write({}, 0x10000, &import, 4);
    image.memory->Write({}, 0x10418, &sentinel, 4);
    image.memory->Write({}, 0x1041c, &sentinel, 4);
    bool fileOpen = true;
    browser::InstallThreadProvider(runtime, image, [&](uint32_t handle) {
      if (handle != 0x60000004 || !fileOpen) { return false; }
      fileOpen = false;
      return true;
    });
    check(!image.diagnosticRuntime, "thread provider claimed unrelated diagnostic imports before drive-key installation");
    check(runtime.KernelTarget(246) && runtime.KernelTarget(143) && runtime.KernelTarget(250),
      "reached thread reference, priority and release exports are missing");
    browser::DeclareDiagnosticImport(image, runtime, 349, false);
    browser::BindImageImports(image, runtime, {});
    X86Context foreign{};
    foreign.host = &runtime;
    foreign.esp = 0x10800;
    bool rejected = false;
    try { runtime.Kernel(foreign, image.memory->Base(), 255); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected && foreign.esp == 0x10800 && image.threads.empty(), "host caller created a session child");
    browser::Runtime other({}, {});
    foreign.host = &other;
    rejected = false;
    try { runtime.Kernel(foreign, image.memory->Base(), 255); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected && image.threads.empty(), "foreign runtime created a session child");
    activeImage = &image;
    browser::ImageExecution execution;
    bool faulted = false;
    try { browser::ExecuteImageEntry(image, runtime, execution); }
    catch (const browser::UnsupportedImport& fault) {
      check(mode == 1 && fault.ordinal == 349 && fault.context->fs_base == image.threads[2].kpcr,
        "second child fault lost identity");
      faulted = true;
    } catch (const browser::Cancelled&) {
      check(mode == 2 || mode == 5, "unexpected cancellation");
      faulted = true;
    }
    const bool interrupted = mode == 1 || mode == 2 || mode == 5;
    const uint32_t expectedEntered = interrupted ? 3u : (mode == 0 ? UINT32_MAX : 7u);
    check(faulted == interrupted && execution.entered && execution.returned && entered.load() == expectedEntered,
      "entry completed without joining all owned descendants");
    check(runtime.ActiveWorkers() == 0 && image.threads.size() == (interrupted ? 3u : (mode == 0 ? browser::kImageThreadCapacity : 4u)),
      "owned worker or retained storage teardown incomplete");
    check(read(execution.context, image.memory->Base(), image.threads[0].ethread + 4) == 1,
      "actual initial return did not signal its own thread object");
    if (interrupted) {
      for (size_t n = 1; n < image.threads.size(); ++n) {
        check(read(execution.context, image.memory->Base(), image.threads[n].ethread + 4) == 0 &&
          read(execution.context, image.memory->Base(), image.threads[n].ethread + 0x120) == 0x103,
          "runtime fault or cancellation fabricated successful guest completion");
      }
    }
    check(read(execution.context, image.memory->Base(), image.threads[0].tlsData) == initialTls,
      "child modified parent TLS");
    for (size_t n = 1; n < image.threads.size(); ++n) {
      const auto& thread = image.threads[n];
      if (!interrupted) {
        check(read(execution.context, image.memory->Base(), thread.ethread + 4) == 1 &&
          read(execution.context, image.memory->Base(), thread.ethread + 0x120) == (mode == 4 ? 41u + n : 72u + n),
          "thread completion status was not retained independently");
      }
      check(thread.id == 0x10 + n * 4 && thread.kpcr == image.threads[n - 1].stackTop + 8192,
        "thread identity or storage slot was reused");
      check(read(execution.context, image.memory->Base(), thread.tlsData) == 0x7654320f + n,
        "retained TLS storage was reused or discarded before image retirement");
      check(thread.stackLimit > image.threads[n - 1].stackTop && thread.tlsData != image.threads[n - 1].tlsData,
        "child stacks or TLS overlapped");
    }
  }
  for (uint32_t round = 0; round < 2; ++round) {
    for (suspMode = 0; suspMode < 5; ++suspMode) {
      entered = 0;
      const std::array<X86FuncMapping, 3> mappings = {{{0x11000, suspParent}, {0x11100, suspSystem}, {0x11200, suspChild}}};
      browser::Runtime runtime(mappings, {});
      browser::PreparedImage image;
      image.memory = std::make_unique<browser::Memory>();
      image.memory->Map(0x10000, 4096);
      image.memory->Commit(0x10000, 4096);
      image.image.kernel_thunk = 0x10000;
      image.image.entry = 0x11000;
      image.image.tls_addr = 0x10080;
      image.image.tls.index_addr = 0x100e0;
      image.tlsTemplate.resize(16);
      std::memcpy(image.tlsTemplate.data(), &initialTls, 4);
      image.memory->Write({}, 0x10408, &sentinel, 4);
      image.memory->Write({}, 0x1040c, &sentinel, 4);
      browser::InstallThreadProvider(runtime, image);
      browser::BindImageImports(image, runtime, {});
      activeImage = &image;
      browser::ImageExecution execution;
      const bool cancelled = suspMode == 2 || suspMode == 3;
      bool faulted = false;
      try { browser::ExecuteImageEntry(image, runtime, execution); }
      catch (const browser::Cancelled&) {
        check(cancelled, "unexpected cancellation");
        faulted = true;
      }
      check(faulted == cancelled && execution.entered && execution.returned != cancelled,
        cancelled ? "cancellation fabricated the initial thread's return" : "entry did not run to its genuine end");
      check(runtime.ActiveWorkers() == 0, "owned worker teardown incomplete");
      const auto session = browser::SnapshotThreadSession(image);
      const uint32_t created = suspMode == 1 ? 2u : 1u;
      check(session.childrenCreated == created && session.childrenCompleted == (cancelled ? 0u : created) &&
        session.handlesClosed == (suspMode == 0 || suspMode == 3 ? 1u : 0u) && session.childrenSuspended == 0 &&
        session.objectReferences == 0 && session.requestedStackBytes == created * 0x10000u &&
        session.committedBytes == created * 0x12000u, "session counters do not match the child history");
      if (cancelled) {
        check(entered.load() == 0 && image.threads.size() == 2, "cancelled parked child ran or lost its retained storage");
        check(read(execution.context, image.memory->Base(), image.threads[0].ethread + 4) == 0 &&
          read(execution.context, image.memory->Base(), image.threads[1].ethread + 4) == 0 &&
          read(execution.context, image.memory->Base(), image.threads[1].ethread + 0x120) == 0x103,
          "cancellation fabricated a thread completion");
      } else {
        const uint32_t expected = suspMode == 1 ? 3u : 1u;
        check(entered.load() == expected && image.threads.size() == (suspMode == 1 ? 3u : 2u),
          "resumed children did not complete genuinely");
        for (size_t n = 1; n < image.threads.size(); ++n) {
          check(read(execution.context, image.memory->Base(), image.threads[n].ethread + 4) == 1 &&
            read(execution.context, image.memory->Base(), image.threads[n].ethread + 0x120) == 72u + n,
            "resumed child did not retain its genuine termination status");
        }
      }
    }
  }
  {
    browser::Runtime runtime({}, {});
    browser::Runtime foreign({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    image.imports = {{0x10000, 259}};
    const uint32_t imported = 0x80000103;
    image.memory->Write({}, 0x10000, &imported, 4);
    const std::array exports{browser::InstallThreadProvider(runtime, image)};
    check(exports[0].ordinal == 259 && exports[0].address == threadType && exports[0].size == 32,
      "thread object type export has the wrong owned metadata shape");
    bool rejected = false;
    try { browser::BindImageImports(image, foreign, exports); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected && !image.importsBound, "another runtime published thread provider metadata");
    browser::BindImageImports(image, runtime, exports);
    uint32_t target = 0;
    image.memory->Read({}, 0x10000, &target, 4);
    check(target == threadType && image.boundImports[0].kind == browser::ImportKind::Data,
      "PsThreadObjectType was bound as a function or copied value instead of metadata address");
  }
  std::puts("thread-provider-contracts-ok");
  return 0;
} catch (const std::exception& error) {
  std::fprintf(stderr, "thread-provider-contracts-failed: %s\n", error.what());
  return 1;
}
