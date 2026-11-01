#include "browser/kernel-provider.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include "vectors.h"

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
    const auto target = runtime.KernelTarget(340);
    check(target, "HMAC provider target absent");
    check(!runtime.KernelTarget(349), "unsupported CBC export registered");
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
    using Frame = std::array<uint32_t, 8>;
    const auto snapshot = [&] {
      Snapshot bytes;
      memory.Read({}, 0x10000, bytes.data(), bytes.size());
      return bytes;
    };
    const auto setup = [&](Frame frame, uint32_t stack = 0x10000) {
      Snapshot bytes;
      bytes.fill(0xa5);
      memory.Write({}, 0x10000, bytes.data(), bytes.size());
      memory.Write({}, stack, frame.data(), sizeof(frame));
      auto ctx = original;
      ctx.esp = stack;
      return ctx;
    };
    const auto unchanged = [&](const X86Context& ctx) {
      check(ctx.eax == original.eax && ctx.ebx == original.ebx && ctx.ecx == original.ecx && ctx.edx == original.edx &&
        ctx.esi == original.esi && ctx.edi == original.edi && ctx.ebp == original.ebp &&
        ctx.fs_base == original.fs_base && ctx.host == original.host && ctx.cf && ctx.pf &&
        ctx.zf && ctx.sf && ctx.df && ctx.of, "HMAC clobbered unrelated state");
    };
    const auto invoke = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), target); };
    const auto reject = [&](X86Context ctx, auto operation) {
      const auto before = snapshot();
      const auto stack = ctx.esp;
      bool rejected = false;
      try { operation(ctx); } catch (const browser::Fault&) { rejected = true; }
      check(rejected, "invalid HMAC request accepted");
      check(snapshot() == before && ctx.esp == stack, "HMAC fault partially published");
      unchanged(ctx);
    };
    for (const auto& vector : vectors) {
      for (const uint32_t split : {0u, vector.inputBytes / 2, vector.inputBytes}) {
        for (const uint32_t output : {0x10000u, 0x10801u, 0x11001u, 0x11ff7u, 0x12fecu}) {
          auto ctx = setup({0xfffffff0, 0x10801, vector.keyBytes, 0x11001, split,
            0x11001 + split, vector.inputBytes - split, output}, 0x10000);
          std::array<uint8_t, 128> key;
          for (uint32_t n = 0; n < key.size(); ++n) { key[n] = uint8_t(n * 13); }
          memory.Write({}, 0x10801, key.data(), vector.keyBytes);
          std::array<uint8_t, 4097> input;
          for (uint32_t n = 0; n < input.size(); ++n) { input[n] = uint8_t(n * 17); }
          memory.Write({}, 0x11001, input.data(), vector.inputBytes);
          auto expected = snapshot();
          std::copy(vector.digest.begin(), vector.digest.end(), expected.begin() + output - 0x10000);
          invoke(ctx);
          check(snapshot() == expected && ctx.esp == 0x10020, "HMAC vector, alias, bounds or cleanup incorrect");
          unchanged(ctx);
        }
      }
    }
    const std::array<uint8_t, 20> rfcDigest = {0xb6, 0x17, 0x31, 0x86, 0x55, 0x05, 0x72, 0x64, 0xe2, 0x8b,
      0xc0, 0xb6, 0xfb, 0x37, 0x8c, 0x8e, 0xf1, 0x46, 0xbe, 0x00};
    auto ctx = setup({0, 0x10800, 20, 0x11000, 3, 0x11003, 5, 0x12000}, 0x10fe0);
    std::array<uint8_t, 20> rfcKey;
    rfcKey.fill(0x0b);
    memory.Write({}, 0x10800, rfcKey.data(), rfcKey.size());
    memory.Write({}, 0x11000, "Hi There", 8);
    invoke(ctx);
    std::array<uint8_t, 20> digest;
    memory.Read({}, 0x12000, digest.data(), digest.size());
    check(digest == rfcDigest && ctx.esp == 0x11000, "RFC 2202 HMAC vector failed");
    unchanged(ctx);
    for (const uint32_t pointer : {0u, 0x13000u, UINT32_MAX}) {
      ctx = setup({0, pointer, 0, pointer, 0, pointer, 0, 0x12000});
      invoke(ctx);
      memory.Read({}, 0x12000, digest.data(), digest.size());
      check(digest == vectors[0].digest, "empty HMAC dereferenced input");
    }
    for (const uint32_t keyLength : {64u, 65u, UINT32_MAX}) {
      ctx = setup({0, 0x12fc0, keyLength, 0, 0, 0, 0, 0x12000});
      invoke(ctx);
      std::array<uint8_t, 20> current;
      memory.Read({}, 0x12000, current.data(), current.size());
      if (keyLength == 64) { digest = current; }
      check(current == digest, "Xbox HMAC hashed or dereferenced truncated key tail");
    }
    const Frame valid = {0, 0x10800, 64, 0x11000, 32, 0x12000, 32, 0x12500};
    for (const uint32_t argument : {1u, 3u, 5u, 7u}) {
      for (const uint32_t address : {0u, 0x12fffu, 0x13000u, 0x14000u, 0xfffffff0u, UINT32_MAX}) {
        auto frame = valid;
        frame[argument] = address;
        reject(setup(frame), invoke);
      }
    }
    for (const uint32_t argument : {4u, 6u}) {
      auto frame = valid;
      frame[argument] = UINT32_MAX;
      reject(setup(frame), invoke);
    }
    for (const uint32_t stack : {0u, 0x12fe4u, 0x13000u, 0x14000u, 0xffffffe0u, 0xfffffffcu}) {
      ctx = setup(valid);
      ctx.esp = stack;
      reject(ctx, invoke);
    }
    browser::Runtime foreign({}, {});
    ctx = setup(valid);
    ctx.host = &foreign;
    bool rejected = false;
    try { invoke(ctx); } catch (const browser::Fault&) { rejected = true; }
    check(rejected && ctx.esp == original.esp && ctx.eax == original.eax, "foreign HMAC runtime accepted");
    reject(setup(valid), [&](X86Context& request) { runtime.Invoke(request, nullptr, target); });
    runtime.Stop();
    reject(setup(valid), invoke);
    reject(setup({0, 0, 0, 0, 0, 0, 0, 0x12000}), invoke);
  }
  std::puts("kernel-hmac-contracts-ok");
}
