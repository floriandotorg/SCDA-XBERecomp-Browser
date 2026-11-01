#include "browser/dpc-provider.h"
#include "cpu/x86_ops.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto callback = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  auto& memory = browser::Memory::From(base);
  check(!browser::Memory::HasBorrow(), "DPC ran under a backing lease");
  uint8_t irql = 0;
  memory.Read(ctx, ctx.fs_base + 0x24, &irql, 1);
  check(irql == 2, "DPC did not execute at dispatch level");
  std::array<uint32_t, 5> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  check(frame == std::array<uint32_t, 5>{0xfffffff0, 0x20100, 0xaabbccdd, 0x12345678, 0x87654321},
    "DPC invocation did not preserve its four snapshotted arguments");
  uint32_t mode = 0, calls = 0;
  memory.Read(ctx, 0x20000, &mode, 4);
  memory.Read(ctx, 0x20004, &calls, 4);
  ++calls;
  memory.Write(ctx, 0x20004, &calls, 4);
  ctx.eax = 0xfeedabcd;
  ctx.xflags = 0x246;
  ctx.xmm[0].q[0] = UINT64_MAX;
  if (mode == 1) { runtime.Kernel(ctx, base, 149); }
  if (mode == 2) { runtime.Cancel(); }
  if (mode == 3 || mode == 5) { runtime.Kernel(ctx, base, 390); }
  if (mode == 4) {
    memory.Decommit(ctx.fs_base, 4096);
    runtime.Kernel(ctx, base, 149);
  }
  if (mode == 6) { throw X86Resume{0x11000, ctx.esp}; }
  if (mode == 7) { throw uint32_t(7); }
  ctx.esp += 20;
};

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    for (uint32_t mode = 0; mode <= 7; ++mode) {
      const std::array<X86FuncMapping, 1> mappings = {{{0x11000, callback}}};
      browser::Runtime runtime(mappings, {});
      browser::PreparedImage image;
      image.memory = std::make_unique<browser::Memory>();
      image.memory->Ensure(0x10000, 4096);
      image.memory->Ensure(0x20000, 4096);
      browser::InstallDpcProvider(runtime, image);
      std::atomic<bool> callbackReady = false, releaseCallback = false;
      const auto waitFor = [&](const X86Context& ctx, const std::atomic<bool>& ready) {
        const auto watchdog = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready.load()) {
          check(std::chrono::steady_clock::now() < watchdog, "DPC concurrency watchdog expired");
          const auto sequence = runtime.Sequence();
          if (!ready.load()) { runtime.Wait(ctx, sequence, std::chrono::milliseconds(10)); }
        }
      };
      runtime.RegisterKernel(390, [&](X86Context& ctx, uint8_t*) {
        if (mode == 5) {
          callbackReady = true;
          runtime.Wake();
          waitFor(ctx, releaseCallback);
        } else {
          check(!browser::DeliverImageDpc(image, runtime, ctx, 0x20100), "recursive DPC dispatch entered its callback");
        }
      });
      image.image.kernel_thunk = 0x10000;
      const std::array<uint32_t, 3> imports = {0x8000006b, 0x80000095, 0};
      image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
      image.imports = {{0x10000, 107}, {0x10004, 149}};
      browser::DeclareDiagnosticImport(image, runtime, 149, false);
      browser::BindImageImports(image, runtime, {});
      const auto thread = browser::PrepareImageThread(image, runtime, 0x10, 0x10000);
      const auto other = browser::PrepareImageThread(image, runtime, 0x14, 0x10000);
      const std::array<uint32_t, 7> dpc = {19, 0xdead0000, 0xbeef0000, 0x11000, 0xaabbccdd, 0x12345678, 0x87654321};
      image.memory->Write({}, 0x20100, dpc.data(), sizeof(dpc));
      image.memory->Write({}, 0x20000, &mode, 4);
      const auto rejected = [&](const X86Context& ctx, uint32_t address, uint32_t expectedCalls = 0) {
        bool refused = false;
        try { browser::DeliverImageDpc(image, runtime, ctx, address); }
        catch (const browser::Fault&) { refused = true; }
        check(refused, "invalid DPC delivery returned success");
        uint32_t calls = 0;
        image.memory->Read({}, 0x20004, &calls, 4);
        check(calls == expectedCalls, "invalid delivery invoked the callback");
      };
      rejected(thread.context, 0x20100);
      runtime.Launch(thread.context, image.memory->Base(), [&](X86Context& ctx, uint8_t*) {
        for (const uint32_t address : {0u, 0x20101u, 0x30000u, 0xfffffff0u}) { rejected(ctx, address); }
        browser::Runtime foreign({}, {});
        auto alien = ctx;
        alien.host = &foreign;
        rejected(alien, 0x20100);
        alien = ctx;
        alien.fs_base += 4096;
        rejected(alien, 0x20100);
        rejected(other.context, 0x20100);
        for (const uint32_t stack : {thread.stackLimit + 16, thread.stackTop + 4, 0xffffffffu}) {
          alien = ctx;
          alien.esp = stack;
          rejected(alien, 0x20100);
        }
        {
          auto lease = image.memory->Borrow();
          rejected(ctx, 0x20100);
        }
        for (const uint32_t header : {0u, 0x113u}) {
          image.memory->Write(ctx, 0x20100, &header, 4);
          rejected(ctx, 0x20100);
        }
        image.memory->Write(ctx, 0x20100, dpc.data(), sizeof(dpc));
        const uint32_t unknown = 0x12345;
        image.memory->Write(ctx, 0x2010c, &unknown, 4);
        rejected(ctx, 0x20100);
        image.memory->Write(ctx, 0x20100, dpc.data(), sizeof(dpc));
        const uint8_t invalidIrql = 3;
        image.memory->Write(ctx, ctx.fs_base + 0x24, &invalidIrql, 1);
        rejected(ctx, 0x20100);
        const uint8_t originalIrql = 1;
        image.memory->Write(ctx, ctx.fs_base + 0x24, &originalIrql, 1);
        const auto before = ctx;
        if (mode == 1 || mode == 4 || mode == 6 || mode == 7) {
          browser::DeliverImageDpc(image, runtime, ctx, 0x20100);
          check(false, "DPC callback fault returned success");
        } else if (mode == 2) {
          bool cancelled = false;
          try { browser::DeliverImageDpc(image, runtime, ctx, 0x20100); }
          catch (const browser::Cancelled&) { cancelled = true; }
          check(cancelled, "cancelled DPC reported successful completion");
          rejected(ctx, 0x20100, 1);
        } else {
          check(browser::DeliverImageDpc(image, runtime, ctx, 0x20100), "valid DPC was not delivered");
        }
        check(std::memcmp(&ctx, &before, sizeof(ctx)) == 0, "DPC changed interrupted CPU context");
        uint8_t restored = 0;
        image.memory->Read({}, ctx.fs_base + 0x24, &restored, 1);
        check(restored == originalIrql, "DPC did not restore interrupted IRQL");
        std::array<uint32_t, 7> unchanged;
        image.memory->Read({}, 0x20100, unchanged.data(), sizeof(unchanged));
        check(unchanged == dpc, "delivery changed DPC storage");
      });
      if (mode == 5) {
        runtime.Launch(other.context, image.memory->Base(), [&](X86Context& ctx, uint8_t*) {
          waitFor(ctx, callbackReady);
          check(!browser::DeliverImageDpc(image, runtime, ctx, 0x20100), "concurrent delivery entered an active session callback");
          uint8_t irql = 0;
          image.memory->Read(ctx, ctx.fs_base + 0x24, &irql, 1);
          check(irql == 0, "deferred delivery changed the competing worker IRQL");
          releaseCallback = true;
          runtime.Wake();
        });
      }
      const auto failure = runtime.Join();
      if (mode == 1 || mode == 4 || mode == 6 || mode == 7) {
        check(bool(failure), "runtime lost callback failure");
        bool attributed = false;
        try { std::rethrow_exception(failure); }
        catch (const browser::UnsupportedImport& fault) {
          attributed = fault.ordinal == 149 && fault.context->fs_base == thread.kpcr &&
            fault.context->last_icall == 0x11000 && fault.context->esp == thread.context.esp - 20 &&
            fault.context->eax == 0xfeedabcd;
        }
        catch (const browser::Fault& fault) {
          attributed = (mode == 6 || mode == 7) && fault.address == 0x11000 && fault.context->fs_base == thread.kpcr &&
            fault.context->esp == thread.context.esp - 20;
        }
        check(attributed, "callback fault lost original type, ordinal or context");
        if (mode == 1) {
          uint8_t restored = 0;
          image.memory->Read({}, thread.kpcr + 0x24, &restored, 1);
          check(restored == 1, "faulted DPC did not restore IRQL");
        }
      } else if (failure) { std::rethrow_exception(failure); }
      check(runtime.ActiveWorkers() == 0, "DPC delivery left owned workers active");
      uint32_t calls = 0;
      image.memory->Read({}, 0x20004, &calls, 4);
      check(calls == 1, "delivery did not invoke exactly one callback");
    }
  }
  std::puts("dpc-delivery-contracts-ok");
}
