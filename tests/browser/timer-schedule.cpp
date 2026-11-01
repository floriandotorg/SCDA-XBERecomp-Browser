#include "browser/timer-provider.h"
#include <array>
#include <atomic>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto callback = [](X86Context&, uint8_t*) { throw std::runtime_error("schedule invoked deferred callback"); };

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    std::atomic<uint64_t> source = 100;
    const std::array<X86FuncMapping, 1> mappings = {{{0x40000, callback}}};
    browser::Runtime runtime(mappings, {}, [&] { return source.load(); });
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.memory->Ensure(0x20000, 8192);
    image.memory->Ensure(0x30000, 4096);
    auto queue = browser::InstallTimerProvider(runtime, image);
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 3> imports = {0x80000071, 0x80000095, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 113}, {0x10004, 149}};
    browser::DeclareDiagnosticImport(image, runtime, 149, false);
    browser::BindImageImports(image, runtime, {});
    check(image.boundImports[1].kind == browser::ImportKind::FunctionTrap, "unfinished delivery enabled scheduling export");
    X86Context context{};
    context.host = &runtime;
    const auto refused = [&](const auto& operation) {
      bool faulted = false;
      try { operation(); } catch (const browser::Fault&) { faulted = true; }
      check(faulted, "invalid schedule operation returned success");
    };
    refused([&] { queue->SetTimer(context, image.memory->Base()); });
    runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      const auto initialize = [&](uint32_t address) {
        const std::array<uint32_t, 3> frame = {0x12345, address, 0};
        ctx.esp = 0x30f00;
        image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
        runtime.Invoke(ctx, base, runtime.KernelTarget(113));
      };
      const auto frame = [&](uint32_t timer, int64_t due, uint32_t dpc = 0x21000) {
        const uint64_t bits = uint64_t(due);
        const std::array<uint32_t, 5> values = {0x12345, timer, uint32_t(bits), uint32_t(bits >> 32), dpc};
        ctx.esp = 0x30f00;
        image.memory->Write(ctx, ctx.esp, values.data(), sizeof(values));
        ctx.eax = 0xabcdef00;
      };
      const auto snapshot = [&] {
        std::array<uint32_t, 10> bytes;
        image.memory->Read(ctx, 0x20000, bytes.data(), sizeof(bytes));
        return bytes;
      };
      initialize(0x20000);
      const std::array<uint32_t, 7> dpc = {19, 0, 0, 0x40000, 0xaabbccdd, 0x12345678, 0x87654321};
      image.memory->Write(ctx, 0x21000, dpc.data(), sizeof(dpc));
      frame(0x20000, -216000000000ll);
      const auto original = ctx;
      queue->SetTimer(ctx, base);
      check(ctx.eax == 0 && ctx.esp == original.esp + 20 && ctx.ecx == original.ecx && ctx.xflags == original.xflags,
        "schedule did not publish exact BOOLEAN and stdcall cleanup");
      const auto armed = snapshot();
      check(armed[0] == 0x000a0008 + 0x01000000 && armed[1] == 0 && armed[2] == 0x20008 && armed[3] == 0x20008 &&
        armed[4] == uint32_t(216000000000ull) && armed[5] == uint32_t(216000000000ull >> 32) && armed[8] == 0x21000 && armed[9] == 0,
        "schedule did not publish exact relative one shot timer fields");
      check(!queue->TakeExpired(ctx), "six hour timer fired early");
      refused([&] { initialize(0x20000); });
      for (const int64_t due : {int64_t(0), int64_t(1)}) {
        frame(0x20000, due);
        const auto before = ctx;
        refused([&] { queue->SetTimer(ctx, base); });
        check(ctx.esp == before.esp && ctx.eax == before.eax && snapshot() == armed && queue->Pending() == 1,
          "failed replacement changed ABI, timer or schedule");
      }
      frame(0x20000, -1);
      {
        auto lease = image.memory->Borrow();
        refused([&] { queue->SetTimer(ctx, base); });
      }
      browser::Runtime foreign({}, {});
      auto alien = ctx;
      alien.host = &foreign;
      refused([&] { queue->SetTimer(alien, base); });
      alien = ctx;
      alien.fs_base += 4096;
      refused([&] { queue->SetTimer(alien, base); });
      refused([&] { queue->SetTimer(ctx, nullptr); });
      for (const uint32_t address : {0u, 0x20001u, 0xfffffff0u, 0x22000u}) {
        frame(address, -1);
        refused([&] { queue->SetTimer(ctx, base); });
        check(snapshot() == armed && queue->Pending() == 1, "invalid timer changed armed storage");
      }
      frame(0x20000, -1, 0x22000);
      refused([&] { queue->SetTimer(ctx, base); });
      frame(0x20000, -5, 0);
      queue->SetTimer(ctx, base);
      check(ctx.eax == 1 && queue->Pending() == 1, "replacement lost previous insertion BOOLEAN");
      source = 104;
      check(!queue->TakeExpired(ctx), "replacement fired early");
      source = 105;
      const auto readyState = snapshot();
      const uint64_t corruptDeadline = 99;
      image.memory->Write(ctx, 0x20010, &corruptDeadline, sizeof(corruptDeadline));
      const auto corruptState = snapshot();
      refused([&] { queue->TakeExpired(ctx); });
      check(snapshot() == corruptState && queue->Pending() == 1, "corrupt expiry partially published signal state");
      image.memory->Write(ctx, 0x20000, readyState.data(), sizeof(readyState));
      const auto expired = queue->TakeExpired(ctx);
      check(expired && expired->timer == 0x20000 && expired->dpc == 0 && expired->deadline == 5, "replacement lost deadline");
      const auto signaled = snapshot();
      check((signaled[0] >> 24) == 0 && signaled[1] == 1 && signaled[8] == 0 && !queue->TakeExpired(ctx),
        "expiry did not atomically publish one shot signal state");
      frame(0x20000, -1);
      queue->SetTimer(ctx, base);
      check(ctx.eax == 0 && snapshot()[1] == 0, "rearm did not clear previous signal");
      refused([&] { queue->Arm(ctx, 0x20000, -1, 0); });
      const auto beforeInvalidFrame = snapshot();
      for (const uint32_t stack : {0xfffffff0u, 0x30ff0u, 0x22000u}) {
        ctx.esp = stack;
        refused([&] { queue->SetTimer(ctx, base); });
        check(snapshot() == beforeInvalidFrame, "invalid call frame changed timer state");
      }
      frame(0x20000, -1, 0);
      std::array<uint32_t, 5> aliased;
      image.memory->Read(ctx, ctx.esp, aliased.data(), sizeof(aliased));
      ctx.esp = 0x20010;
      image.memory->Write(ctx, ctx.esp, aliased.data(), sizeof(aliased));
      queue->SetTimer(ctx, base);
      check(ctx.eax == 1 && ctx.esp == 0x20024 && snapshot()[8] == 0, "aliased frame did not use snapshotted arguments");
      image.memory->Write(ctx, 0x20018, dpc.data(), sizeof(dpc));
      const auto overlapping = snapshot();
      frame(0x20000, -1, 0x20018);
      refused([&] { queue->SetTimer(ctx, base); });
      check(snapshot() == overlapping, "overlapping DPC changed timer state");
      frame(0x20000, -1, 0);
      queue->SetTimer(ctx, base);
      for (uint32_t n = 1; n < 64; ++n) {
        initialize(0x20000 + n * 64);
        frame(0x20000 + n * 64, -10, 0);
        queue->SetTimer(ctx, base);
      }
      initialize(0x21100);
      std::array<uint32_t, 10> extraBefore, extraAfter;
      image.memory->Read(ctx, 0x21100, extraBefore.data(), sizeof(extraBefore));
      frame(0x21100, -1, 0);
      refused([&] { queue->SetTimer(ctx, base); });
      image.memory->Read(ctx, 0x21100, extraAfter.data(), sizeof(extraAfter));
      check(extraBefore == extraAfter && queue->Pending() == 64, "capacity failure changed destination storage");
      frame(0x20000, -2, 0);
      queue->SetTimer(ctx, base);
      check(ctx.eax == 1 && queue->Pending() == 64, "full queue refused existing timer replacement");
      const auto full = snapshot();
      source = UINT64_MAX;
      frame(0x20000, -101, 0);
      refused([&] { queue->SetTimer(ctx, base); });
      check(snapshot() == full && queue->Pending() == 64, "deadline overflow changed existing schedule");
      runtime.Cancel();
      const auto cancelled = snapshot();
      frame(0x20000, -2);
      refused([&] { queue->SetTimer(ctx, base); });
      check(snapshot() == cancelled && queue->Pending() == 64, "cancellation changed schedule");
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(runtime.ActiveWorkers() == 0, "schedule retained guest worker");
  }
  std::puts("timer-schedule-contracts-ok");
}
