#include "browser/delay-provider.h"
#include <array>
#include <bit>
#include <cstdio>
#include <string_view>

using browser::Memory;
using browser::Runtime;
using namespace std::chrono_literals;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto refuse = [](const auto& operation, std::string_view reason = {}) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault& error) {
    rejected = true;
    check(std::string_view(error.what()).starts_with(reason), "unexpected delay fault diagnostic");
  }
  check(rejected, "invalid delay operation succeeded");
};
const auto await = [](const auto& ready) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!ready() && std::chrono::steady_clock::now() < deadline) { std::this_thread::sleep_for(1ms); }
  check(ready(), "delay worker did not reach blocking boundary");
};
const auto put = [](Memory& memory, uint32_t address, uint64_t bits, size_t size) {
  std::array<uint8_t, 8> bytes{};
  for (size_t n = 0; n < size; ++n) { bytes[n] = uint8_t(bits >> (n * 8)); }
  memory.Write({}, address, bytes.data(), size);
};
constexpr uint32_t stack = 0x10003;
constexpr uint32_t intervalAddress = 0x11003;

struct Fixture {
  std::atomic<uint64_t> source = 900000;
  std::atomic<uint32_t> polls = 0;
  Runtime runtime;
  Memory memory;
  X86Context context{};
  explicit Fixture(bool realClock = false)
    : runtime({}, {}, realClock ? std::function<uint64_t()>{} : std::function<uint64_t()>{[this] { return source.load(); }}) {
    memory.Ensure(0x10000, 0x3000);
    context.host = &runtime;
    context.fs_base = 0x12000;
    context.esp = stack;
    context.eax = 0xdeadbeef;
    context.ebx = 0x12345678;
    context.ecx = 0x87654321;
    context.edx = 0x10203040;
    context.esi = 0x11223344;
    context.edi = 0x55667788;
    context.ebp = 0x99aabbcc;
    context.cf = 1;
    context.zf = 1;
    browser::InstallDelayProvider(runtime);
    runtime.InstallExpiryDriver([this](X86Context&, uint8_t*) { ++polls; });
  }
  void Frame(int64_t interval, uint32_t mode = 0, uint32_t alertable = 0, uint32_t pointer = intervalAddress) {
    put(memory, stack, 0xabcdef01, 4);
    put(memory, stack + 4, mode, 4);
    put(memory, stack + 8, alertable, 4);
    put(memory, stack + 12, pointer, 4);
    put(memory, intervalAddress, std::bit_cast<uint64_t>(interval), 8);
  }
  void Publish() { runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {}); }
  void Join() { if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); } }
  void Run(const auto& entry) { runtime.Launch(context, memory.Base(), entry); Join(); }
  void Invoke(X86Context& worker, uint8_t* base) { runtime.Kernel(worker, base, 99); }
  void Registers(const X86Context& actual, bool success) {
    check(actual.eax == (success ? 0 : context.eax) && actual.esp == context.esp + (success ? 16 : 0) &&
      actual.ebx == context.ebx && actual.ecx == context.ecx && actual.edx == context.edx &&
      actual.esi == context.esi && actual.edi == context.edi && actual.ebp == context.ebp &&
      actual.cf == context.cf && actual.zf == context.zf && actual.fs_base == context.fs_base &&
      actual.host == context.host, "delay changed unrelated registers or published failure results");
  }
};

const auto abiContracts = [] {
  Fixture fixture;
  check(fixture.runtime.KernelTarget(99) == 0xfe00018c && !fixture.runtime.KernelTarget(98) &&
    !fixture.runtime.KernelTarget(100), "delay provider registered wrong ordinal");
  const auto revision = fixture.runtime.KernelRevision();
  refuse([&] { browser::InstallDelayProvider(fixture.runtime); });
  check(fixture.runtime.KernelRevision() == revision, "duplicate delay install replaced state");
  fixture.Frame(0);
  refuse([&] { fixture.Invoke(fixture.context, fixture.memory.Base()); });
  fixture.Publish();
  fixture.Run([&](X86Context& worker, uint8_t* base) {
    for (const uint32_t mode : {0u, 1u}) {
      fixture.Frame(0, mode, 0xffffff00);
      auto request = worker;
      fixture.Invoke(request, base);
      fixture.Registers(request, true);
    }
    const auto reject = [&](std::string_view reason = {}) {
      auto request = worker;
      refuse([&] { fixture.Invoke(request, base); }, reason);
      fixture.Registers(request, false);
    };
    for (const uint32_t mode : {2u, UINT32_MAX}) { fixture.Frame(0, mode); reject("invalid delay processor mode"); }
    for (const uint32_t alertable : {1u, 0x101u, 0xffu}) {
      fixture.Frame(0, 0, alertable);
      reject("alertable execution delays are not implemented");
    }
    for (const int64_t interval : {int64_t(1), INT64_MAX}) {
      fixture.Frame(interval);
      reject("absolute execution delays require a system time epoch");
    }
    for (const uint32_t pointer : {0u, 0xffffu, 0x12ff9u, 0xfffffffcu}) {
      fixture.Frame(0, 0, 0, pointer);
      reject();
    }
    fixture.Frame(0);
    for (const uint32_t address : {0xffffu, 0x12ff1u, 0xfffffff0u, 0xfffffffcu}) {
      auto request = worker;
      request.esp = address;
      refuse([&] { fixture.Invoke(request, base); });
      check(request.esp == address && request.eax == worker.eax, "invalid frame published cleanup");
    }
    {
      auto lease = fixture.memory.Borrow();
      reject("execution delay requires its owned runtime worker without a backing lease");
    }
    auto foreign = worker;
    ++foreign.fs_base;
    refuse([&] { fixture.Invoke(foreign, base); });
    Memory other;
    refuse([&] { fixture.Invoke(worker, other.Base()); });
    refuse([&] { fixture.Invoke(worker, nullptr); });
    fixture.Frame(0);
    auto request = worker;
    request.esp = 0x12ff0;
    std::array<uint8_t, 16> frame{};
    fixture.memory.Read(worker, stack, frame.data(), frame.size());
    fixture.memory.Write(worker, request.esp, frame.data(), frame.size());
    fixture.Invoke(request, base);
    check(request.esp == 0x13000 && request.eax == 0, "exact frame extent required trailing backing");
  });
};

