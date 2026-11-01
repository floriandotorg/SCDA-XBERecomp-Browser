#include "browser/kernel-provider.h"
#include <array>
#include <cstdio>

using browser::Memory;
using browser::Runtime;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto fault = [](auto operation) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, "invalid status translation accepted");
};

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    Runtime runtime({}, {});
    Memory memory;
    memory.Map(0x10000, 0x2000);
    memory.Commit(0x10000, 0x1000);
    browser::InstallKernelProvider(runtime, memory);
    const uint32_t target = runtime.KernelTarget(301);
    check(target, "status translation provider target absent");
    check(!runtime.KernelTarget(302), "unimplemented exception service registered");
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
    original.cf = 1;
    original.pf = 1;
    original.zf = 1;
    original.sf = 1;
    original.df = 1;
    original.of = 1;
    const auto translate = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), target); };
    const std::array<std::array<uint32_t, 2>, 30> cases = {{
      {0, 0}, {0x103, 997}, {0x102, 1460}, {0xc000000f, 2}, {0xc0000034, 2},
      {0xc000003a, 3}, {0xc0000022, 5}, {0xc0000121, 5}, {0xc00000ba, 5},
      {0xc0000008, 6}, {0xc0000017, 8}, {0x80000006, 18}, {0xc0000011, 38},
      {0xc00000bb, 50}, {0xc000000d, 87}, {0xc0000002, 120}, {0xc0000023, 122},
      {0xc0000033, 123}, {0xc0000035, 183}, {0xc0000103, 267}, {0xc000007f, 112},
      {0xc000009d, 1167}, {1, 317}, {0x101, 317}, {0x40000000, 317},
      {0x80000000, 317}, {0xc0000000, 317}, {UINT32_MAX, 317}, {0x12345678, 317}, {0xdeadbeef, 317},
    }};
    for (const auto& [status, expected] : cases) {
      for (const uint32_t stack : {0x10000u, 0x10ff8u}) {
        auto request = original;
        request.esp = stack;
        const std::array<uint32_t, 2> frame = {0xfffffff0, status};
        memory.Write({}, stack, frame.data(), sizeof(frame));
        std::array<uint8_t, 0x1000> before, after;
        memory.Read({}, 0x10000, before.data(), before.size());
        translate(request);
        memory.Read({}, 0x10000, after.data(), after.size());
        check(request.eax == expected && request.esp == stack + 8, "incorrect status translation or stdcall cleanup");
        check(request.ebx == original.ebx && request.ecx == original.ecx && request.edx == original.edx &&
          request.esi == original.esi && request.edi == original.edi && request.ebp == original.ebp &&
          request.fs_base == original.fs_base && request.host == original.host && request.cf && request.pf &&
          request.zf && request.sf && request.df && request.of, "status translation clobbered unrelated state");
        check(before == after, "status translation changed guest memory");
      }
    }
    for (const uint32_t stack : {0u, 0x10ffcu, 0x11000u, 0x12000u, 0xfffffff8u, 0xfffffffcu}) {
      auto request = original;
      request.esp = stack;
      fault([&] { translate(request); });
      check(request.eax == original.eax && request.esp == stack, "invalid stack published status result");
    }
    Runtime foreign({}, {});
    auto request = original;
    request.host = &foreign;
    fault([&] { translate(request); });
    check(request.eax == original.eax && request.esp == original.esp, "foreign runtime published status result");
    request = original;
    fault([&] { runtime.Invoke(request, nullptr, target); });
    check(request.eax == original.eax && request.esp == original.esp, "foreign memory published status result");
    runtime.Stop();
    request = original;
    fault([&] { translate(request); });
    check(request.eax == original.eax && request.esp == original.esp, "retired runtime published status result");
  }
  std::puts("kernel-status-contracts-ok");
}
