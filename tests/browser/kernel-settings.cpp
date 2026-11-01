#include "browser/kernel-provider.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

const auto virtualDisplayContracts = [] {
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime runtime({}, {});
    browser::Runtime foreign({}, {});
    browser::Memory memory;
    browser::Memory foreignMemory;
    memory.Map(0x10000, 0x3000);
    memory.Commit(0x10000, 0x2000);
    browser::InstallKernelProvider(runtime, memory);
    const uint32_t target = runtime.KernelTarget(2);
    check(target == 0xfe000008, "virtual display provider target absent");
    for (const uint32_t ordinal : {1u, 3u, 4u}) {
      check(!runtime.KernelTarget(ordinal), "unobserved AV service registered");
    }
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    bool rebound = false;
    try { runtime.RegisterKernel(2, [](X86Context&, uint8_t*) {}); }
    catch (const browser::Fault&) { rebound = true; }
    check(rebound && runtime.KernelTarget(2) == target, "bound virtual display target mutable");
    X86Context original{};
    original.host = &runtime;
    original.esp = 0x10000;
    original.eax = 0xabcdef01;
    original.ebx = 0x12345678;
    original.ecx = 0x87654321;
    original.edx = 0x11223344;
    original.esi = 0x55667788;
    original.edi = 0x99aabbcc;
    original.ebp = 0xddeeff00;
    original.fs_base = 0xfffffff0;
    original.cf = original.pf = original.zf = original.sf = original.df = original.of = 1;
    using Frame = std::array<uint32_t, 5>;
    using Snapshot = std::array<uint8_t, 0x2000>;
    const auto snapshot = [&] {
      Snapshot bytes;
      memory.Read({}, 0x10000, bytes.data(), bytes.size());
      return bytes;
    };
    const auto setup = [&](const Frame& frame, uint32_t stack = 0x10000) {
      Snapshot bytes;
      bytes.fill(0xa5);
      memory.Write({}, 0x10000, bytes.data(), bytes.size());
      memory.Write({}, stack, frame.data(), sizeof(frame));
      auto ctx = original;
      ctx.esp = stack;
      return ctx;
    };
    const auto unchanged = [&](const X86Context& ctx, const X86Context& before) {
      check(ctx.eax == before.eax && ctx.ebx == before.ebx && ctx.ecx == before.ecx &&
        ctx.edx == before.edx && ctx.esi == before.esi && ctx.edi == before.edi &&
        ctx.ebp == before.ebp && ctx.fs_base == before.fs_base && ctx.host == before.host &&
        ctx.cf == before.cf && ctx.pf == before.pf && ctx.zf == before.zf &&
        ctx.sf == before.sf && ctx.df == before.df && ctx.of == before.of,
        "void virtual display query clobbered registers or flags");
    };
    const auto invoke = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), target); };
    const auto reject = [&](X86Context ctx, auto operation) {
      const auto bytes = snapshot();
      const auto before = ctx;
      bool rejected = false;
      try { operation(ctx); } catch (const browser::Fault&) { rejected = true; }
      check(rejected, "unsupported virtual display request accepted");
      check(snapshot() == bytes && ctx.esp == before.esp, "rejected virtual display query partially published");
      unchanged(ctx, before);
    };
    reject(setup({0x4a2a84, 0, 6, 0, 0x11000}), invoke);
    runtime.Launch(original, memory.Base(), [&](X86Context&, uint8_t*) {
      for (const uint32_t stack : {0x10000u, 0x11fecu}) {
        for (const uint32_t output : {0x11000u, stack == 0x10000 ? 0x10014u : 0x11fe8u}) {
          auto ctx = setup({0x4a2a84, 0, 6, 0, output}, stack);
          auto expected = snapshot();
          constexpr uint32_t profile = 0x00480104;
          for (uint32_t n = 0; n < 4; ++n) { expected[output - 0x10000 + n] = uint8_t(profile >> (n * 8)); }
          invoke(ctx);
          check(ctx.esp == stack + 20 && snapshot() == expected, "virtual display profile, exact output span or stdcall cleanup incorrect");
          unchanged(ctx, original);
        }
      }
      auto ctx = setup({0x4a2a84, 0, 6, 0, 0x11ffc});
      invoke(ctx);
      check(ctx.eax == original.eax && ctx.esp == 0x10014, "virtual display exact boundary output rejected");
      std::array<uint32_t, 6> settingsFrame{0, 8, 0, 0x11000, 4, 0};
      memory.Write({}, 0x10000, settingsFrame.data(), sizeof(settingsFrame));
      ctx = original;
      runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(24));
      uint32_t flags;
      memory.Read({}, 0x11000, &flags, sizeof(flags));
      check(flags == 0x80000 && (0x00480104 & 0x00bf0000) == flags,
        "virtual display profile diverged from immutable 4:3/480p settings");
      for (const uint32_t output : {0u, 0x11001u, 0x11ffeu, 0x12000u, 0x13000u, 0xfffffffcu, UINT32_MAX}) {
        reject(setup({0x4a2a84, 0, 6, 0, output}), invoke);
      }
      for (uint32_t n = 0; n < 5; ++n) {
        reject(setup({0x4a2a84, 0, 6, 0, 0x10000 + 4 * n}), invoke);
      }
      for (const uint32_t stack : {0u, 0x11ff0u, 0x12000u, 0x13000u, 0xffffffecu, 0xfffffffcu}) {
        ctx = original;
        ctx.esp = stack;
        reject(ctx, invoke);
      }
      for (uint32_t n = 0; n < 32; ++n) {
        if (n != 6) { reject(setup({0x4a2a84, 0, n, 0, 0x11000}), invoke); }
      }
      reject(setup({0x4a2a84, 0, UINT32_MAX, 0, 0x11000}), invoke);
      for (const auto& frame : std::array<Frame, 4>{{
        {0x4a2a84, 1, 6, 0, 0x11000}, {0x4a2a84, 0xfd000000, 6, 0, 0x11000},
        {0x4a2a84, 0, 6, 1, 0x11000}, {0x4a2a84, 0, 6, UINT32_MAX, 0x11000},
      }}) {
        reject(setup(frame), invoke);
      }
      ctx = setup({0x4a2a84, 0, 6, 0, 0x11000});
      ctx.host = &foreign;
      reject(ctx, invoke);
      ctx = setup({0x4a2a84, 0, 6, 0, 0x11000});
      ++ctx.fs_base;
      reject(ctx, invoke);
      for (auto* base : {static_cast<uint8_t*>(nullptr), foreignMemory.Base()}) {
        reject(setup({0x4a2a84, 0, 6, 0, 0x11000}), [&](X86Context& request) { runtime.Invoke(request, base, target); });
      }
      ctx = setup({0x4a2a84, 0, 6, 0, 0x11000});
      {
        auto lease = memory.Borrow();
        reject(ctx, invoke);
      }
      runtime.Cancel();
      reject(ctx, invoke);
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
};

