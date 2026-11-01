#include "browser/dpc-provider.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto callback = [](X86Context&, uint8_t*) { throw std::runtime_error("DPC callback executed during initialization or retirement"); };

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    const std::array<X86FuncMapping, 1> mappings = {{{0x11000, callback}}};
    browser::Runtime runtime(mappings, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.memory->Ensure(0x20000, 4096);
    image.memory->Ensure(0x21000, 4096);
    image.memory->Map(0x30000, 4096);
    browser::InstallDpcProvider(runtime, image);
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 4> imports = {0x8000006b, 0x80000077, 0x80000089, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 107}, {0x10004, 119}, {0x10008, 137}};
    for (const uint32_t ordinal : {119u, 137u}) { browser::DeclareDiagnosticImport(image, runtime, ordinal, false); }
    browser::BindImageImports(image, runtime, {});
    check(image.boundImports[0].kind == browser::ImportKind::Function &&
      image.boundImports[1].kind == browser::ImportKind::FunctionTrap &&
      image.boundImports[2].kind == browser::ImportKind::FunctionTrap, "DPC initialization or scheduling binding is incorrect");
    X86Context context{};
    context.host = &runtime;
    const auto invoke = [&](X86Context& ctx, uint8_t* base, uint32_t address, uint32_t routine, uint32_t deferredContext, uint32_t stack = 0x21f00) {
      const std::array<uint32_t, 4> frame = {0x12345, address, routine, deferredContext};
      ctx.esp = stack;
      ctx.eax = 0xfeedabcd;
      ctx.ecx = 0x12345678;
      ctx.edx = 0x87654321;
      ctx.xflags = 0x246;
      image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
      runtime.Invoke(ctx, base, runtime.KernelTarget(107));
      check(ctx.esp == stack + 16 && ctx.eax == 0xfeedabcd && ctx.ecx == 0x12345678 && ctx.edx == 0x87654321 &&
        ctx.xflags == 0x246, "DPC void ABI changed registers or stack cleanup");
    };
    const auto reject = [&](X86Context& ctx, uint8_t* base, uint32_t address, uint32_t routine) {
      std::array<uint8_t, 4096> before{}, after{};
      image.memory->Read({}, 0x20000, before.data(), before.size());
      bool refused = false;
      try { invoke(ctx, base, address, routine, 0); }
      catch (const browser::Fault&) { refused = true; }
      image.memory->Read({}, 0x20000, after.data(), after.size());
      check(refused && ctx.esp == 0x21f00 && ctx.eax == 0xfeedabcd && before == after,
        "invalid DPC initialization changed storage or returned success");
    };
    reject(context, image.memory->Base(), 0x20100, 0x11000);
    runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      for (const uint32_t address : {0x20100u, 0x20ff0u}) {
        for (const uint32_t deferredContext : {0u, 0xffffffffu, 0x30000u}) {
          std::array<uint8_t, 36> expected;
          expected.fill(0xa5);
          image.memory->Write(ctx, address - 4, expected.data(), expected.size());
          invoke(ctx, base, address, 0x11000, deferredContext);
          expected[4] = 19;
          expected[5] = 0;
          expected[6] = 0;
          const uint32_t routine = 0x11000;
          std::memcpy(expected.data() + 16, &routine, 4);
          std::memcpy(expected.data() + 20, &deferredContext, 4);
          std::array<uint8_t, 36> actual;
          image.memory->Read(ctx, address - 4, actual.data(), actual.size());
          check(actual == expected, "DPC initialization lost fields, bounds or arbitrary context");
          invoke(ctx, base, address, 0x11000, 0);
        }
      }
      const std::array<uint32_t, 4> aliasedFrame = {0x12345, 0x21f00, 0x11000, 0xffffffff};
      invoke(ctx, base, 0x21f00, 0x11000, 0xffffffff);
      std::array<uint32_t, 7> aliased;
      image.memory->Read(ctx, 0x21f00, aliased.data(), sizeof(aliased));
      check(aliased[0] == 19 && aliased[1] == aliasedFrame[1] && aliased[2] == aliasedFrame[2] &&
        aliased[3] == 0x11000 && aliased[4] == 0xffffffff, "DPC output alias corrupted unsnapshotted arguments");
      for (const uint32_t address : {0u, 0x20101u, 0x21ff0u, 0x30000u, 0xfffffff0u}) {
        reject(ctx, base, address, 0x11000);
      }
      reject(ctx, base, 0x20100, 0);
      reject(ctx, base, 0x20100, 0x11001);
      reject(ctx, base, 0x20100, runtime.KernelTarget(119));
      for (const uint32_t stack : {0x21ff8u, 0x30000u, 0xfffffff8u}) {
        ctx.esp = stack;
        bool refused = false;
        try { runtime.Invoke(ctx, base, runtime.KernelTarget(107)); }
        catch (const browser::Fault&) { refused = true; }
        check(refused && ctx.esp == stack, "invalid DPC frame changed stack or returned success");
      }
      browser::Runtime foreign({}, {});
      auto alien = ctx;
      alien.host = &foreign;
      reject(alien, base, 0x20100, 0x11000);
      reject(ctx, nullptr, 0x20100, 0x11000);
      {
        auto lease = image.memory->Borrow();
        reject(ctx, base, 0x20100, 0x11000);
      }
      for (const uint32_t ordinal : {119u, 137u}) {
        bool trapped = false;
        try { runtime.Invoke(ctx, base, runtime.KernelTarget(ordinal)); }
        catch (const browser::UnsupportedImport& fault) { trapped = fault.ordinal == ordinal; }
        check(trapped, "unsupported DPC scheduling returned success");
      }
      runtime.Cancel();
      reject(ctx, base, 0x20100, 0x11000);
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(!runtime.ActiveWorkers(), "DPC session retained active workers");
  }
  std::puts("dpc-provider-contracts-ok");
}
