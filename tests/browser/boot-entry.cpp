#include "browser/image.h"
#include "browser/guest-wait.h"
#include <array>
#include <cstdio>

static_assert(alignof(X86Context) == 64);
static_assert(alignof(browser::Fault) <= 16);
static_assert(alignof(browser::Cancelled) <= 16);
static_assert(alignof(browser::UnsupportedImport) <= 16);
static_assert(alignof(browser::WaitWatchdogExpired) <= 16);
static_assert(std::is_same_v<decltype(browser::Fault::context), std::shared_ptr<const X86Context>>);

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

constexpr std::array snapshotRegisters = {
  &X86Context::eax, &X86Context::ecx, &X86Context::edx, &X86Context::ebx,
  &X86Context::esp, &X86Context::ebp, &X86Context::esi, &X86Context::edi,
  &X86Context::cf, &X86Context::zf, &X86Context::sf, &X86Context::of,
  &X86Context::pf, &X86Context::df, &X86Context::xflags, &X86Context::fs_base,
  &X86Context::top, &X86Context::mxcsr, &X86Context::thread_handle, &X86Context::last_icall
};

const auto checkSnapshot = [](const std::shared_ptr<const X86Context>& snapshot, const X86Context& expected) {
  check(snapshot && reinterpret_cast<uintptr_t>(snapshot.get()) % alignof(X86Context) == 0,
    "fault snapshot lost context alignment");
  for (auto member : snapshotRegisters) { check(snapshot.get()->*member == expected.*member, "fault lost a scalar register"); }
  check(snapshot->fcw == expected.fcw && snapshot->fsw == expected.fsw && snapshot->host == expected.host,
    "fault lost control state or runtime identity");
  for (size_t n = 0; n < 8; ++n) {
    check(snapshot->st[n] == expected.st[n] && snapshot->mm[n] == expected.mm[n], "fault lost x87 or MMX state");
    for (const auto lane : {0u, 1u, 2u, 3u}) {
      check(snapshot->xmm[n].u[lane] == expected.xmm[n].u[lane], "fault lost SIMD state");
    }
  }
};

const auto testFaultSnapshots = [] {
  for (const auto mode : {0u, 1u, 2u}) {
    browser::Runtime runtime({}, {});
    X86Context expected{};
    for (uint32_t n = 0; auto member : snapshotRegisters) { expected.*member = 0x12340000 + ++n; }
    expected.fcw = 0x123;
    expected.fsw = 0x456;
    expected.host = &runtime;
    for (size_t n = 0; n < 8; ++n) {
      expected.st[n] = double(n) + 0.25;
      expected.mm[n] = 0xfedcba9876543210ull + n;
      for (const auto lane : {0u, 1u, 2u, 3u}) { expected.xmm[n].u[lane] = 0xabcdef00 + n * 4 + lane; }
    }
    std::optional<browser::Fault> retained;
    uint8_t backing = 0;
    runtime.Launch(expected, &backing, [&](X86Context& context, uint8_t*) {
      check(runtime.OwnsCurrentContext(context) && reinterpret_cast<uintptr_t>(&context) % alignof(X86Context) == 0,
        "fault test did not run in an aligned owned worker");
      const auto raise = [&](const auto& fault) {
        check(fault.context.get() != &context, "fault borrowed its live context");
        checkSnapshot(fault.context, expected);
        const auto copied = fault;
        retained.emplace(copied);
        check(retained->context == fault.context && copied.context == fault.context, "fault copy lost shared ownership");
        context = {};
        checkSnapshot(copied.context, expected);
        throw copied;
      };
      if (mode == 0) { raise(browser::Fault(context, 0x87654321, 17, "snapshot fault")); }
      if (mode == 1) { raise(browser::UnsupportedImport(context, 349)); }
      if (mode == 2) { raise(browser::WaitWatchdogExpired(context)); }
    });
    auto failure = runtime.Join();
    check(failure && retained && runtime.ActiveWorkers() == 0, "owned fault did not survive worker joining");
    const auto copiedFailure = failure;
    failure = {};
    for (uint32_t n = 0; n < 2; ++n) {
      bool matched = false;
      try { std::rethrow_exception(copiedFailure); }
      catch (const browser::UnsupportedImport& fault) {
        matched = mode == 1 && fault.ordinal == 349 && fault.address == 349 && fault.size == 0;
        checkSnapshot(fault.context, expected);
        check(fault.context == retained->context, "exception pointer lost snapshot ownership");
      } catch (const browser::WaitWatchdogExpired& fault) {
        matched = mode == 2 && fault.address == 0 && fault.size == 0;
        checkSnapshot(fault.context, expected);
      } catch (const browser::Fault& fault) {
        matched = mode == 0 && fault.address == 0x87654321 && fault.size == 17;
        checkSnapshot(fault.context, expected);
      }
      check(matched, "owned worker rethrow lost fault type, ordinal or range");
    }
    checkSnapshot(retained->context, expected);
    try { throw browser::Cancelled(expected); }
    catch (const browser::Cancelled& fault) {
      check(fault.address == expected.last_icall, "cancellation lost its call address");
      checkSnapshot(fault.context, expected);
    }
  }
};

