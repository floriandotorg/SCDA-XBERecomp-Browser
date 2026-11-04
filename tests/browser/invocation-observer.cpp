#include "browser/cpu_runtime.h"
#include "cpu/x86_ops.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

struct PublicFailure {};
void PublicTarget(X86Context& ctx, uint8_t*) { ctx.eax += 7; }
void PublicThrow(X86Context&, uint8_t*) { throw PublicFailure{}; }

int main() {
  const X86FuncMapping mappings[] = {{0x1000, PublicTarget}, {0x2000, PublicThrow}, {0x3000, PublicTarget}};
  const uint32_t continuations[] = {0x3000};
  browser::Memory memory, other;
  browser::Runtime runtime(mappings, continuations, [] { return uint64_t(0); });
  uint32_t observations = 0;
  uint32_t sdkObservations = 0;
  runtime.InstallSdkObserver(memory, [&](X86Context&, uint32_t, uint64_t) { ++sdkObservations; return true; });
  bool throwObserver = false;
  const auto callback = [&](const X86Context& ctx, uint8_t* base, uint32_t target, uint64_t worker) {
    check(base == memory.Base() && worker && runtime.OwnsCurrentContext(ctx), "observer ownership changed");
    check(target == ctx.last_icall, "observer did not see resolved actual target");
    ++observations;
    if (throwObserver) { throw std::runtime_error("diagnostic-only failure"); }
    const_cast<X86Context&>(ctx).ebx = 0;
  };
  runtime.InstallInvocationObserver(memory, callback);
  const auto rejected = [](auto operation) {
    bool failed = false;
    try { operation(); } catch (const browser::Fault&) { failed = true; }
    check(failed, "invalid observer lifecycle operation accepted");
  };
  rejected([&] { runtime.InstallInvocationObserver(memory, callback); });
  rejected([&] { runtime.PublishImageMemory(other, runtime.KernelRevision(), [] {}); });
  runtime.RegisterKernel(5, [](X86Context& ctx, uint8_t*) { ctx.eax += 11; });
  runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
  rejected([&] { runtime.InstallInvocationObserver(memory, callback); });
  X86Context original{};
  original.host = &runtime;
  original.eax = 3;
  original.ebx = 0x12345678;
  runtime.Launch(original, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
    check(browser::Runtime::ObserveSdkEntry(ctx, base, 1) && sdkObservations == 1 && observations == 0,
      "invocation observer replaced SDK observation");
    runtime.Invoke(ctx, base, 0x1000);
    check(observations == 1 && ctx.eax == 10 && ctx.ebx == original.ebx, "observer changed actual dispatch registers");
    const uint32_t host = runtime.HostThunk(PublicTarget);
    runtime.Invoke(ctx, base, host);
    check(observations == 2 && ctx.eax == 17, "resolved host thunk was not observed");
    runtime.Invoke(ctx, base, runtime.KernelTarget(5));
    check(observations == 3 && ctx.eax == 28, "resolved kernel was not observed");
    const uint32_t before = observations;
    rejected([&] { runtime.Invoke(ctx, base, 0x4000); });
    rejected([&] { runtime.Invoke(ctx, base, 0xfe000018); });
    rejected([&] { runtime.Invoke(ctx, nullptr, 0x1000); });
    bool resumed = false;
    try { runtime.Invoke(ctx, base, 0x3000); } catch (const X86Resume&) { resumed = true; }
    check(resumed && observations == before, "unknown or continuation target emitted an invocation event");
    throwObserver = true;
    runtime.Invoke(ctx, base, 0x1000);
    check(ctx.eax == 35 && observations == before + 1, "observer exception changed target result");
    bool exact = false;
    try { runtime.Invoke(ctx, base, 0x2000); } catch (const PublicFailure&) { exact = true; }
    check(exact && observations == before + 2, "observer replaced original guest exception");
  });
  check(!runtime.Join(), "observer changed worker completion");
  const uint32_t before = observations;
  runtime.Cancel();
  auto retired = original;
  retired.host = &runtime;
  rejected([&] { runtime.Invoke(retired, memory.Base(), 0x1000); });
  check(observations == before && !runtime.ActiveWorkers(), "retirement emitted observations or retained a worker");
  browser::Runtime empty({}, {});
  rejected([&] { empty.InstallInvocationObserver(memory, {}); });
  empty.InstallSdkObserver(other, [](X86Context&, uint32_t, uint64_t) { return true; });
  rejected([&] { empty.InstallInvocationObserver(memory, callback); });
  browser::Runtime previouslyUsed({}, {}), borrowed({}, {});
  previouslyUsed.Launch({}, memory.Base(), [](X86Context&, uint8_t*) {});
  previouslyUsed.Join();
  rejected([&] { previouslyUsed.InstallInvocationObserver(memory, callback); });
  {
    auto lease = memory.Borrow();
    rejected([&] { borrowed.InstallInvocationObserver(memory, callback); });
  }
  borrowed.Cancel();
  rejected([&] { borrowed.InstallInvocationObserver(memory, callback); });
  browser::Memory filteredMemory;
  filteredMemory.Ensure(0x10000, 4096);
  browser::Runtime filtered(mappings, continuations, [] { return uint64_t(0); });
  uint32_t filteredObservations = 0;
  filtered.InstallInvocationObserver(filteredMemory, [&](const X86Context&, uint8_t*, uint32_t, uint64_t) { ++filteredObservations; }, {0x5555});
  filtered.PublishImageMemory(filteredMemory, filtered.KernelRevision(), [] {});
  X86Context call{};
  call.host = &filtered;
  call.esp = 0x10800;
  filtered.Launch(call, filteredMemory.Base(), [&](X86Context& ctx, uint8_t* base) {
    const auto returnTo = [&](uint32_t address) { filteredMemory.Write(ctx, ctx.esp, &address, 4); };
    returnTo(0x4444);
    filtered.Invoke(ctx, base, 0x1000);
    check(filteredObservations == 0 && ctx.eax == 7, "filtered observer saw an unrelated return address");
    returnTo(0x5555);
    filtered.Invoke(ctx, base, 0x1000);
    check(filteredObservations == 1 && ctx.eax == 14, "filtered observer missed its return address");
  });
  check(!filtered.Join(), "filtered observer changed worker completion");
  std::puts("invocation-observer-ok");
}
