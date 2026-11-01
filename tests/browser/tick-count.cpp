#include "browser/timer-provider.h"
#include "cpu/x86_ops.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto refuse = [](const auto& operation) {
  bool refused = false;
  try { operation(); } catch (const browser::Fault&) { refused = true; }
  check(refused, "invalid tick count operation succeeded");
};
const auto waitFor = [](const auto& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    check(std::chrono::steady_clock::now() < deadline, "tick count synchronization timed out");
    std::this_thread::yield();
  }
};

struct Fixture {
  browser::Runtime runtime;
  browser::PreparedImage image;
  std::shared_ptr<browser::TimerQueue> queue;
  std::vector<browser::KernelDataExport> exports;
  explicit Fixture(std::function<uint64_t()> source = {}) : runtime({}, {}, std::move(source)) {
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 2> imports = {0x8000009c, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 156}};
    queue = browser::InstallTimerProvider(runtime, image);
  }
  void Install() {
    exports.push_back(browser::InstallTimerTickCount(runtime, image, queue));
  }
  void Bind() { browser::BindImageImports(image, runtime, exports); }
  uint32_t Read(X86Context context = {}) {
    uint32_t address = 0;
    image.memory->Read(context, 0x10000, &address, 4);
    return uint32_t(browser::Access<uint32_t>(context, image.memory->Base(), address));
  }
  void Join() {
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(runtime.ActiveWorkers() == 0, "tick count left a detached worker");
  }
};

const auto controlled = [] {
  for (uint32_t n = 0; n < 3; ++n) {
    std::atomic<uint64_t> source = 900000;
    Fixture fixture([&] { return source.load(); });
    fixture.Install();
    fixture.Bind();
    check(fixture.exports.size() == 1 && fixture.exports[0].ordinal == 156 && fixture.exports[0].size == 4,
      "tick count export is not the guest four byte ABI");
    check(fixture.image.boundImports[0].kind == browser::ImportKind::Data && fixture.Read() == 0,
      "fresh session tick count is not initialized data");
    const uint32_t sentinel = 0xaabbccdd;
    fixture.image.memory->Write({}, fixture.exports[0].address + 4, &sentinel, 4);
    const auto thread = browser::PrepareImageThread(fixture.image, fixture.runtime, 0x10, 0x10000);
    fixture.runtime.Launch(thread.context, fixture.image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      check(fixture.runtime.TimerTime(ctx) == 0 && fixture.Read(ctx) == 0, "fresh epoch inherited host or previous session uptime");
      source += 9999;
      fixture.runtime.Poll(ctx, base);
      check(fixture.Read(ctx) == 0, "tick count rounded submillisecond time up");
      source += 1;
      fixture.runtime.Poll(ctx, base);
      check(fixture.Read(ctx) == 1 && fixture.queue->Pending() == 0, "tick count did not advance without timer objects");
      fixture.runtime.ticks = UINT64_MAX;
      for (uint32_t n = 0; n < 200; ++n) { fixture.runtime.Poll(ctx, base); }
      check(fixture.Read(ctx) == 1, "paused monotonic source followed CPU progress");
      constexpr uint32_t timer = 0x10100;
      const std::array<uint32_t, 10> initialized = {0x000a0008, 0, timer + 8, timer + 8};
      fixture.image.memory->Write(ctx, timer, initialized.data(), sizeof(initialized));
      fixture.queue->Arm(ctx, timer, -10000, 0);
      const uint8_t elevated = 2;
      fixture.image.memory->Write(ctx, ctx.fs_base + 0x24, &elevated, 1);
      source += 50000;
      fixture.runtime.Poll(ctx, base);
      check(fixture.Read(ctx) == 6 && fixture.queue->Pending() == 1, "dispatch level froze tick count or consumed deferred expiry");
      const uint8_t passive = 0;
      fixture.image.memory->Write(ctx, ctx.fs_base + 0x24, &passive, 1);
      {
        std::lock_guard delivery(fixture.image.dpcDeliveryLock);
        source += 10000;
        fixture.runtime.Poll(ctx, base);
        check(fixture.Read(ctx) == 7 && fixture.queue->Pending() == 1, "contended DPC delivery froze tick count or consumed deferred expiry");
      }
      source = 900000 + uint64_t(UINT32_MAX) * 10000 + 9999;
      fixture.runtime.Poll(ctx, base);
      check(fixture.Read(ctx) == UINT32_MAX, "tick count saturated before 32 bit wrap");
      source += 1;
      fixture.runtime.Poll(ctx, base);
      check(fixture.Read(ctx) == 0, "tick count did not wrap naturally at 32 bits");
      source += 30000;
      fixture.runtime.Poll(ctx, base);
      check(fixture.Read(ctx) == 3, "tick count stopped after wrap");
      uint32_t neighbor = 0;
      fixture.image.memory->Read(ctx, fixture.exports[0].address + 4, &neighbor, 4);
      check(neighbor == sentinel, "tick count publication exceeded four bytes");
      auto forged = ctx;
      forged.fs_base += 4096;
      refuse([&] { fixture.queue->Drive(forged, base); });
      browser::Memory foreign;
      refuse([&] { fixture.queue->Drive(ctx, foreign.Base()); });
      {
        auto lease = foreign.Borrow();
        source += 10000;
        refuse([&] { fixture.queue->Drive(ctx, base); });
        fixture.runtime.Poll(ctx, base);
        check(fixture.Read(ctx) == 3, "foreign backing lease permitted publication");
      }
      fixture.runtime.Poll(ctx, base);
      check(fixture.Read(ctx) == 4, "tick count did not resume after lease release");
      fixture.runtime.Cancel();
      source += 10000;
      refuse([&] { fixture.queue->Drive(ctx, base); });
      check(fixture.Read(ctx) == 4, "cancelled runtime published tick count");
    });
    fixture.Join();
    auto stopped = thread.context;
    refuse([&] { fixture.queue->Drive(stopped, fixture.image.memory->Base()); });
  }
};

