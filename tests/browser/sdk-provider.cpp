#include "browser/sdk-provider.h"
#include "sdk-fixture.h"
#include <algorithm>
#include <atomic>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto rejects = [](const auto& action) {
  bool refused = false;
  try { action(); } catch (const browser::Fault&) { refused = true; }
  check(refused, "invalid SDK observation was accepted");
};
std::atomic<uint32_t> originalCalls = 0;
thread_local X86Context* expectedContext = nullptr;
thread_local uint8_t* expectedBase = nullptr;

void SdkFixtureOriginal(X86Context& ctx, uint8_t* base, uint32_t id) {
  check(!expectedContext || (expectedContext == &ctx && expectedBase == base), "observer changed original context or base identity");
  {
    browser::Memory::PauseExecution pause;
    check(!browser::Memory::HasBorrow(), "original executed under observer memory lease");
  }
  ++originalCalls;
  auto& memory = browser::Memory::From(base);
  if (ctx.eax == 0xdeadbeef) {
    browser::Runtime::From(ctx).Kernel(ctx, base, 44);
    throw std::runtime_error("retained unsupported import returned");
  }
  if (id == 69 || id == 71) {
    uint32_t argument = 0;
    memory.Read(ctx, ctx.esp + 4, &argument, 4);
    ctx.eax = id == 69 ? (argument == 9 ? 0 : 0x30000 + argument) : argument;
    ctx.ecx ^= 0x13579bdf;
    ctx.edx += 17;
    ctx.xflags ^= 0x41;
    ctx.esp += id == 69 ? 8 : 12;
    return;
  }
  ctx.eax ^= 0x12340000 + id;
  ctx.ecx += id;
  ctx.edx ^= 0xabcdef;
  ctx.xflags ^= 0x40;
  memory.Write(ctx, 0x20f00, &ctx.eax, 4);
  ctx.esp += id == 1 ? 8 : id == 2 ? 12 : 28;
}

struct Fixture {
  browser::PreparedImage image;
  browser::Runtime runtime;
  std::shared_ptr<const browser::SdkTrace> trace;
  explicit Fixture(bool install = true, X86Func* begin = SDK_FIXTURE_BEGIN, X86Func* end = SDK_FIXTURE_END)
    : runtime([&] {
        std::vector<X86FuncMapping> mappings = {{SDK_FIXTURE_XNET_ADDRESS, SDK_FIXTURE_XNET}, {SDK_FIXTURE_WSA_ADDRESS, SDK_FIXTURE_WSA}, {SDK_FIXTURE_D3D_ADDRESS, SDK_FIXTURE_D3D}};
        if (begin) { mappings.push_back({SDK_FIXTURE_BEGIN_ADDRESS, begin}); }
        if (end) { mappings.push_back({SDK_FIXTURE_END_ADDRESS, end}); }
        return mappings;
      }(), {}) {
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.memory->Ensure(0x20000, 8192);
    image.memory->Map(0x30000, 4096);
    image.image.kernel_thunk = 0x10000;
    image.imports = {{0x10000, 44}};
    const std::array<uint32_t, 2> imports = {0x8000002c, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    browser::DeclareDiagnosticImport(image, runtime, 44, false);
    if (install) { trace = browser::InstallSdkProvider(runtime, image); }
  }
  ~Fixture() { runtime.Stop(); }
  void Bind() { browser::BindImageImports(image, runtime, {}); }
  X86Context Context(uint32_t stack = 0x20100) {
    X86Context ctx{};
    ctx.host = &runtime;
    ctx.esp = stack;
    ctx.fs_base = stack + 0x100000;
    ctx.thread_handle = stack + 8;
    ctx.eax = 0x12345678;
    ctx.ebx = 0x87654321;
    ctx.ecx = 0x11223344;
    ctx.edx = 0x55667788;
    ctx.esi = 0x99887766;
    ctx.edi = 0xaabbccdd;
    ctx.ebp = stack + 0x80;
    ctx.xflags = 0x246;
    const std::array<uint32_t, 3> frame = {0xfef001, 0xffff0202, 0x30000};
    image.memory->Write(ctx, stack, frame.data(), sizeof(frame));
    return ctx;
  }
  void Join() { if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); } }
};