int main() {
  testFaultSnapshots();
  browser::Runtime runtime({}, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  image.memory->Map(0x10000, 4096);
  image.memory->Commit(0x10000, 4096);
  image.image.kernel_thunk = 0x10000;
  image.imports = {{0x10000, 349}, {0x10004, 323}};
  const std::array<uint32_t, 3> imports = {0x8000015d, 0x80000143, 0};
  image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
  browser::DeclareDiagnosticImport(image, runtime, 349, false);
  browser::DeclareDiagnosticImport(image, runtime, 323, true);
  bool replaced = false;
  try { runtime.RegisterKernel(349, [](X86Context&, uint8_t*) {}); }
  catch (const browser::Fault&) { replaced = true; }
  check(replaced, "diagnostic target was replaced before binding");
  const auto coverage = browser::InspectImageImports(image, runtime, {});
  check(coverage[0].kind == browser::ImportKind::FunctionTrap, "missing function is not a diagnostic trap");
  check(coverage[1].kind == browser::ImportKind::DataGuard, "missing data is not guarded");
  {
    browser::Runtime other({}, {});
    bool rejected = false;
    try { browser::BindImageImports(image, other, {}); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected && !image.importsBound, "diagnostic imports bound to a foreign runtime");
  }
  browser::BindImageImports(image, runtime, {});
  bool sealed = false;
  try { runtime.RegisterKernel(348, [](X86Context&, uint8_t*) {}); }
  catch (const browser::Fault&) { sealed = true; }
  check(sealed && !runtime.KernelTarget(348), "published image accepted a new kernel export");
  {
    browser::PreparedImage other;
    other.memory = std::make_unique<browser::Memory>();
    other.memory->Map(0x10000, 4096);
    other.memory->Commit(0x10000, 4096);
    other.image.kernel_thunk = 0x10000;
    other.imports = {{0x10000, 349}};
    other.memory->Write({}, 0x10000, imports.data(), 4);
    bool rejected = false;
    try { browser::BindImageImports(other, runtime, {}); }
    catch (const browser::Fault&) { rejected = true; }
    uint32_t original = 0;
    other.memory->Read({}, 0x10000, &original, 4);
    check(rejected && !other.importsBound && original == imports[0], "runtime published exports into a second image");
    rejected = false;
    std::atomic<bool> entered = false;
    X86Context ownedWorker{};
    ownedWorker.host = &runtime;
    try { runtime.Launch(ownedWorker, other.memory->Base(), [&](X86Context&, uint8_t*) { entered = true; }); }
    catch (const browser::Fault&) { rejected = true; }
    runtime.Join();
    check(rejected && !entered, "bound runtime launched a foreign image worker");
    browser::Runtime foreignOwner({}, {});
    X86Context foreignWorker{};
    foreignWorker.host = &foreignOwner;
    rejected = false;
    try { runtime.Launch(foreignWorker, image.memory->Base(), [&](X86Context&, uint8_t*) { entered = true; }); }
    catch (const browser::Fault&) { rejected = true; }
    runtime.Join();
    check(rejected && !entered, "bound runtime adopted a foreign worker context");
    X86Context foreignContext{};
    foreignContext.host = &runtime;
    foreignContext.esp = 0x10800;
    rejected = false;
    try { runtime.Call(foreignContext, other.memory->Base(), coverage[0].target, {42}); }
    catch (const browser::Fault&) { rejected = true; }
    uint32_t stackValue = 1;
    other.memory->Read({}, 0x107fc, &stackValue, 4);
    check(rejected && foreignContext.esp == 0x10800 && stackValue == 0, "foreign image call mutated its stack before rejection");
  }
  X86Context context{};
  context.host = &runtime;
  context.esp = 0x10800;
  context.eax = 0xabcdef01;
  bool trapped = false;
  try { runtime.Invoke(context, image.memory->Base(), coverage[0].target); }
  catch (const browser::UnsupportedImport& fault) {
    trapped = fault.ordinal == 349 && fault.address == 349;
  }
  check(trapped && context.eax == 0xabcdef01 && context.esp == 0x10800, "function trap published fake success");
  uint32_t value = 0;
  trapped = false;
  try { image.memory->Read(context, coverage[1].target, &value, 4); }
  catch (const browser::Fault&) { trapped = true; }
  check(trapped, "guarded data returned fabricated bytes");
  check(browser::DiagnosticDataOrdinal(image, coverage[1].target + 3) == 323, "data fault lost ordinal");
  check(browser::DiagnosticDataOrdinal(image, 0) == 0, "unrelated memory fault misclassified");
  browser::Runtime foreign({}, {});
  trapped = false;
  try { browser::DeclareDiagnosticImport(image, foreign, 348, false); }
  catch (const browser::Fault&) { trapped = true; }
  check(trapped, "published image accepted diagnostic registration");
  context.host = &foreign;
  trapped = false;
  try { runtime.Invoke(context, image.memory->Base(), coverage[0].target); }
  catch (const browser::Fault&) { trapped = true; }
  check(trapped, "foreign runtime invoked a diagnostic trap");
  context.host = &runtime;
  runtime.Stop();
  trapped = false;
  try { runtime.Invoke(context, image.memory->Base(), coverage[0].target); }
  catch (const browser::Cancelled&) { trapped = true; }
  check(trapped, "retirement was replaced with an unsupported export failure");
  image.memory.reset();
  {
    browser::Runtime guest({}, {});
    browser::PreparedImage startup;
    startup.memory = std::make_unique<browser::Memory>();
    startup.memory->Map(0x10000, 4096);
    startup.memory->Commit(0x10000, 4096);
    startup.image.kernel_thunk = 0x10000;
    startup.imports = {{0x10000, 323}};
    const std::array<uint32_t, 2> dataImport = {0x80000143, 0};
    startup.memory->Write({}, 0x10000, dataImport.data(), sizeof(dataImport));
    browser::DeclareDiagnosticImport(startup, guest, 323, true);
    guest.RegisterKernel(323, [](X86Context&, uint8_t*) {});
    trapped = false;
    try { browser::BindImageImports(startup, guest, {}); }
    catch (const browser::Fault&) { trapped = true; }
    uint32_t original = 0;
    startup.memory->Read({}, 0x10000, &original, 4);
    check(trapped && !startup.importsBound && original == dataImport[0], "data guard concealed a conflicting function registration");
    guest.RegisterKernel(348, [](X86Context&, uint8_t*) {});
    check(guest.KernelTarget(348) != 0, "failed preflight sealed an unpublished runtime");
  }
  for (uint32_t n = 0; n < 3; ++n) {
    const std::array<X86FuncMapping, 1> mappings = {{{0x11000, [](X86Context& ctx, uint8_t* base) {
      auto& memory = browser::Memory::From(base);
      uint32_t argument = 1;
      memory.Read(ctx, ctx.esp + 4, &argument, 4);
      check(argument == 0 && ctx.fs_base && ctx.host, "initial thread context is invalid");
      browser::Runtime::From(ctx).Kernel(ctx, base, 349);
    }}}};
    browser::Runtime guest(mappings, {});
    browser::PreparedImage startup;
    startup.memory = std::make_unique<browser::Memory>();
    startup.memory->Map(0x10000, 4096);
    startup.memory->Commit(0x10000, 4096);
    startup.image.kernel_thunk = 0x10000;
    startup.image.entry = 0x11000;
    startup.image.stack_commit = 0x10000;
    startup.imports = {{0x10000, 349}};
    startup.memory->Write({}, 0x10000, imports.data(), 4);
    browser::DeclareDiagnosticImport(startup, guest, 349, false);
    browser::BindImageImports(startup, guest, {});
    browser::ImageExecution execution;
    trapped = false;
    try { browser::ExecuteImageEntry(startup, guest, execution); }
    catch (const browser::UnsupportedImport& fault) { trapped = fault.ordinal == 349; }
    check(trapped && execution.entered && !execution.returned, "real entry or unsupported call was hidden");
    check(execution.context.fs_base == startup.threads[0].kpcr, "fault lost guest context");
    check(browser::ImageExecutionThreadId(startup, execution) == 0x10, "initial fault lost its thread identity");
    check(guest.ActiveWorkers() == 0, "entry fault leaked a runtime worker");
  }
  {
    const std::array<X86FuncMapping, 1> mappings = {{{0x11000, [](X86Context& ctx, uint8_t*) {
      check(ctx.ebx == 0x13579bdf, "fresh image entry ran before host launch state restoration");
      ctx.eax = 42;
      ctx.esp += 8;
    }}}};
    browser::Runtime guest(mappings, {});
    browser::PreparedImage startup;
    startup.memory = std::make_unique<browser::Memory>();
    startup.image.entry = 0x11000;
    startup.importsBound = true;
    startup.boundRuntime = &guest;
    startup.tlsCallbacks.push_back(0x11000);
    browser::ImageExecution execution;
    trapped = false;
    try { browser::ExecuteImageEntry(startup, guest, execution); }
    catch (const browser::Fault&) { trapped = true; }
    check(trapped && !execution.entered && startup.threads.empty(), "unsupported TLS callback was silently skipped");
    startup.tlsCallbacks.clear();
    bool restored = false;
    browser::ExecuteImageEntry(startup, guest, execution, [&](X86Context& context) {
      check(guest.OwnsCurrentContext(context) && !restored, "launch restoration requires its one fresh owned worker");
      context.ebx = 0x13579bdf;
      restored = true;
    });
    check(restored, "host launch state was not restored");
    check(execution.entered && execution.returned && guest.ActiveWorkers() == 0, "entry return or joining failed");
  }
  for (uint32_t n = 0; n < 3; ++n) {
    const std::array<X86FuncMapping, 1> mappings = {{{0x11000, [](X86Context& ctx, uint8_t* base) {
      browser::Runtime::From(ctx).Kernel(ctx, base, 349);
    }}}};
    browser::Runtime guest(mappings, {});
    browser::PreparedImage startup;
    startup.memory = std::make_unique<browser::Memory>();
    startup.image.entry = 0x11000;
    startup.importsBound = true;
    startup.boundRuntime = &guest;
    guest.RegisterKernel(349, [&](X86Context& ctx, uint8_t* base) {
      if (n == 2) {
        guest.Launch(ctx, base, [&](X86Context& child, uint8_t*) {
          while (guest.ActiveWorkers() > 1) { std::this_thread::yield(); }
          guest.Cancel();
          guest.Checkpoint(child);
        });
        return;
      }
      ctx.eax = 0x12345678;
      guest.Cancel();
      if (n == 0) { guest.Checkpoint(ctx); }
    });
    browser::ImageExecution execution;
    bool cancelled = false;
    try { browser::ExecuteImageEntry(startup, guest, execution); }
    catch (const browser::Cancelled&) { cancelled = true; }
    check(cancelled, "cancelled entry was reported as successful execution");
    check(execution.entered && execution.returned == (n == 2), "cancellation misreported the initial entry return");
    check(execution.context.last_icall == 0x11000 && execution.context.fs_base == startup.threads[0].kpcr,
      "cancelled entry lost its guest context");
    check(guest.ActiveWorkers() == 0 && !guest.Join(), "cancelled entry leaked a worker or fabricated a guest failure");
  }
  for (uint32_t n = 0; n < 3; ++n) {
    const std::array<X86FuncMapping, 1> mappings = {{{0x11000, [](X86Context& ctx, uint8_t* base) {
      browser::Runtime::From(ctx).Kernel(ctx, base, 348);
    }}}};
    browser::Runtime guest(mappings, {});
    browser::PreparedImage startup;
    startup.memory = std::make_unique<browser::Memory>();
    startup.image.entry = 0x11000;
    startup.importsBound = true;
    startup.boundRuntime = &guest;
    guest.RegisterKernel(348, [&](X86Context& ctx, uint8_t* base) {
      guest.Launch(ctx, base, [&](X86Context& child, uint8_t* childBase) {
        while (guest.ActiveWorkers() > 1) { std::this_thread::yield(); }
        child.eax = 0x12345678;
        child.esp = 0x11223344;
        child.fs_base = 0xd2001000;
        child.last_icall = 0x55667788;
        if (n == 1) { browser::Memory::From(childBase).Read(child, 0, &child.ebx, 4); }
        throw browser::UnsupportedImport(child, 255);
      });
    });
    browser::ImageExecution execution;
    trapped = false;
    try { browser::ExecuteImageEntry(startup, guest, execution); }
    catch (const browser::Fault& fault) { trapped = fault.address == (n == 1 ? 0u : 255u); }
    check(trapped && execution.entered && execution.returned, "descendant fault changed genuine initial entry return");
    check(execution.context.eax == 0x12345678 && execution.context.esp == 0x11223344 &&
      execution.context.fs_base == 0xd2001000 && execution.context.last_icall == 0x55667788,
      "descendant fault was attributed to the initial entry context");
    check(guest.ActiveWorkers() == 0, "descendant diagnostic escaped before worker joining");
    check(browser::ImageExecutionThreadId(startup, execution) == 0, "unknown descendant was labeled as the initial thread");
    browser::ImageThread descendant{};
    descendant.kpcr = execution.context.fs_base;
    descendant.id = 0x14;
    startup.threads.push_back(descendant);
    check(browser::ImageExecutionThreadId(startup, execution) == 0x14, "descendant context did not resolve its owned thread identity");
  }
  std::puts("boot-entry-contracts-ok");
}
