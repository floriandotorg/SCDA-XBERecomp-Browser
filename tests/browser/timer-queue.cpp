#include "browser/timer-provider.h"
#include <array>
#include <atomic>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto callback = [](X86Context&, uint8_t*) { throw std::runtime_error("expiry queue invoked a guest callback"); };

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    std::atomic<uint64_t> source = 100;
    const std::array<X86FuncMapping, 1> mappings = {{{0x40000, callback}}};
    browser::Runtime runtime(mappings, {}, [&] { return source.load(); });
    browser::PreparedImage image;
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.memory->Ensure(0x20000, 8192);
    image.memory->Ensure(0x23000, 4096);
    image.memory->Ensure(0x30000, 4096);
    auto queue = browser::InstallTimerProvider(runtime, image);
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 3> imports = {0x80000071, 0x80000095, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 113}, {0x10004, 149}};
    browser::DeclareDiagnosticImport(image, runtime, 149, false);
    browser::BindImageImports(image, runtime, {});
    check(image.boundImports[1].kind == browser::ImportKind::FunctionTrap, "partial queue enabled production scheduling");
    X86Context context{};
    context.host = &runtime;
    std::array<std::atomic<uint32_t>, 64> consumed{};
    std::atomic<bool> descendantDone = false;
    const auto refused = [&](const auto& operation) {
      bool faulted = false;
      try { operation(); } catch (const browser::Fault&) { faulted = true; }
      check(faulted, "invalid queue operation returned success");
    };
    refused([&] { queue->Arm(context, 0x20000, -1, 0); });
    refused([&] { queue->TakeExpired(context); });
    runtime.Launch(context, image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      const auto initialize = [&](uint32_t address, uint32_t type = 0) {
        const std::array<uint32_t, 3> frame = {0x12345, address, type};
        ctx.esp = 0x30f00;
        image.memory->Write(ctx, ctx.esp, frame.data(), sizeof(frame));
        runtime.Invoke(ctx, base, runtime.KernelTarget(113));
      };
      for (uint32_t n = 0; n < 65; ++n) { initialize(0x20000 + n * 64, n % 2); }
      std::array<uint32_t, 7> dpc = {19, 0, 0, 0x40000, 0x12345678, 0xaabbccdd, 0xeeff0011};
      image.memory->Write(ctx, 0x23000, dpc.data(), sizeof(dpc));
      std::array<uint8_t, 8192> before{}, after{};
      image.memory->Read(ctx, 0x20000, before.data(), before.size());
      check(!queue->Arm(ctx, 0x20000, -216000000000ll, 0x23000), "new six hour schedule was previously inserted");
      check(!queue->TakeExpired(ctx), "six hour timer expired early");
      check(queue->Arm(ctx, 0x20000, -5, 0x23000), "replacement lost previous insertion");
      check(queue->Pending() == 1, "replacement consumed extra queue capacity");
      refused([&] { initialize(0x20000); });
      for (const int64_t due : {int64_t(0), int64_t(1)}) { refused([&] { queue->Arm(ctx, 0x20000, due, 0); }); }
      for (const uint32_t address : {0u, 0x20001u, 0x24000u, 0xfffffff0u}) {
        refused([&] { queue->Arm(ctx, address, -1, 0); });
      }
      for (const uint32_t address : {0x23001u, 0x24000u, 0xfffffff0u}) {
        refused([&] { queue->Arm(ctx, 0x20000, -1, address); });
      }
      for (const uint32_t header : {0u, 20u, 0x113u}) {
        dpc[0] = header;
        image.memory->Write(ctx, 0x23000, dpc.data(), sizeof(dpc));
        refused([&] { queue->Arm(ctx, 0x20000, -1, 0x23000); });
      }
      dpc[0] = 19;
      refused([&] { queue->Arm(ctx, 0x23000, -1, 0); });
      dpc[3] = 0xdeadbeef;
      image.memory->Write(ctx, 0x23000, dpc.data(), sizeof(dpc));
      refused([&] { queue->Arm(ctx, 0x20000, -1, 0x23000); });
      dpc[3] = 0x40000;
      image.memory->Write(ctx, 0x23000, dpc.data(), sizeof(dpc));
      browser::Runtime foreign({}, {});
      auto alien = ctx;
      alien.host = &foreign;
      refused([&] { queue->Arm(alien, 0x20000, -1, 0); });
      refused([&] { queue->TakeExpired(alien); });
      {
        auto lease = image.memory->Borrow();
        refused([&] { queue->Arm(ctx, 0x20000, -1, 0); });
        refused([&] { queue->TakeExpired(ctx); });
      }
      source = 104;
      check(!queue->TakeExpired(ctx), "timer expired before exact deadline");
      source = 105;
      const auto expired = queue->TakeExpired(ctx);
      check(expired && expired->timer == 0x20000 && expired->dpc == 0x23000 && expired->deadline == 5,
        "replacement or failed replacement lost original expiry identity");
      check(!queue->TakeExpired(ctx) && queue->Pending() == 0, "one shot timer expired twice");
      initialize(0x20000);
      for (uint32_t n = 0; n < 64; ++n) { check(!queue->Arm(ctx, 0x20000 + n * 64, -10, 0), "new timer was already inserted"); }
      refused([&] { queue->Arm(ctx, 0x21000, -1, 0); });
      check(queue->Pending() == 64 && queue->Arm(ctx, 0x20000, -20, 0), "full queue refused atomic replacement");
      source = 115;
      for (uint32_t n = 1; n < 64; ++n) {
        const auto ready = queue->TakeExpired(ctx);
        check(ready && ready->timer == 0x20000 + n * 64 && ready->deadline == 15 && ready->dpc == 0,
          "deadline ties did not preserve insertion order");
      }
      check(!queue->TakeExpired(ctx), "later replacement expired with earlier schedule");
      source = 125;
      const auto later = queue->TakeExpired(ctx);
      check(later && later->timer == 0x20000, "later replacement was lost");
      image.memory->Read(ctx, 0x20000, after.data(), after.size());
      check(before == after, "internal queue published partial guest timer state");
      for (uint32_t n = 0; n < 64; ++n) { queue->Arm(ctx, 0x20000 + n * 64, -1, 0); }
      source = 126;
      const auto consume = [&](const X86Context& consumer) {
        while (const auto ready = queue->TakeExpired(consumer)) {
          check(ready->timer >= 0x20000 && ready->timer < 0x21000 && (ready->timer - 0x20000) % 64 == 0,
            "concurrent expiry returned an unknown timer");
          ++consumed[(ready->timer - 0x20000) / 64];
        }
      };
      runtime.Launch(ctx, base, [&, consume](X86Context& child, uint8_t*) {
        consume(child);
        descendantDone = true;
        runtime.Wake();
      });
      consume(ctx);
      const auto watchdog = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      while (!descendantDone.load()) {
        check(std::chrono::steady_clock::now() < watchdog, "concurrent expiry watchdog expired");
        const auto sequence = runtime.Sequence();
        if (!descendantDone.load()) { runtime.Wait(ctx, sequence, std::chrono::milliseconds(10)); }
      }
      for (const auto& count : consumed) { check(count.load() == 1, "concurrent expiry lost or duplicated a timer"); }
      check(queue->Pending() == 0, "concurrent expiry retained an armed timer");
      check(!queue->Arm(ctx, 0x20000, INT64_MIN, 0), "full negative deadline was not accepted");
      source = UINT64_MAX;
      refused([&] { queue->Arm(ctx, 0x20000, -101, 0); });
      check(queue->Pending() == 1, "overflow changed existing schedule");
      runtime.Cancel();
      refused([&] { queue->TakeExpired(ctx); });
      refused([&] { queue->Arm(ctx, 0x20000, -1, 0); });
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(runtime.ActiveWorkers() == 0, "queue retained guest workers");
  }
  std::puts("timer-queue-contracts-ok");
}