int main() {
  for (const auto pair : {std::pair<X86Func*, X86Func*>{nullptr, SDK_FIXTURE_END},
      {SDK_FIXTURE_BEGIN, nullptr}, {SDK_FIXTURE_BEGIN_ORIGINAL, SDK_FIXTURE_END},
      {SDK_FIXTURE_BEGIN, SDK_FIXTURE_END_ORIGINAL}}) {
    Fixture fixture(false, pair.first, pair.second);
    const auto owner = fixture.image.diagnosticRuntime;
    rejects([&] { browser::InstallSdkProvider(fixture.runtime, fixture.image); });
    check(fixture.image.diagnosticRuntime == owner && !fixture.runtime.HasPublishedImage(), "signature mapping rejection changed image ownership");
  }
  {
    Fixture fixture(false, nullptr, nullptr);
    fixture.trace = browser::InstallSdkProvider(fixture.runtime, fixture.image);
    fixture.Bind();
    auto ctx = fixture.Context();
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      const auto before = current;
      const auto calls = originalCalls.load();
      rejects([&] { SDK_FIXTURE_BEGIN(current, base); });
      rejects([&] { SDK_FIXTURE_END(current, base); });
      check(!std::memcmp(&current, &before, sizeof(current)) && calls == originalCalls.load() && fixture.trace->Snapshot().total == 0,
        "absent signature mappings delegated or mutated state");
      SDK_FIXTURE_XNET(current, base);
    });
    fixture.Join();
    check(fixture.trace->Snapshot().total == 1, "absent signature mappings changed M3 SDK behavior");
  }
  {
    Fixture fixture;
    fixture.Bind();
    std::atomic<uint32_t> ready = 0;
    for (uint32_t n = 0; n < 2; ++n) {
      auto ctx = fixture.Context(0x20100 + n * 0x100);
      fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
        const uint32_t marker = (current.fs_base - 0x120100) / 0x100;
        const uint32_t stack = current.esp;
        ++ready;
        while (ready.load() < 2) { std::this_thread::yield(); }
        for (uint32_t n = 0; n < 40; ++n) {
          current.esp = stack;
          std::array<uint32_t, 3> frame = {marker ? 0x3f724u : 0x3f58cu, marker, 0xffffffff};
          fixture.image.memory->Write(current, stack, frame.data(), sizeof(frame));
          fixture.runtime.Invoke(current, base, SDK_FIXTURE_BEGIN_ADDRESS);
          frame[1] = current.eax;
          current.esp = stack;
          fixture.image.memory->Write(current, stack, frame.data(), sizeof(frame));
          fixture.runtime.Invoke(current, base, SDK_FIXTURE_END_ADDRESS);
        }
      });
    }
    fixture.Join();
    const auto trace = fixture.trace->Snapshot();
    check(trace.count == 64 && trace.total == 320, "signature trace lost concurrent entries or exceeded its bound");
    for (size_t n = 0; n < trace.count; ++n) {
      const auto& event = trace.events[n];
      const uint32_t marker = (event.fsBase - 0x120100) / 0x100;
      check(event.sequence == 257 + n && event.workerId && marker < 2 && event.id >= 69 && event.id <= 72 &&
        event.returnAddress == (marker ? 0x3f724u : 0x3f58cu) &&
        event.arguments[0] == (event.id < 71 ? marker : 0x30000 + marker) &&
        event.arguments[1] == (event.id % 2 ? 0u : 0x30000 + marker) &&
        std::all_of(event.arguments.begin() + 2, event.arguments.end(), [](uint32_t word) { return word == 0; }),
        "signature concurrent snapshots mixed workers, results or extra data");
    }
  }
  for (const uint32_t mode : {0u, 1u, 2u, 4u, 8u, 9u, 0xffffffffu}) {
    Fixture fixture;
    fixture.Bind();
    auto ctx = fixture.Context();
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      for (uint32_t n = 0; n < 2; ++n) {
        current = fixture.Context();
        const std::array<uint32_t, 3> frame = {n ? 0x3f79au : 0x3f724u, n ? 0x30000u : mode, 0xffffffff};
        fixture.image.memory->Write(current, current.esp, frame.data(), sizeof(frame));
        auto expected = current;
        const auto original = n ? SDK_FIXTURE_END_ORIGINAL : SDK_FIXTURE_BEGIN_ORIGINAL;
        const auto observed = n ? SDK_FIXTURE_END : SDK_FIXTURE_BEGIN;
        original(expected, base);
        const auto calls = originalCalls.load();
        expectedContext = &current;
        expectedBase = base;
        observed(current, base);
        expectedContext = nullptr;
        check(originalCalls.load() == calls + 1 && !std::memcmp(&current, &expected, sizeof(current)),
          "signature wrapper changed original identity, ABI, result or call count");
      }
    });
    fixture.Join();
    const auto trace = fixture.trace->Snapshot();
    check(trace.total == 4 && trace.count == 4, "signature entry/return observations missing");
    for (size_t n = 0; n < 4; ++n) {
      const auto& event = trace.events[n];
      check(event.id == 69 + n && event.sequence == n + 1 && event.workerId &&
        event.behavior == browser::SdkBehavior::Original && event.returnAddress == (n < 2 ? 0x3f724u : 0x3f79au) &&
        event.esp == ctx.esp && event.argumentCount == (n % 2 ? 2 : 1) &&
        event.arguments[0] == (n < 2 ? mode : 0x30000u) &&
        std::all_of(event.presentation.begin(), event.presentation.end(), [](uint32_t word) { return word == 0; }),
        "signature metadata includes wrong caller, mode, identity or extra data");
    }
    check(trace.events[1].arguments[1] == (mode == 9 ? 0 : 0x30000 + mode) && trace.events[3].arguments[1] == 0x30000,
      "signature trace invented a context or finalizer success");
  }
  for (const uint32_t result : {0u, 50u, 0xffffffffu}) {
    Fixture fixture;
    fixture.Bind();
    auto ctx = fixture.Context();
    const std::array<uint32_t, 3> frame = {0x3f79a, result, 0xffffffff};
    fixture.image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      SDK_FIXTURE_END(current, base);
      check(current.eax == result && current.esp == ctx.esp + 12, "finalizer status or stack was rewritten");
    });
    fixture.Join();
    const auto trace = fixture.trace->Snapshot();
    check(trace.total == 2 && trace.events[1].arguments[1] == result, "finalizer status was normalized to success");
  }
  for (const auto observed : {SDK_FIXTURE_BEGIN, SDK_FIXTURE_END}) {
    Fixture fixture;
    fixture.Bind();
    auto ctx = fixture.Context();
    rejects([&] { observed(ctx, fixture.image.memory->Base()); });
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      const auto unchanged = [&](const auto& action) {
        const auto before = current;
        const auto calls = originalCalls.load();
        const auto total = fixture.trace->Snapshot().total;
        rejects(action);
        check(!std::memcmp(&current, &before, sizeof(current)) && calls == originalCalls.load() &&
          total == fixture.trace->Snapshot().total, "invalid signature admission changed state");
      };
      unchanged([&] { observed(current, nullptr); });
      browser::Memory foreign;
      unchanged([&] { observed(current, foreign.Base()); });
      auto alias = current;
      rejects([&] { observed(alias, base); });
      browser::Runtime foreignRuntime({}, {});
      alias.host = &foreignRuntime;
      rejects([&] { observed(alias, base); });
      {
        auto lease = fixture.image.memory->Borrow();
        unchanged([&] { observed(current, base); });
      }
      for (const uint32_t stack : {0x21ffcu, 0x30000u, 0xfffffffcu}) {
        current.esp = stack;
        unchanged([&] { observed(current, base); });
      }
      current = fixture.Context();
      fixture.image.importsBound = false;
      unchanged([&] { observed(current, base); });
      fixture.image.importsBound = true;
      fixture.image.boundRuntime = &foreignRuntime;
      unchanged([&] { observed(current, base); });
      fixture.image.boundRuntime = &fixture.runtime;
      auto memory = std::move(fixture.image.memory);
      fixture.image.memory = std::make_unique<browser::Memory>();
      unchanged([&] { observed(current, base); });
      fixture.image.memory = std::move(memory);
      fixture.runtime.Cancel();
      unchanged([&] { observed(current, base); });
    });
    fixture.Join();
  }
  for (const auto observed : {SDK_FIXTURE_BEGIN, SDK_FIXTURE_END}) {
    Fixture fixture;
    fixture.Bind();
    auto ctx = fixture.Context();
    ctx.eax = 0xdeadbeef;
    const auto before = ctx;
    const auto calls = originalCalls.load();
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) { observed(current, base); });
    bool trapped = false;
    try { fixture.Join(); }
    catch (const browser::UnsupportedImport& fault) {
      trapped = fault.ordinal == 44 && fault.context && !std::memcmp(fault.context.get(), &before, sizeof(before));
    }
    check(trapped && originalCalls.load() == calls + 1 && fixture.trace->Snapshot().count == 1,
      "signature original fault swallowed, duplicated or given a return event");
  }
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Memory memory;
    browser::Runtime runtime({}, {});
    uint32_t calls = 0;
    runtime.InstallSdkObserver(memory, [&](X86Context& current, uint32_t id, uint64_t worker) {
      check(id == 3 && worker != 0, "SDK mutable admission lost identity");
      ++calls;
      current.eax = 0x12345678;
      current.esp += 28;
      return false;
    });
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    X86Context context{};
    context.host = &runtime;
    context.esp = 0x80001000;
    rejects([&] { browser::Runtime::ObserveSdkEntry(context, memory.Base(), 3); });
    runtime.Launch(context, memory.Base(), [&](X86Context& current, uint8_t* base) {
      auto copied = current;
      rejects([&] { browser::Runtime::ObserveSdkEntry(copied, base, 3); });
      rejects([&] { browser::Runtime::ObserveSdkEntry(current, nullptr, 3); });
      {
        auto lease = memory.Borrow();
        rejects([&] { browser::Runtime::ObserveSdkEntry(current, base, 3); });
      }
      check(calls == 0 && current.esp == context.esp && current.eax == 0, "rejected mutable SDK entry changed state");
      check(!browser::Runtime::ObserveSdkEntry(current, base, 3) && current.eax == 0x12345678 && current.esp == context.esp + 28,
        "owned mutable SDK service cannot complete its ABI");
    });
    check(!runtime.Join() && calls == 1 && runtime.ActiveWorkers() == 0, "mutable SDK admission did not join exactly once");
    runtime.Cancel();
    rejects([&] { browser::Runtime::ObserveSdkEntry(context, memory.Base(), 3); });
    check(calls == 1, "retired SDK admission invoked service");
  }
  {
    Fixture fixture(false);
    fixture.trace = browser::InstallSdkProvider(fixture.runtime, fixture.image, browser::SdkPolicy::UnavailableXNet);
    fixture.Bind();
    auto ctx = fixture.Context();
    const std::array<uint32_t, 7> frame = {0x1234, 0, 1, 2, 3, 0x20500, 0x20500};
    const std::array<uint32_t, 13> parameters = {0, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    fixture.image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    fixture.image.memory->Write(ctx, frame[5], parameters.data(), sizeof(parameters));
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      const auto calls = originalCalls.load();
      auto expected = current;
      SDK_FIXTURE_D3D_ORIGINAL(expected, base);
      const auto before = originalCalls.load();
      expectedContext = &current;
      expectedBase = base;
      SDK_FIXTURE_D3D(current, base);
      expectedContext = nullptr;
      check(originalCalls.load() == before + 1 && !std::memcmp(&current, &expected, sizeof(current)), "device entry did not delegate unchanged exactly once");
      check(originalCalls.load() == calls + 2, "device original call count is incorrect");
    });
    fixture.Join();
    const auto event = fixture.trace->Snapshot().events[0];
    check(event.id == 3 && event.argumentCount == 6 && event.arguments[5] == frame[6] &&
      event.presentation == parameters && event.behavior == browser::SdkBehavior::Original, "device entry snapshot is incorrect");
  }
  {
    Fixture fixture;
    fixture.Bind();
    auto ctx = fixture.Context();
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      check(browser::Runtime::ObserveSdkEntry(current, base, 66) && browser::Runtime::ObserveSdkEntry(current, base, 67),
        "SDK release observations without a graphics owner did not delegate");
      check(browser::Runtime::ObserveSdkEntry(current, base, 68), "SDK overlay boundary without a graphics owner did not delegate");
      rejects([&] { browser::Runtime::ObserveSdkEntry(current, base, 73); });
      const auto unchanged = [&](const auto& action) {
        const auto before = current;
        const auto calls = originalCalls.load();
        const auto count = fixture.trace->Snapshot().total;
        rejects(action);
        check(!std::memcmp(&current, &before, sizeof(current)) && originalCalls.load() == calls &&
          fixture.trace->Snapshot().total == count, "invalid device entry changed state");
      };
      std::array<uint32_t, 7> frame = {1, 2, 3, 4, 5, 0, 0xffffffff};
      for (const uint32_t pointer : {0u, 0x30000u, 0xfffffff0u, 0x21fe0u}) {
        frame[5] = pointer;
        fixture.image.memory->Write(current, current.esp, frame.data(), sizeof(frame));
        unchanged([&] { SDK_FIXTURE_D3D(current, base); });
      }
      frame[5] = 0x20501;
      fixture.image.memory->Write(current, current.esp, frame.data(), sizeof(frame));
      unchanged([&] { SDK_FIXTURE_D3D(current, nullptr); });
      {
        auto lease = fixture.image.memory->Borrow();
        unchanged([&] { SDK_FIXTURE_D3D(current, base); });
      }
      auto alias = current;
      rejects([&] { SDK_FIXTURE_D3D(alias, base); });
      SDK_FIXTURE_D3D(current, base);
      check(fixture.trace->Snapshot().events[0].arguments[5] == 0xffffffff, "output scalar was dereferenced");
    });
    fixture.Join();
  }
  {
    Fixture fixture;
    fixture.Bind();
    auto ctx = fixture.Context();
    std::array<uint32_t, 7> frame = {1, 2, 3, 4, 5, 0x20500, 0};
    fixture.image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    ctx.eax = 0xdeadbeef;
    const auto before = ctx;
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) { SDK_FIXTURE_D3D(current, base); });
    bool trapped = false;
    try { fixture.Join(); }
    catch (const browser::UnsupportedImport& fault) {
      trapped = fault.ordinal == 44 && fault.context && !std::memcmp(fault.context.get(), &before, sizeof(before));
    }
    check(trapped && fixture.trace->Snapshot().count == 1, "device original fault was altered");
  }
  {
    Fixture fixture;
    fixture.Bind();
    auto ctx = fixture.Context();
    const std::array<uint32_t, 7> frame = {1, 2, 3, 4, 5, 0x20500, 0};
    fixture.image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      fixture.runtime.Cancel();
      const auto before = current;
      const auto calls = originalCalls.load();
      rejects([&] { SDK_FIXTURE_D3D(current, base); });
      check(!std::memcmp(&current, &before, sizeof(current)) && originalCalls.load() == calls &&
        fixture.trace->Snapshot().total == 0, "cancelled device call changed state");
    });
    fixture.Join();
  }
  {
    Fixture fixture;
    fixture.Bind();
    std::atomic<uint32_t> ready = 0;
    for (uint32_t n = 0; n < 2; ++n) {
      auto ctx = fixture.Context(0x20100 + n * 0x100);
      const uint32_t marker = n + 1;
      const uint32_t pp = 0x20500 + n * 0x100;
      const std::array<uint32_t, 7> frame = {marker, marker, marker, marker, marker, pp, marker};
      std::array<uint32_t, 13> parameters{};
      parameters.fill(marker);
      fixture.image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
      fixture.image.memory->Write(ctx, pp, parameters.data(), sizeof(parameters));
      fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
        ++ready;
        while (ready.load() < 2) { std::this_thread::yield(); }
        for (uint32_t n = 0; n < 40; ++n) {
          current.esp = current.fs_base - 0x100000;
          fixture.runtime.Invoke(current, base, SDK_FIXTURE_D3D_ADDRESS);
        }
      });
    }
    fixture.Join();
    const auto trace = fixture.trace->Snapshot();
    check(trace.total == 80 && trace.count == 64, "device trace lost concurrent entries");
    for (size_t n = 0; n < trace.count; ++n) {
      const auto& event = trace.events[n];
      const auto marker = event.returnAddress;
      check(event.sequence == 17 + n && event.workerId && event.id == 3 && (marker == 1 || marker == 2) &&
        event.arguments[0] == marker && event.arguments[5] == marker &&
        std::all_of(event.presentation.begin(), event.presentation.end(), [&](uint32_t word) { return word == marker; }),
        "device trace mixed concurrent snapshots");
    }
  }
  {
    Fixture fixture(false);
    fixture.trace = browser::InstallSdkProvider(fixture.runtime, fixture.image, browser::SdkPolicy::UnavailableXNet);
    fixture.Bind();
    auto ctx = fixture.Context();
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      for (uint32_t n = 0; n < 2; ++n) {
        current = fixture.Context();
        const uint32_t argument = n ? 0xffffffff : 0;
        fixture.image.memory->Write(current, current.esp + 4, &argument, 4);
        auto expected = current;
        expected.eax = 0x276b;
        expected.esp += 8;
        std::array<uint32_t, 3> before{};
        fixture.image.memory->Read(current, current.esp, before.data(), sizeof(before));
        const auto calls = originalCalls.load();
        SDK_FIXTURE_XNET(current, base);
        std::array<uint32_t, 3> after{};
        fixture.image.memory->Read(current, expected.esp - 8, after.data(), sizeof(after));
        check(!std::memcmp(&current, &expected, sizeof(current)) && before == after && originalCalls.load() == calls,
          "unavailable XNet changed guest state or delegated");
      }
      current = fixture.Context();
      const auto calls = originalCalls.load();
      SDK_FIXTURE_WSA(current, base);
      check(originalCalls.load() == calls + 1, "unavailable policy intercepted WSA");
      current = fixture.Context();
      const auto unchanged = [&](const auto& action) {
        const auto before = current;
        const auto total = fixture.trace->Snapshot().total;
        const auto original = originalCalls.load();
        rejects(action);
        check(!std::memcmp(&current, &before, sizeof(current)) && fixture.trace->Snapshot().total == total &&
          originalCalls.load() == original, "unavailable rejection produced partial output");
      };
      unchanged([&] { SDK_FIXTURE_XNET(current, nullptr); });
      auto alias = current;
      rejects([&] { SDK_FIXTURE_XNET(alias, base); });
      {
        auto lease = fixture.image.memory->Borrow();
        unchanged([&] { SDK_FIXTURE_XNET(current, base); });
      }
      for (const uint32_t stack : {0x21ffcu, 0x30000u, 0xfffffffcu}) {
        current.esp = stack;
        unchanged([&] { SDK_FIXTURE_XNET(current, base); });
      }
      current = fixture.Context();
      fixture.image.importsBound = false;
      unchanged([&] { SDK_FIXTURE_XNET(current, base); });
      fixture.image.importsBound = true;
      fixture.runtime.Cancel();
      unchanged([&] { SDK_FIXTURE_XNET(current, base); });
    });
    fixture.Join();
    const auto trace = fixture.trace->Snapshot();
    check(trace.count == 3 && trace.events[0].behavior == browser::SdkBehavior::Unavailable &&
      trace.events[1].behavior == browser::SdkBehavior::Unavailable && trace.events[2].behavior == browser::SdkBehavior::Original,
      "SDK trace omitted actual behavior");
  }
  {
    Fixture fixture;
    auto& runtime = fixture.runtime;
    auto& image = fixture.image;
    check(!runtime.HasPublishedImage() && fixture.trace->Snapshot().total == 0, "SDK install published or executed image");
    check(runtime.KernelTarget(44) && image.boundImports.empty(), "SDK install changed kernel declarations");
    rejects([&] { browser::InstallSdkProvider(runtime, image); });
    fixture.Bind();
    check(image.boundImports.size() == 1 && image.boundImports[0].kind == browser::ImportKind::FunctionTrap,
      "SDK observer claimed physical kernel coverage");
    rejects([&] { browser::InstallSdkProvider(runtime, image); });
    auto ctx = fixture.Context();
    rejects([&] { SDK_FIXTURE_XNET(ctx, image.memory->Base()); });
    runtime.Launch(ctx, image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      for (uint32_t n = 0; n < 2; ++n) {
        current = fixture.Context();
        auto expected = current;
        const auto original = n ? SDK_FIXTURE_WSA_ORIGINAL : SDK_FIXTURE_XNET_ORIGINAL;
        const auto observed = n ? SDK_FIXTURE_WSA : SDK_FIXTURE_XNET;
        original(expected, base);
        uint32_t expectedOutput = 0;
        image.memory->Read(current, 0x20f00, &expectedOutput, 4);
        const uint32_t sentinel = 0xa5a5a5a5;
        image.memory->Write(current, 0x20f00, &sentinel, 4);
        const auto before = current;
        const uint32_t calls = originalCalls.load();
        expectedContext = &current;
        expectedBase = base;
        observed(current, base);
        expectedContext = nullptr;
        uint32_t actualOutput = 0;
        image.memory->Read(current, 0x20f00, &actualOutput, 4);
        check(originalCalls.load() == calls + 1 && !std::memcmp(&current, &expected, sizeof(current)) && expectedOutput == actualOutput,
          "SDK strong override did not preserve original ABI and memory effects exactly once");
        const auto trace = fixture.trace->Snapshot();
        const auto& event = trace.events[n];
        check(trace.count == n + 1 && trace.total == n + 1 && event.sequence == n + 1 && event.workerId &&
          event.id == n + 1 && event.fsBase == before.fs_base && event.threadHandle == before.thread_handle &&
          event.esp == before.esp && event.returnAddress == 0xfef001 && event.argumentCount == n + 1 &&
          event.arguments[0] == 0xffff0202 && event.arguments[1] == (n ? 0x30000u : 0u), "SDK entry frame or identity is incorrect");
      }
      current = fixture.Context();
      const auto before = current;
      const uint64_t total = fixture.trace->Snapshot().total;
      const uint32_t calls = originalCalls.load();
      const auto unchanged = [&](const auto& action) {
        rejects(action);
        check(!std::memcmp(&current, &before, sizeof(current)) && originalCalls.load() == calls && fixture.trace->Snapshot().total == total,
          "rejected SDK entry mutated context, trace or delegated");
      };
      unchanged([&] { SDK_FIXTURE_XNET(current, nullptr); });
      browser::Memory foreignMemory;
      unchanged([&] { SDK_FIXTURE_XNET(current, foreignMemory.Base()); });
      browser::Runtime foreign({}, {});
      auto alien = current;
      rejects([&] { SDK_FIXTURE_XNET(alien, base); });
      alien.host = &foreign;
      rejects([&] { SDK_FIXTURE_XNET(alien, base); });
      alien = current;
      ++alien.fs_base;
      rejects([&] { SDK_FIXTURE_XNET(alien, base); });
      {
        auto lease = image.memory->Borrow();
        unchanged([&] { SDK_FIXTURE_XNET(current, base); });
      }
      for (const uint32_t stack : {0x21ffcu, 0x30000u, 0xfffffffcu}) {
        current.esp = stack;
        rejects([&] { SDK_FIXTURE_XNET(current, base); });
        rejects([&] { SDK_FIXTURE_WSA(current, base); });
      }
      current.esp = 0x21ff8;
      const std::array<uint32_t, 2> shortFrame = {0xabcdef, 0x30000};
      image.memory->Write(current, current.esp, shortFrame.data(), sizeof(shortFrame));
      rejects([&] { SDK_FIXTURE_WSA(current, base); });
      SDK_FIXTURE_XNET(current, base);
      check(fixture.trace->Snapshot().total == total + 1, "complete 8 byte XNet frame was rejected");
      current = before;
      auto retainedMemory = std::move(image.memory);
      image.memory = std::make_unique<browser::Memory>();
      rejects([&] { SDK_FIXTURE_XNET(current, base); });
      image.memory = std::move(retainedMemory);
      image.importsBound = false;
      rejects([&] { SDK_FIXTURE_XNET(current, base); });
      image.importsBound = true;
      image.boundRuntime = &foreign;
      rejects([&] { SDK_FIXTURE_XNET(current, base); });
      image.boundRuntime = &runtime;
      runtime.Cancel();
      rejects([&] { SDK_FIXTURE_XNET(current, base); });
      check(fixture.trace->Snapshot().total == total + 1 && originalCalls.load() == calls + 1,
        "stale or cancelled image appended SDK records or delegated");
    });
    fixture.Join();
  }
  {
    Fixture fixture;
    fixture.Bind();
    auto ctx = fixture.Context();
    ctx.eax = 0xdeadbeef;
    const auto before = ctx;
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) { SDK_FIXTURE_WSA(current, base); });
    bool trapped = false;
    try { fixture.Join(); }
    catch (const browser::UnsupportedImport& fault) {
      trapped = fault.ordinal == 44 && fault.context && !std::memcmp(fault.context.get(), &before, sizeof(before));
    }
    check(trapped && fixture.trace->Snapshot().count == 1 && !fixture.runtime.ActiveWorkers(), "original physical trap was swallowed or altered");
    const auto kernel = fixture.runtime.SnapshotKernelTrace();
    check(kernel.count == 2 && kernel.events[0].ordinal == 44 && kernel.events[1].phase == browser::Runtime::KernelTracePhase::Fault,
      "SDK observer contaminated physical kernel trace");
  }
  {
    Fixture fixture;
    fixture.Bind();
    std::atomic<uint32_t> ready = 0;
    for (uint32_t n = 0; n < 2; ++n) {
      auto ctx = fixture.Context(0x20100 + n * 0x100);
      fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
        ++ready;
        while (ready.load() < 2) { std::this_thread::yield(); }
        for (uint32_t n = 0; n < 40; ++n) {
          current.esp = current.fs_base - 0x100000;
          fixture.runtime.Invoke(current, base, SDK_FIXTURE_XNET_ADDRESS);
        }
      });
    }
    fixture.Join();
    const auto trace = fixture.trace->Snapshot();
    check(trace.total == 80 && trace.count == 64, "SDK ledger is not bounded or lost concurrent entries");
    for (size_t n = 0; n < trace.count; ++n) {
      check(trace.events[n].sequence == 17 + n && trace.events[n].workerId && trace.events[n].id == 1, "SDK ledger order or worker identity is invalid");
    }
    check(fixture.runtime.SnapshotKernelTrace().count == 0, "public SDK trace was counted as kernel calls");
  }
  {
    Fixture fixture(false);
    fixture.Bind();
    auto ctx = fixture.Context();
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      rejects([&] { SDK_FIXTURE_XNET(current, base); });
    });
    fixture.Join();
  }
  {
    Fixture fixture;
    auto ctx = fixture.Context();
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) {
      rejects([&] { SDK_FIXTURE_XNET(current, base); });
    });
    fixture.Join();
    Fixture target(false);
    target.runtime.Launch({}, target.image.memory->Base(), [](X86Context&, uint8_t*) {});
    target.Join();
    rejects([&] { browser::InstallSdkProvider(target.runtime, target.image); });
  }
  {
    Fixture fixture;
    auto retainedMemory = std::move(fixture.image.memory);
    fixture.image.memory = std::make_unique<browser::Memory>();
    rejects([&] { fixture.runtime.PublishImageMemory(*fixture.image.memory, fixture.runtime.KernelRevision(), [] {}); });
    fixture.image.memory = std::move(retainedMemory);
  }
  {
    Fixture fixture;
    const uint32_t cell = 0x13579bdf;
    fixture.image.memory->Write({}, 0x20800, &cell, 4);
    fixture.image.imports.push_back({0x10004, 40});
    const std::array<uint32_t, 3> imports = {0x8000002c, 0x80000028, 0};
    fixture.image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    const std::array<browser::KernelDataExport, 1> data = {{{40, 0x20800, 4}}};
    browser::BindImageImports(fixture.image, fixture.runtime, data);
    const auto bound = fixture.image.boundImports;
    uint32_t beforeSlot = 0;
    fixture.image.memory->Read({}, 0x10004, &beforeSlot, 4);
    auto ctx = fixture.Context();
    fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) { SDK_FIXTURE_XNET(current, base); });
    fixture.Join();
    uint32_t afterCell = 0, afterSlot = 0;
    fixture.image.memory->Read({}, 0x20800, &afterCell, 4);
    fixture.image.memory->Read({}, 0x10004, &afterSlot, 4);
    check(afterCell == cell && beforeSlot == afterSlot && afterSlot == 0x20800 && fixture.image.boundImports[1].kind == browser::ImportKind::Data &&
      fixture.image.boundImports[1].target == bound[1].target, "SDK observation changed published kernel data or import ownership");
  }
  {
    Fixture fixture(false);
    browser::Runtime foreign({}, {});
    fixture.image.diagnosticRuntime = &foreign;
    rejects([&] { browser::InstallSdkProvider(fixture.runtime, fixture.image); });
    fixture.image.diagnosticRuntime = &fixture.runtime;
    fixture.trace = browser::InstallSdkProvider(fixture.runtime, fixture.image);
    check(fixture.trace->Snapshot().total == 0, "rejected foreign image partially installed SDK binding");
  }
  for (const uint32_t entry : {SDK_FIXTURE_XNET_ADDRESS, SDK_FIXTURE_WSA_ADDRESS, SDK_FIXTURE_D3D_ADDRESS, SDK_FIXTURE_BEGIN_ADDRESS, SDK_FIXTURE_END_ADDRESS}) {
    Fixture fixture(false);
    fixture.image.image.entry = entry;
    rejects([&] { browser::InstallSdkProvider(fixture.runtime, fixture.image); });
    fixture.image.image.entry = 0;
    fixture.trace = browser::InstallSdkProvider(fixture.runtime, fixture.image);
  }
  {
    Fixture fixture(false);
    fixture.image.diagnosticRuntime = nullptr;
    fixture.trace = browser::InstallSdkProvider(fixture.runtime, fixture.image);
    check(fixture.image.diagnosticRuntime == &fixture.runtime, "fresh SDK install did not claim exact image owner");
  }
  {
    Fixture fixture(false);
    fixture.runtime.Cancel();
    fixture.image.diagnosticRuntime = nullptr;
    rejects([&] { browser::InstallSdkProvider(fixture.runtime, fixture.image); });
    check(!fixture.image.diagnosticRuntime, "cancelled SDK install partially claimed image ownership");
  }
  {
    std::weak_ptr<const browser::SdkTrace> lifetime;
    {
      Fixture fixture;
      fixture.Bind();
      lifetime = fixture.trace;
      fixture.trace.reset();
      check(!lifetime.expired(), "SDK callback borrowed session trace lifetime");
      auto ctx = fixture.Context();
      fixture.runtime.Launch(ctx, fixture.image.memory->Base(), [&](X86Context& current, uint8_t* base) { SDK_FIXTURE_XNET(current, base); });
      fixture.Join();
      check(lifetime.lock()->Snapshot().count == 1, "owned SDK trace did not survive client handle release");
    }
    check(lifetime.expired(), "SDK observer leaked lifetime beyond its runtime");
  }
  std::puts("sdk-provider-contracts-ok");
}