int main() {
  virtualDisplayContracts();
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime runtime({}, {});
    browser::Memory memory;
    memory.Map(0x10000, 0x3000);
    memory.Commit(0x10000, 0x2000);
    browser::InstallKernelProvider(runtime, memory);
    const uint32_t target = runtime.KernelTarget(24);
    check(target, "settings provider target absent");
    check(!runtime.KernelTarget(29), "settings write service registered");
    X86Context original{};
    original.host = &runtime;
    original.esp = 0x10000;
    original.eax = 0xabcdef01;
    original.ebx = 0x12345678;
    original.ecx = 0x87654321;
    original.edx = 0x11223344;
    original.esi = 0x55667788;
    original.edi = 0x99aabbcc;
    original.ebp = 0xddeeff00;
    original.fs_base = 0xfffffff0;
    original.cf = original.pf = original.zf = original.sf = original.df = original.of = 1;
    const auto invoke = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), target); };
    using Frame = std::array<uint32_t, 6>;
    using Snapshot = std::array<uint8_t, 0x2000>;
    const auto snapshot = [&] {
      Snapshot bytes;
      memory.Read({}, 0x10000, bytes.data(), bytes.size());
      return bytes;
    };
    const auto word = [&](uint32_t address) {
      uint32_t value;
      memory.Read({}, address, &value, 4);
      return value;
    };
    const auto setup = [&](const Frame& frame, uint32_t stack = 0x10000) {
      Snapshot bytes;
      bytes.fill(0xa5);
      memory.Write({}, 0x10000, bytes.data(), bytes.size());
      memory.Write({}, stack, frame.data(), sizeof(frame));
      auto ctx = original;
      ctx.esp = stack;
      return ctx;
    };
    const auto unchanged = [&](const X86Context& ctx) {
      check(ctx.ebx == original.ebx && ctx.ecx == original.ecx && ctx.edx == original.edx &&
        ctx.esi == original.esi && ctx.edi == original.edi && ctx.ebp == original.ebp &&
        ctx.fs_base == original.fs_base && ctx.host == original.host && ctx.cf && ctx.pf &&
        ctx.zf && ctx.sf && ctx.df && ctx.of, "settings clobbered unrelated state");
    };
    const auto reject = [&](X86Context ctx, auto operation) {
      const auto before = snapshot();
      const auto stack = ctx.esp;
      bool rejected = false;
      try { operation(ctx); } catch (const browser::Fault&) { rejected = true; }
      check(rejected, "invalid settings request accepted");
      check(snapshot() == before && ctx.eax == original.eax && ctx.esp == stack, "failed settings request partially published");
      unchanged(ctx);
    };
    for (const auto& [index, expected] : std::array<std::array<uint32_t, 2>, 3>{{{7, 1}, {8, 0x80000}, {9, 0}}}) {
      for (const uint32_t stack : {0x10000u, 0x11fe8u}) {
        for (const uint32_t length : {4u, 5u, 64u, UINT32_MAX}) {
          auto ctx = setup({0xfffffff0, index, 0x11000, 0x11004, length, 0x11008}, stack);
          auto expectedBytes = snapshot();
          const auto put = [&](uint32_t offset, uint32_t value) {
            for (uint32_t n = 0; n < 4; ++n) { expectedBytes[offset + n] = uint8_t(value >> (n * 8)); }
          };
          put(0x1000, 4);
          put(0x1004, expected);
          put(0x1008, 4);
          invoke(ctx);
          check(ctx.eax == 0 && ctx.esp == stack + 24, "settings result or cleanup incorrect");
          check(snapshot() == expectedBytes, "settings wrote beyond exact output words");
          unchanged(ctx);
        }
      }
      auto ctx = setup({0, index, 0, 0x11001, 4, 0});
      invoke(ctx);
      check(ctx.eax == 0 && word(0x11001) == expected, "optional or unaligned settings outputs rejected");
      for (uint32_t length = 0; length < 4; ++length) {
        ctx = setup({0, index, 0x11000, 0, length, 0x11008});
        invoke(ctx);
        check(ctx.eax == 0xc0000023 && word(0x11000) == 4 && word(0x11008) == 4 &&
          word(0x11004) == 0xa5a5a5a5, "small settings buffer not reported truthfully");
      }
    }
    for (const uint32_t index : {0u, 0x12u, 0x103u, 0x104u, 0xffffu, UINT32_MAX}) {
      auto ctx = setup({0, index, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX});
      const auto before = snapshot();
      invoke(ctx);
      check(ctx.eax == 0xc00000bb && ctx.esp == original.esp + 24 && snapshot() == before,
        "unsupported query fabricated EEPROM data or dereferenced outputs");
    }
    for (const auto& frame : std::array<Frame, 8>{{
      {0, 7, 0x12000, 0x11004, 4, 0x11008}, {0, 7, 0x11000, 0x12000, 4, 0x11008},
      {0, 7, 0x11000, 0x11004, 4, 0x12000}, {0, 7, 0x11000, 0, 4, 0x11008},
      {0, 7, UINT32_MAX, 0x11004, 4, 0x11008}, {0, 7, 0x11000, UINT32_MAX, 4, 0x11008},
      {0, 7, 0x11000, 0x11004, 4, UINT32_MAX}, {0, 7, 0x11000, 0x11004, 0, 0x12000},
    }}) {
      reject(setup(frame), invoke);
    }
    for (const uint32_t stack : {0u, 0x11fecu, 0x12000u, 0x13000u, 0xffffffe8u, 0xfffffffcu}) {
      auto ctx = original;
      ctx.esp = stack;
      reject(ctx, invoke);
    }
    auto ctx = setup({0, 7, 0x11000, 0x10004, 4, 0x11008});
    invoke(ctx);
    check(word(0x10004) == 1 && word(0x11000) == 4 && word(0x11008) == 4,
      "settings did not snapshot overlapping input frame");
    ctx = setup({0, 7, 0x11000, 0x11000, 4, 0x11000});
    invoke(ctx);
    check(word(0x11000) == 4, "aliased outputs did not publish value, type then length");
    browser::Runtime foreign({}, {});
    ctx = setup({0, 7, 0x11000, 0x11004, 4, 0x11008});
    ctx.host = &foreign;
    bool rejected = false;
    try { invoke(ctx); } catch (const browser::Fault&) { rejected = true; }
    check(rejected && ctx.eax == original.eax && ctx.esp == original.esp, "foreign settings runtime accepted");
    reject(setup({0, 7, 0x11000, 0x11004, 4, 0x11008}), [&](X86Context& request) {
      runtime.Invoke(request, nullptr, target);
    });
    runtime.Stop();
    reject(setup({0, 7, 0x11000, 0x11004, 4, 0x11008}), invoke);
  }
  std::puts("kernel-settings-contracts-ok");
}