const auto virtualClockContracts = [] {
  for (const int64_t interval : {int64_t(-1), int64_t(-10001), INT64_MIN}) {
    Fixture fixture;
    fixture.Frame(interval);
    fixture.Publish();
    std::atomic<bool> completed = false;
    fixture.runtime.Launch(fixture.context, fixture.memory.Base(), [&](X86Context& worker, uint8_t* base) {
      fixture.Invoke(worker, base);
      fixture.Registers(worker, true);
      completed = true;
    });
    await([&] { return fixture.polls.load() != 0; });
    std::this_thread::sleep_for(25ms);
    check(!completed && fixture.polls < 200, "virtual delay busy-waited or completed without clock progress");
    fixture.runtime.Wake();
    std::this_thread::sleep_for(5ms);
    check(!completed, "unrelated runtime wake completed execution delay");
    fixture.memory.Decommit(0x10000, 0x2000);
    const uint64_t duration = uint64_t(-(interval + 1)) + 1;
    fixture.source += duration - 1;
    fixture.runtime.Wake();
    std::this_thread::sleep_for(5ms);
    check(!completed, "execution delay completed one 100 ns tick early");
    fixture.source += 1;
    fixture.runtime.Wake();
    fixture.Join();
    check(completed, "virtual deadline did not complete with original success status");
    const auto trace = fixture.runtime.SnapshotKernelTrace();
    check(trace.count == 2 && trace.events[0].phase == Runtime::KernelTracePhase::Before &&
      trace.events[1].phase == Runtime::KernelTracePhase::After && trace.events[1].eax == 0 &&
      trace.events[1].esp == stack + 16, "delay did not preserve kernel trace ABI completion");
  }
};

const auto cancellationContracts = [] {
  for (uint32_t n = 0; n < 3; ++n) {
    Fixture fixture;
    fixture.Frame(INT64_MIN);
    fixture.Publish();
    std::atomic<bool> cancelled = false;
    fixture.runtime.Launch(fixture.context, fixture.memory.Base(), [&](X86Context& worker, uint8_t* base) {
      try { fixture.Invoke(worker, base); }
      catch (const browser::Cancelled&) {
        fixture.Registers(worker, false);
        cancelled = true;
        throw;
      }
    });
    await([&] { return fixture.polls.load() != 0; });
    const auto start = std::chrono::steady_clock::now();
    check(!fixture.runtime.Stop() && cancelled && fixture.runtime.ActiveWorkers() == 0,
      "Stop did not cancel and join blocked delay worker");
    check(std::chrono::steady_clock::now() - start < 1s, "cancellation waited for guest deadline");
  }
};

const auto independentProgressContracts = [] {
  Fixture fixture(true);
  fixture.Frame(-800000);
  fixture.Publish();
  std::atomic<bool> complete = false;
  std::atomic<uint32_t> gpuCallbacks = 0;
  fixture.runtime.Launch(fixture.context, fixture.memory.Base(), [&](X86Context& worker, uint8_t* base) {
    const auto before = fixture.runtime.TimerTime(worker);
    fixture.Invoke(worker, base);
    check(fixture.runtime.TimerTime(worker) >= before + 800000, "real delay returned before owned clock deadline");
    fixture.Registers(worker, true);
    complete = true;
  });
  await([&] { return fixture.polls.load() != 0; });
  while (!complete) {
    ++gpuCallbacks;
    fixture.runtime.Wake();
    std::this_thread::sleep_for(2ms);
  }
  fixture.Join();
  check(gpuCallbacks >= 10 && fixture.polls >= 2, "blocked worker starved independent UI/GPU callback fixture or timer driver");
  check(fixture.runtime.ActiveWorkers() == 0, "completed delay leaked a worker");
};

int main() {
  try {
    abiContracts();
    virtualClockContracts();
    cancellationContracts();
    independentProgressContracts();
    std::puts("delay-provider-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
