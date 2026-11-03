#include "browser/kernel-provider.h"
#include <array>
#include <cstdio>

const auto check = [](bool condition, const char* message) {
  if (!condition) { throw std::runtime_error(message); }
};

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Memory memory;
    browser::Runtime runtime({}, {}), foreign({}, {});
    memory.Map(0x10000, 0x2000);
    memory.Commit(0x10000, 0x1000);
    browser::InstallKernelProvider(runtime, memory);
    check(runtime.KernelTarget(37) == 0xfe000094, "named FSC non-caching policy absent");
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    X86Context original{};
    original.host = &runtime;
    original.esp = 0x10000;
    original.eax = 0x12345678;
    original.ebx = 0x31415926;
    original.ecx = 0x27182818;
    original.edx = 0x11223344;
    original.esi = 0x55667788;
    original.edi = 0x99aabbcc;
    original.ebp = 0xabcdef01;
    original.cf = original.pf = original.zf = original.sf = original.df = original.of = 1;
    const auto reject = [&](X86Context& ctx, auto operation) {
      const auto before = ctx;
      std::array<uint8_t, 0x1000> bytes{}, after{};
      memory.Read({}, 0x10000, bytes.data(), bytes.size());
      bool failed = false;
      try { operation(); } catch (const browser::Fault&) { failed = true; }
      memory.Read({}, 0x10000, after.data(), after.size());
      check(failed && after == bytes && ctx.eax == before.eax && ctx.esp == before.esp, "invalid FSC request partially published");
    };
    auto cold = original;
    reject(cold, [&] { runtime.Invoke(cold, memory.Base(), runtime.KernelTarget(37)); });
    const size_t backing = memory.BackingBytes();
    runtime.Launch(original, memory.Base(), [&](X86Context& owned, uint8_t*) {
      for (uint32_t count = 0; count < 3; ++count) {
        for (const uint32_t hint : {0u, 1u, 16u, 0x10000u, 0x80000000u, UINT32_MAX}) {
          const std::array<uint32_t, 2> frame = {0x12340000, hint};
          memory.Write(owned, owned.esp, frame.data(), sizeof(frame));
          std::array<uint8_t, 0x1000> before{}, after{};
          memory.Read({}, 0x10000, before.data(), before.size());
          auto ctx = owned;
          runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(37));
          memory.Read({}, 0x10000, after.data(), after.size());
          check(ctx.eax == 0 && ctx.esp == owned.esp + 8, "FSC advisory status or one-argument stdcall ABI differs from native");
          check(before == after && memory.BackingBytes() == backing, "non-caching policy wrote guest data or allocated a cache");
          check(ctx.ebx == owned.ebx && ctx.ecx == owned.ecx && ctx.edx == owned.edx && ctx.esi == owned.esi &&
            ctx.edi == owned.edi && ctx.ebp == owned.ebp && ctx.cf == owned.cf && ctx.pf == owned.pf &&
            ctx.zf == owned.zf && ctx.sf == owned.sf && ctx.df == owned.df && ctx.of == owned.of,
            "FSC policy changed non-result registers or flags");
        }
      }
      for (const uint32_t stack : {0u, 0x10ffcu, 0x11000u, 0x12000u, 0xfffffffcu, UINT32_MAX}) {
        auto invalid = owned;
        invalid.esp = stack;
        reject(invalid, [&] { runtime.Invoke(invalid, memory.Base(), runtime.KernelTarget(37)); });
      }
      auto borrowed = owned;
      {
        auto lease = memory.Borrow();
        reject(borrowed, [&] { runtime.Invoke(borrowed, memory.Base(), runtime.KernelTarget(37)); });
      }
      auto other = owned;
      other.host = &foreign;
      reject(other, [&] { runtime.Invoke(other, memory.Base(), runtime.KernelTarget(37)); });
      auto wrongBase = owned;
      reject(wrongBase, [&] { runtime.Invoke(wrongBase, nullptr, runtime.KernelTarget(37)); });
    });
    runtime.Join();
    runtime.Cancel();
    auto retired = original;
    reject(retired, [&] { runtime.Invoke(retired, memory.Base(), runtime.KernelTarget(37)); });
    check(!runtime.ActiveWorkers(), "FSC policy retained a worker");
  }
  std::puts("kernel-cache-policy-ok");
}