const auto production = [] {
  Fixture fixture;
  fixture.Install();
  fixture.Bind();
  const auto thread = browser::PrepareImageThread(fixture.image, fixture.runtime, 0x10, 0x10000);
  fixture.runtime.Launch(thread.context, fixture.image.memory->Base(), [&](X86Context& ctx, uint8_t* base) {
    const uint32_t before = fixture.Read(ctx);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    fixture.runtime.Poll(ctx, base);
    check(fixture.Read(ctx) >= before + 3, "owned guest poll did not publish production milliseconds");
    const uint32_t start = fixture.Read(ctx);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5);
    do { X86_BARRIER(); } while (std::chrono::steady_clock::now() < deadline);
    check(fixture.Read(ctx) >= start + 4 && fixture.queue->Pending() == 0,
      "guest busy loop without timer objects did not advance uptime");
  });
  fixture.Join();
};

const auto concurrent = [] {
  std::atomic<uint64_t> samples = 0;
  Fixture fixture([&] { return samples.fetch_add(1) * 10000; });
  fixture.Install();
  fixture.Bind();
  const auto first = browser::PrepareImageThread(fixture.image, fixture.runtime, 0x10, 0x10000);
  const auto second = browser::PrepareImageThread(fixture.image, fixture.runtime, 0x20, 0x10000);
  std::atomic<uint32_t> ready = 0;
  std::atomic<bool> poll = false;
  const auto worker = [&](X86Context& ctx, uint8_t* base) {
    ++ready;
    waitFor([&] { return poll.load(); });
    fixture.runtime.Poll(ctx, base);
    for (uint32_t n = 0; n < 499; ++n) {
      const uint32_t before = fixture.Read(ctx);
      fixture.runtime.Poll(ctx, base);
      check(fixture.Read(ctx) >= before, "two worker publication moved tick count backwards");
    }
  };
  fixture.runtime.Launch(first.context, fixture.image.memory->Base(), worker);
  fixture.runtime.Launch(second.context, fixture.image.memory->Base(), worker);
  waitFor([&] { return ready == 2; });
  {
    auto lease = fixture.image.memory->Borrow();
    poll = true;
    waitFor([&] { return samples >= 2; });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(samples == 2, "sampling escaped publication serialization while another worker was blocked on storage");
  }
  fixture.Join();
  check(samples == 1001 && fixture.Read() == 1000, "workers lost or reordered shared session samples");
};

