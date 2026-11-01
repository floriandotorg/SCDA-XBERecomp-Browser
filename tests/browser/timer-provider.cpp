#include "browser/timer-provider.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime runtime({}, {});
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.memory->Ensure(0x20000, 4096);
    image.memory->Ensure(0x21000, 4096);
    image.memory->Map(0x30000, 4096);
    browser::InstallTimerProvider(runtime, image);
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 5> imports = {0x80000071, 0x80000095, 0x80000096, 0x80000061, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 113}, {0x10004, 149}, {0x10008, 150}, {0x1000c, 97}};
    for (const uint32_t ordinal : {149u, 150u, 97u}) { browser::DeclareDiagnosticImport(image, runtime, ordinal, false); }
    browser::BindImageImports(image, runtime, {});
    check(image.boundImports[0].kind == browser::ImportKind::Function, "timer initialization did not bind a production target");
    for (size_t n = 1; n < image.boundImports.size(); ++n) {
      check(image.boundImports[n].kind == browser::ImportKind::FunctionTrap, "timer initialization enabled scheduling");
    }
    X86Context context{};
    context.host = &runtime;
    const auto invoke = [&](X86Context& ctx, uint8_t* base, uint32_t address, uint32_t type, uint32_t stack = 0x21f00) {
      const std::array<uint32_t, 3> frame = {0x12345, address, type};
      ctx.esp = stack;
      ctx.eax = 0xfeedabcd;
      ctx.ecx = 0x12345678;
      ctx.edx = 0x87654321;
      ctx.xflags = 0x246;
      image.memory->Write(ctx, stack, frame.data(), sizeof(frame));
      runtime.Invoke(ctx, base, runtime.KernelTarget(113));
      check(ctx.esp == stack + 12 && ctx.eax == 0xfeedabcd && ctx.ecx == 0x12345678 && ctx.edx == 0x87654321 &&
        ctx.xflags == 0x246, "timer void ABI changed registers or stack cleanup");
    };
    const auto reject = [&](X86Context& ctx, uint8_t* base, uint32_t address, uint32_t type) {
      std::array<uint8_t, 4096> before{}, after{};
      image.memory->Read({}, 0x20000, before.data(), before.size());
      bool refused = false;
      try { invoke(ctx, base, address, type); }
      catch (const browser::Fault&) { refused = true; }
      image.memory->Read({}, 0x20000, after.data(), after.size());
      check(refused && ctx.esp == 0x21f00 && ctx.eax == 0xfeedabcd && before == after,
        "invalid timer initialization changed storage or returned success");
    };
    reject(context, image.memory->Base(), 0x20100, 0);
    runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      for (const uint32_t address : {0x20100u, 0x20ff0u}) {
        for (const uint32_t type : {0u, 1u, 0u}) {
          std::array<uint8_t, 48> expected;
          expected.fill(0xa5);
          image.memory->Write(ctx, address - 4, expected.data(), expected.size());
          invoke(ctx, base, address, type);
          expected[4] = uint8_t(8 + type);
          expected[6] = 10;
          expected[7] = 0;
          const std::array<uint32_t, 7> fields = {0, address + 8, address + 8, 0, 0, 0, 0};
          std::memcpy(expected.data() + 8, fields.data(), sizeof(fields));
          const uint32_t period = 0;
          std::memcpy(expected.data() + 40, &period, sizeof(period));
          std::array<uint8_t, 48> actual;
          image.memory->Read(ctx, address - 4, actual.data(), actual.size());
          check(actual == expected, "timer initialization lost fields, preserved bytes or bounds");
        }
      }
      invoke(ctx, base, 0x21f00, 1);
      std::array<uint32_t, 10> aliased;
      image.memory->Read(ctx, 0x21f00, aliased.data(), sizeof(aliased));
      check(aliased[0] == 0x000a2309 && aliased[1] == 0 && aliased[2] == 0x21f08 && aliased[3] == 0x21f08 &&
        aliased[4] == 0 && aliased[5] == 0 && aliased[6] == 0 && aliased[7] == 0 && aliased[9] == 0,
        "timer output alias corrupted unsnapshotted arguments");
      for (const uint32_t address : {0u, 0x20101u, 0x21fe0u, 0x30000u, 0xfffffff0u}) {
        reject(ctx, base, address, 0);
      }
      for (const uint32_t type : {2u, 256u, 0xffffffffu}) { reject(ctx, base, 0x20100, type); }
      for (const uint32_t stack : {0x21ff8u, 0x30000u, 0xfffffff8u}) {
        ctx.esp = stack;
        bool refused = false;
        try { runtime.Invoke(ctx, base, runtime.KernelTarget(113)); }
        catch (const browser::Fault&) { refused = true; }
        check(refused && ctx.esp == stack, "invalid timer frame changed stack or returned success");
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
      for (const uint32_t ordinal : {149u, 150u, 97u}) {
        bool trapped = false;
        try { runtime.Invoke(ctx, base, runtime.KernelTarget(ordinal)); }
        catch (const browser::UnsupportedImport& fault) { trapped = fault.ordinal == ordinal; }
        check(trapped, "unsupported timer scheduling returned success");
      }
      runtime.Cancel();
      reject(ctx, base, 0x20100, 0);
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(!runtime.ActiveWorkers(), "timer session retained active workers");
  }
  std::puts("timer-provider-contracts-ok");
}
