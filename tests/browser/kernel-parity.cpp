#include "browser/kernel-provider.h"
#include <array>
#include <bit>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime runtime({}, {});
    browser::Memory memory;
    memory.Map(0x10000, 0x2000);
    memory.Commit(0x10000, 0x2000);
    memory.Map(0x12000, 0x2000);
    memory.Commit(0x12000, 0x1000);
    browser::InstallKernelProvider(runtime, memory);
    const auto target = runtime.KernelTarget(346);
    check(target, "DES parity provider target absent");
    check(!runtime.KernelTarget(347), "unimplemented key table registered");
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
    using Snapshot = std::array<uint8_t, 0x3000>;
    using Frame = std::array<uint32_t, 3>;
    const auto snapshot = [&] {
      Snapshot bytes;
      memory.Read({}, 0x10000, bytes.data(), bytes.size());
      return bytes;
    };
    const auto setup = [&](uint32_t address, uint32_t length, uint32_t stack = 0x10000) {
      Snapshot bytes;
      bytes.fill(0xa5);
      memory.Write({}, 0x10000, bytes.data(), bytes.size());
      const Frame frame = {0xfffffff0, address, length};
      memory.Write({}, stack, frame.data(), sizeof(frame));
      auto ctx = original;
      ctx.esp = stack;
      return ctx;
    };
    const auto unchanged = [&](const X86Context& ctx) {
      check(ctx.eax == original.eax && ctx.ebx == original.ebx && ctx.ecx == original.ecx && ctx.edx == original.edx &&
        ctx.esi == original.esi && ctx.edi == original.edi && ctx.ebp == original.ebp &&
        ctx.fs_base == original.fs_base && ctx.host == original.host && ctx.cf && ctx.pf &&
        ctx.zf && ctx.sf && ctx.df && ctx.of, "DES parity clobbered unrelated state");
    };
    const auto invoke = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), target); };
    const auto reject = [&](X86Context ctx, auto operation) {
      const auto before = snapshot();
      const auto stack = ctx.esp;
      bool rejected = false;
      try { operation(ctx); } catch (const browser::Fault&) { rejected = true; }
      check(rejected, "invalid DES parity request accepted");
      check(snapshot() == before && ctx.esp == stack, "DES parity fault partially published");
      unchanged(ctx);
    };
    for (const uint32_t address : {0x11001u, 0x11f80u, 0x12f00u}) {
      auto ctx = setup(address, 256, 0x10ff4);
      std::array<uint8_t, 256> input;
      for (uint32_t n = 0; n < input.size(); ++n) { input[n] = uint8_t(n); }
      memory.Write({}, address, input.data(), input.size());
      auto expected = snapshot();
      for (uint32_t n = 0; n < input.size(); ++n) {
        expected[address - 0x10000 + n] = uint8_t(n ^ ((std::popcount(n) & 1) ^ 1));
      }
      invoke(ctx);
      check(ctx.esp == 0x11000 && snapshot() == expected, "DES parity exhaustive vector or exact bounds incorrect");
      unchanged(ctx);
      ctx = original;
      const Frame frame = {0, address, 256};
      memory.Write({}, ctx.esp, frame.data(), sizeof(frame));
      const auto before = snapshot();
      invoke(ctx);
      check(snapshot() == before, "DES parity not idempotent");
    }
    for (const uint32_t length : {1u, 7u, 8u, 24u, 257u, 4097u}) {
      auto ctx = setup(0x11001, length);
      auto expected = snapshot();
      for (uint32_t n = 0; n < length; ++n) { expected[0x1001 + n] = 0xa4; }
      invoke(ctx);
      check(snapshot() == expected && ctx.esp == original.esp + 12, "DES parity length or cleanup incorrect");
      unchanged(ctx);
    }
    for (const uint32_t address : {0u, 0x13000u, UINT32_MAX}) {
      auto ctx = setup(address, 0);
      const auto before = snapshot();
      invoke(ctx);
      check(snapshot() == before && ctx.esp == original.esp + 12, "zero length DES parity dereferenced buffer");
      unchanged(ctx);
    }
    for (const auto& [address, length] : std::array<std::array<uint32_t, 2>, 7>{{
      {0, 1}, {0x13000, 1}, {0x14000, 1}, {0x12fff, 2}, {0xfffffff0, 32}, {0x11000, UINT32_MAX}, {UINT32_MAX, 1}
    }}) { reject(setup(address, length), invoke); }
    for (const uint32_t stack : {0u, 0x12ff8u, 0x13000u, 0x14000u, 0xfffffff4u, 0xfffffffcu}) {
      auto ctx = setup(0x11000, 8);
      ctx.esp = stack;
      reject(ctx, invoke);
    }
    auto ctx = setup(0x10000, 12);
    const auto before = snapshot();
    auto expected = before;
    for (uint32_t n = 0; n < 12; ++n) {
      expected[n] ^= uint8_t((std::popcount(unsigned(expected[n])) & 1) ^ 1);
    }
    invoke(ctx);
    check(snapshot() == expected && ctx.esp == original.esp + 12, "DES parity did not snapshot aliased frame");
    browser::Runtime foreign({}, {});
    ctx = setup(0x11000, 8);
    ctx.host = &foreign;
    bool rejected = false;
    try { invoke(ctx); } catch (const browser::Fault&) { rejected = true; }
    check(rejected && ctx.eax == original.eax && ctx.esp == original.esp, "foreign DES parity runtime accepted");
    reject(setup(0x11000, 8), [&](X86Context& request) { runtime.Invoke(request, nullptr, target); });
    runtime.Stop();
    reject(setup(0x11000, 8), invoke);
    reject(setup(0, 0), invoke);
  }
  std::puts("kernel-parity-contracts-ok");
}
