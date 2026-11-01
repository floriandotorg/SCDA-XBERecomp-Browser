#include "browser/timer-provider.h"
#include "browser/dpc-provider.h"
#include "cpu/x86_ops.h"
#include <array>
#include <atomic>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
std::atomic<uint32_t> calls = 0;
std::atomic<uint32_t> mode = 0;
const auto callback = [](X86Context& ctx, uint8_t* base) {
  auto& runtime = browser::Runtime::From(ctx);
  auto& memory = browser::Memory::From(base);
  check(!browser::Memory::HasBorrow(), "timer callback ran under backing lease");
  uint8_t irql = 0;
  memory.Read(ctx, ctx.fs_base + 0x24, &irql, 1);
  check(irql == 2, "timer callback IRQL is not dispatch level");
  std::array<uint32_t, 5> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  check(frame[0] == 0xfffffff0 && frame[1] == 0x20100 && frame[2] == 0xaabbccdd,
    "timer callback lost deferred identity or context");
  const uint64_t time = (uint64_t(frame[4]) << 32) | frame[3];
  const uint64_t now = browser::Runtime::SystemTime();
  check(time <= now && now - time < 100000000, "timer DPC arguments are not current NT system time");
  std::array<uint32_t, 2> stored;
  memory.Read(ctx, 0x20114, stored.data(), sizeof(stored));
  check(stored[0] == frame[3] && stored[1] == frame[4], "timer DPC argument publication disagrees with callback");
  runtime.Poll(ctx, base);
  ++calls;
  if (mode == 1) { runtime.Kernel(ctx, base, 150); }
  if (mode == 2) { runtime.Cancel(); }
  if (mode == 3 && calls == 1) {
    const uint32_t savedStack = ctx.esp;
    const std::array<uint32_t, 5> frame = {0x12345, 0x20000, 0xffffffff, 0xffffffff, 0x20100};
    ctx.esp -= 20;
    memory.Write(ctx, ctx.esp, frame.data(), sizeof(frame));
    runtime.Kernel(ctx, base, 149);
    check(ctx.eax == 0 && ctx.esp == savedStack, "callback rearm lost insertion or stack");
  }
  ctx.esp += 20;
};

int main() {
  {
    browser::Runtime owner({}, {}), foreign({}, {});
    browser::PreparedImage ownedImage, foreignImage;
    const auto queue = browser::InstallTimerProvider(owner, ownedImage);
    bool refused = false;
    try { browser::InstallTimerExpiryDriver(foreign, foreignImage, queue); }
    catch (const browser::Fault&) { refused = true; }
    check(refused && foreign.KernelTarget(149) == 0, "foreign queue installed an expiry driver");
  }
  for (uint32_t n = 0; n < 3; ++n) {
    for (uint32_t selectedMode = 0; selectedMode < 4; ++selectedMode) {
      calls = 0;
      mode = selectedMode;
      std::atomic<uint64_t> source = 100;
      const std::array<X86FuncMapping, 1> mappings = {{{0x11000, callback}}};
      browser::Runtime runtime(mappings, {}, [&] { return source.load(); });
      browser::PreparedImage image;
      image.memory = std::make_unique<browser::Memory>();
      image.memory->Ensure(0x10000, 4096);
      image.memory->Ensure(0x20000, 4096);
      browser::InstallDpcProvider(runtime, image);
      const auto queue = browser::InstallTimerProvider(runtime, image);
      browser::InstallTimerExpiryDriver(runtime, image, queue);
      image.image.kernel_thunk = 0x10000;
      const std::array<uint32_t, 4> imports = {0x80000071, 0x80000095, 0x80000096, 0};
      image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
      image.imports = {{0x10000, 113}, {0x10004, 149}, {0x10008, 150}};
      browser::DeclareDiagnosticImport(image, runtime, 150, false);
      browser::BindImageImports(image, runtime, {});
      check(image.boundImports[1].kind == browser::ImportKind::Function, "integrated timer still traps");
      const auto thread = browser::PrepareImageThread(image, runtime, 0x10, 0x10000);
      runtime.Launch(thread.context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
        const uint32_t stack = ctx.esp;
        const std::array<uint32_t, 3> init = {0x12345, 0x20000, 0};
        ctx.esp = stack - 12;
        image.memory->Write(ctx, ctx.esp, init.data(), sizeof(init));
        runtime.Kernel(ctx, base, 113);
        const std::array<uint32_t, 7> dpc = {19, 0, 0, 0x11000, 0xaabbccdd, 0x12345678, 0x87654321};
        image.memory->Write(ctx, 0x20100, dpc.data(), sizeof(dpc));
        const auto arm = [&](int64_t due) {
          const uint64_t bits = uint64_t(due);
          const std::array<uint32_t, 5> frame = {0x12345, 0x20000, uint32_t(bits), uint32_t(bits >> 32), 0x20100};
          ctx.esp = stack - 20;
          image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
          runtime.Kernel(ctx, base, 149);
        };
        arm(-10);
        source = 105;
        runtime.Poll(ctx, base);
        check(calls == 0 && queue->Pending() == 1, "driver fired early");
        arm(-20);
        check(ctx.eax == 1, "replacement lost previous insertion");
        source = 110;
        runtime.Poll(ctx, base);
        check(calls == 0, "replaced timer fired at stale deadline");
        source = 125;
        const uint8_t elevated = 2;
        image.memory->Write(ctx, ctx.fs_base + 0x24, &elevated, 1);
        runtime.Poll(ctx, base);
        check(calls == 0 && queue->Pending() == 1, "driver delivered at dispatch level");
        const uint8_t passive = 0;
        image.memory->Write(ctx, ctx.fs_base + 0x24, &passive, 1);
        {
          auto lease = image.memory->Borrow();
          runtime.Poll(ctx, base);
          check(calls == 0 && queue->Pending() == 1, "driver delivered under backing lease");
        }
        const auto before = ctx;
        X86_BARRIER();
        check(std::memcmp(&ctx, &before, sizeof(ctx)) == 0, "timer callback changed interrupted registers");
        runtime.Poll(ctx, base);
        check(calls == 1 && queue->Pending() == size_t(selectedMode == 3), "driver did not consume expiry exactly once");
        if (selectedMode == 3) {
          source = 126;
          X86_BARRIER();
          check(calls == 2 && queue->Pending() == 0, "callback rearm was lost or deadlocked");
        }
        uint32_t signal = 0;
        image.memory->Read(ctx, 0x20004, &signal, 4);
        check(signal == 1, "driver failed to signal timer");
        arm(-216000000000ll);
        runtime.Cancel();
      });
      const auto failure = runtime.Join();
      if (selectedMode == 1) {
        check(bool(failure), "timer callback fault was lost");
        try { std::rethrow_exception(failure); }
        catch (const browser::UnsupportedImport& fault) {
          check(fault.ordinal == 150 && fault.context->fs_base == thread.kpcr && fault.context->last_icall == 0x11000,
            "timer callback fault lost attribution");
        }
      } else if (failure) { std::rethrow_exception(failure); }
      uint8_t restored = 9;
      image.memory->Read({}, thread.kpcr + 0x24, &restored, 1);
      check(restored == 0 && calls == (selectedMode == 3 ? 2 : 1) && runtime.ActiveWorkers() == 0, "timer teardown lost IRQL, callback count or workers");
      runtime.Stop();
    }
  }
  std::puts("timer-driver-contracts-ok");
}
