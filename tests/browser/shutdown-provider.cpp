#include "browser/shutdown-provider.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto callback = [](X86Context&, uint8_t*) { throw std::runtime_error("shutdown callback executed during registration or retirement"); };

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    const std::array<X86FuncMapping, 1> mappings = {{{0x11000, callback}}};
    browser::Runtime runtime(mappings, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.memory->Ensure(0x20000, 4096);
    image.memory->Map(0x30000, 4096);
    const auto registry = browser::InstallShutdownProvider(runtime, image);
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 3> imports = {0x8000002f, 0x80000031, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 47}, {0x10004, 49}};
    browser::DeclareDiagnosticImport(image, runtime, 49, false);
    browser::BindImageImports(image, runtime, {});
    check(image.boundImports[0].kind == browser::ImportKind::Function, "shutdown registration remained trapped");
    check(image.boundImports[1].kind == browser::ImportKind::FunctionTrap, "firmware shutdown lost explicit trap");
    X86Context context{};
    context.host = &runtime;
    context.esp = 0x20f00;
    const auto record = [&](uint32_t address, uint32_t routine, uint32_t priority) {
      const std::array<uint32_t, 4> bytes = {routine, priority, 0xabcdef01, 0x12345678};
      image.memory->Write({}, address, bytes.data(), sizeof(bytes));
    };
    record(0x20100, 0x11000, 0x80000000);
    record(0x20110, 0x11000, 0x7fffffff);
    record(0x20120, 0x11000, 0x7fffffff);
    const auto invoke = [&](X86Context& ctx, uint8_t* base, uint32_t address, uint32_t mode) {
      const std::array<uint32_t, 3> frame = {0x12345, address, mode};
      ctx.esp = 0x20f00;
      ctx.eax = 0xfeedabcd;
      image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
      runtime.Invoke(ctx, base, runtime.KernelTarget(47));
      check(ctx.esp == 0x20f0c && ctx.eax == 0xfeedabcd, "void shutdown ABI changed EAX or stack cleanup");
    };
    const auto reject = [&](X86Context& ctx, uint8_t* base, uint32_t address, uint32_t mode) {
      const auto count = registry->Snapshot().size();
      bool refused = false;
      try { invoke(ctx, base, address, mode); }
      catch (const browser::Fault&) { refused = true; }
      check(refused && ctx.esp == 0x20f00 && ctx.eax == 0xfeedabcd && registry->Snapshot().size() == count,
        "invalid registration mutated state or returned success");
    };
    reject(context, image.memory->Base(), 0x20100, 1);
    runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      std::array<uint8_t, 4096> before{}, after{};
      image.memory->Read(ctx, 0x20000, before.data(), before.size());
      invoke(ctx, base, 0x20100, 0x10001);
      invoke(ctx, base, 0x20110, 1);
      invoke(ctx, base, 0x20120, 1);
      const auto retained = registry->Snapshot();
      check(retained.size() == 3 && retained[0].address == 0x20110 && retained[1].address == 0x20120 &&
        retained[2].address == 0x20100 && retained[2].priority == INT32_MIN && retained[2].routine == 0x11000,
        "shutdown registrations lost signed priority, stable ties or callback identity");
      image.memory->Read(ctx, 0x20000, after.data(), after.size());
      check(std::equal(before.begin(), before.begin() + 0xf00, after.begin()), "registration changed guest records");
      reject(ctx, base, 0x20100, 1);
      for (const uint32_t address : {0u, 0x20101u, 0x2fffcu, 0x30000u, 0xfffffffcu}) {
        reject(ctx, base, address, 1);
      }
      record(0x20130, 0x11001, 0);
      reject(ctx, base, 0x20130, 1);
      invoke(ctx, base, 0x20110, 0x100);
      check(registry->Snapshot().size() == 2, "BOOLEAN low byte did not unregister");
      reject(ctx, base, 0x20110, 0);
      invoke(ctx, base, 0x20110, 1);
      check(registry->Snapshot()[0].address == 0x20120, "reregistration lost stable priority ordering");
      for (uint32_t n = 3; n < 64; ++n) {
        const uint32_t address = 0x20100 + n * 16;
        record(address, 0x11000, n);
        invoke(ctx, base, address, 1);
      }
      record(0x20500, 0x11000, 1);
      reject(ctx, base, 0x20500, 1);
      invoke(ctx, base, 0x20100, 0);
      invoke(ctx, base, 0x20500, 1);
      check(registry->Snapshot().size() == 64, "bounded registration capacity did not recover");
      for (const uint32_t stack : {0x20ff8u, 0x30000u, 0xfffffff8u}) {
        ctx.esp = stack;
        bool refused = false;
        try { runtime.Invoke(ctx, base, runtime.KernelTarget(47)); }
        catch (const browser::Fault&) { refused = true; }
        check(refused && ctx.esp == stack, "invalid frame returned or changed stack");
      }
      browser::Runtime foreign({}, {});
      auto alien = ctx;
      alien.host = &foreign;
      reject(alien, base, 0x20100, 0);
      reject(ctx, nullptr, 0x20100, 0);
      {
        auto lease = image.memory->Borrow();
        reject(ctx, base, 0x20100, 0);
      }
      bool shutdownTrapped = false;
      try { runtime.Invoke(ctx, base, runtime.KernelTarget(49)); }
      catch (const browser::UnsupportedImport& fault) { shutdownTrapped = fault.ordinal == 49; }
      check(shutdownTrapped, "shutdown execution returned fake success");
      runtime.Cancel();
      bool cancelled = false;
      try { invoke(ctx, base, 0x20100, 0); }
      catch (const browser::Cancelled&) { cancelled = true; }
      check(cancelled && registry->Snapshot().size() == 64, "retirement changed registrations");
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(!runtime.ActiveWorkers(), "shutdown session retained active workers");
  }
  std::puts("shutdown-provider-contracts-ok");
}