const auto refusals = [] {
  {
    Fixture fixture;
    browser::Runtime foreign({}, {});
    browser::PreparedImage other;
    refuse([&] { browser::InstallTimerTickCount(foreign, fixture.image, fixture.queue); });
    refuse([&] { browser::InstallTimerTickCount(fixture.runtime, other, fixture.queue); });
    refuse([&] { browser::InstallTimerTickCount(fixture.runtime, fixture.image, {}); });
    {
      browser::Memory memory;
      auto lease = memory.Borrow();
      refuse([&] { fixture.Install(); });
    }
    fixture.Install();
    refuse([&] { fixture.Install(); });
    fixture.Bind();
    refuse([&] { fixture.Install(); });
    check(fixture.Read() == 0, "rejected installation changed initial uptime");
  }
  {
    Fixture fixture;
    fixture.image.memory->Map(0xc0002000, 4096);
    fixture.image.memory->Commit(0xc0002000, 4096);
    const uint32_t sentinel = 0xaabbccdd;
    fixture.image.memory->Write({}, 0xc0002000, &sentinel, 4);
    refuse([&] { fixture.Install(); });
    uint32_t stored = 0;
    fixture.image.memory->Read({}, 0xc0002000, &stored, 4);
    check(stored == sentinel && fixture.runtime.KernelTarget(149) == 0, "occupied storage was claimed or capability leaked");
  }
  {
    Fixture fixture;
    browser::InstallTimerExpiryDriver(fixture.runtime, fixture.image, fixture.queue);
    refuse([&] { fixture.Install(); });
    check(!fixture.image.memory->QueryPages(0xc0002000).mapped, "driver conflict leaked tick storage");
  }
  for (const uint32_t ordinal : {149u, 156u}) {
    Fixture fixture;
    fixture.runtime.RegisterKernel(ordinal, [](X86Context&, uint8_t*) {});
    refuse([&] { fixture.Install(); });
    check(!fixture.image.memory->QueryPages(0xc0002000).mapped, "function conflict leaked tick storage");
  }
  {
    Fixture fixture;
    browser::DeclareDiagnosticImport(fixture.image, fixture.runtime, 156, true);
    refuse([&] { fixture.Install(); });
    check(!fixture.image.memory->QueryPages(0xc0002000).mapped, "diagnostic conflict leaked tick storage");
  }
  {
    Fixture fixture;
    fixture.runtime.Cancel();
    refuse([&] { fixture.Install(); });
    check(!fixture.image.memory->QueryPages(0xc0002000).mapped, "cancelled installation leaked tick storage");
  }
  {
    Fixture fixture;
    browser::Memory published;
    fixture.runtime.PublishImageMemory(published, fixture.runtime.KernelRevision(), [] {});
    refuse([&] { fixture.Install(); });
    check(!fixture.image.memory->QueryPages(0xc0002000).mapped && fixture.runtime.KernelTarget(149) == 0,
      "foreign published runtime leaked tick storage or driver");
  }
  {
    Fixture fixture;
    X86Context context{};
    std::atomic<bool> ready = false;
    std::atomic<bool> finish = false;
    fixture.runtime.Launch(context, fixture.image.memory->Base(), [&](X86Context&, uint8_t*) {
      ready = true;
      waitFor([&] { return finish.load(); });
    });
    waitFor([&] { return ready.load(); });
    refuse([&] { fixture.Install(); });
    finish = true;
    fixture.Join();
    check(!fixture.image.memory->QueryPages(0xc0002000).mapped, "active installation leaked tick storage");
  }
  {
    Fixture fixture;
    fixture.image.memory.reset();
    refuse([&] { fixture.Install(); });
  }
  {
    Fixture fixture;
    fixture.Install();
    fixture.Bind();
    auto original = std::move(fixture.image.memory);
    fixture.image.memory = std::make_unique<browser::Memory>();
    X86Context context{};
    context.host = &fixture.runtime;
    fixture.runtime.Launch(context, original->Base(), [&](X86Context& ctx, uint8_t* base) {
      refuse([&] { fixture.queue->Drive(ctx, base); });
    });
    fixture.Join();
  }
};

int main() {
  controlled();
  production();
  concurrent();
  refusals();
  std::puts("tick-count-contracts-ok");
}
