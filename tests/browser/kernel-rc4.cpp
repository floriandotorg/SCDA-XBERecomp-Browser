#include "browser/kernel-provider.h"
#include "kernel/crypto.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>

const auto check = [](bool value, const char* reason) {
  if (!value) { std::fprintf(stderr, "%s\n", reason); throw std::runtime_error(reason); }
};

int main() {
  using State = std::array<uint8_t, 258>;
  for (uint32_t session = 0; session < 3; ++session) {
    browser::Runtime runtime({}, {}), foreign({}, {});
    browser::Memory memory, foreignMemory;
    memory.Map(0x10000, 0x4000);
    memory.Commit(0x10000, 0x3000);
    browser::InstallKernelProvider(runtime, memory);
    const auto target = runtime.KernelTarget(338);
    const auto cryptTarget = runtime.KernelTarget(339);
    check(target && cryptTarget, "RC4 target registration incorrect");
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
    original.xflags = 0x200000;
    original.xmm[7].q[1] = UINT64_MAX;
    original.st[4] = 123.5;
    original.mm[3] = 0xabcdef0123456789;
    const auto snapshot = [&] {
      std::vector<uint8_t> bytes(0x3000);
      memory.Read({}, 0x10000, bytes.data(), bytes.size());
      return bytes;
    };
    const auto setup = [&](uint32_t output, uint32_t length, uint32_t key, uint32_t stack = 0x10000) {
      std::vector<uint8_t> bytes(0x3000, 0xa5);
      memory.Write({}, 0x10000, bytes.data(), bytes.size());
      const std::array<uint32_t, 4> frame = {0xfffffff0, output, length, key};
      memory.Write({}, stack, frame.data(), sizeof(frame));
      auto ctx = original;
      ctx.esp = stack;
      return ctx;
    };
    const auto unchanged = [&](const X86Context& ctx, const X86Context& before, uint32_t stack) {
      auto expected = before;
      expected.esp = stack;
      expected.last_icall = ctx.last_icall;
      check(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0, "RC4 clobbered CPU state");
    };
    const auto invoke = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), target); };
    const auto reject = [&](X86Context ctx, auto operation) {
      const auto bytes = snapshot();
      const auto before = ctx;
      bool rejected = false;
      try { operation(ctx); } catch (const browser::Fault&) { rejected = true; }
      check(rejected, "invalid RC4 request accepted");
      check(snapshot() == bytes, "RC4 fault partially published");
      unchanged(ctx, before, before.esp);
    };
    reject(setup(0x11000, 5, 0x10800), invoke);
    runtime.Launch(original, memory.Base(), [&](X86Context&, uint8_t*) {
      for (const uint32_t length : {1u, 5u, 16u, 20u, 256u}) {
        for (const uint32_t keyAddress : {0x10800u, 0x11000u, 0x11001u, 0x110ffu}) {
          auto request = setup(0x11000, length, keyAddress);
          std::array<uint8_t, 256> key{};
          for (uint32_t n = 0; n < length; ++n) { key[n] = uint8_t(n + 1); }
          memory.Write({}, keyAddress, key.data(), length);
          auto expected = snapshot();
          State reference;
          for (uint32_t n = 0; n < 256; ++n) { reference[n] = uint8_t(n); }
          uint8_t j = 0;
          for (uint32_t n = 0; n < 256; ++n) {
            j = uint8_t(j + reference[n] + key[n % length]);
            std::swap(reference[n], reference[j]);
          }
          reference[256] = reference[257] = 0;
          std::copy(reference.begin(), reference.end(), expected.begin() + 0x1000);
          invoke(request);
          check(snapshot() == expected, "RC4 state, alias or exact span incorrect");
          unchanged(request, original, 0x10010);
          State state;
          memory.Read({}, 0x11000, state.data(), state.size());
          auto sorted = state;
          std::sort(sorted.begin(), sorted.begin() + 256);
          for (uint32_t n = 0; n < 256; ++n) { check(sorted[n] == n, "RC4 state is not a permutation"); }
          check(state[256] == 0 && state[257] == 0, "RC4 indices incorrect");
          if (length == 5) {
            const std::array<uint8_t, 16> kat = {0xb2, 0x39, 0x63, 0x05, 0xf0, 0x3d, 0xc0, 0x27, 0xcc, 0xc3, 0x52, 0x4a, 0x0a, 0x11, 0x18, 0xa8};
            std::array<uint8_t, 16> stream{};
            crypto::Rc4Crypt(state.data(), stream.data(), stream.size());
            check(stream == kat, "RFC 6229 RC4 known answer incorrect");
            memory.Copy({}, 0x12000, 0x11000, 258);
            State copy;
            memory.Read({}, 0x12000, copy.data(), copy.size());
            stream.fill(0);
            crypto::Rc4Crypt(copy.data(), stream.data(), stream.size());
            check(stream == kat, "copied RC4 state incorrect");
          }
        }
      }
      {
        auto request = setup(0x11000, 5, 0x10800);
        const std::array<uint8_t, 5> key = {1, 2, 3, 4, 5};
        memory.Write({}, 0x10800, key.data(), key.size());
        invoke(request);
        memory.Copy({}, 0x12000, 0x11000, 258);
        const std::array<uint8_t, 32> kat = {0xb2, 0x39, 0x63, 0x05, 0xf0, 0x3d, 0xc0, 0x27, 0xcc, 0xc3, 0x52, 0x4a, 0x0a, 0x11, 0x18, 0xa8,
          0x69, 0x82, 0x94, 0x4f, 0x18, 0xfc, 0x82, 0xd5, 0x89, 0xc4, 0x03, 0xa4, 0x7a, 0x0d, 0x09, 0x19};
        std::array<uint8_t, 32> zero{};
        memory.Write({}, 0x10800, zero.data(), zero.size());
        for (uint32_t n = 0; n < 2; ++n) {
          const std::array<uint32_t, 4> frame = {0, 0x11000, 16, 0x10800 + n * 16};
          memory.Write({}, 0x10000, frame.data(), sizeof(frame));
          request = original;
          runtime.Invoke(request, memory.Base(), cryptTarget);
          unchanged(request, original, 0x10010);
        }
        std::array<uint8_t, 32> result;
        memory.Read({}, 0x10800, result.data(), result.size());
        check(result == kat, "RC4 crypt continuation known answer incorrect");
        memory.Write({}, 0x10800, zero.data(), zero.size());
        const std::array<uint32_t, 4> forkFrame = {0, 0x12000, 32, 0x10800};
        memory.Write({}, 0x10000, forkFrame.data(), sizeof(forkFrame));
        request = original;
        runtime.Invoke(request, memory.Base(), cryptTarget);
        memory.Read({}, 0x10800, result.data(), result.size());
        check(result == kat, "RC4 copied state known answer incorrect");
        State first, second;
        memory.Read({}, 0x11000, first.data(), first.size());
        memory.Read({}, 0x12000, second.data(), second.size());
        check(first == second, "RC4 copied state continuation differs");
        request = setup(0x11000, 16, 0x10800);
        std::array<uint8_t, 16> key16;
        for (uint32_t n = 0; n < key16.size(); ++n) { key16[n] = uint8_t(n + 1); }
        memory.Write({}, 0x10800, key16.data(), key16.size());
        invoke(request);
        std::array<uint8_t, 16> zero16{};
        memory.Write({}, 0x10800, zero16.data(), zero16.size());
        const std::array<uint32_t, 4> vectorFrame = {0, 0x11000, 16, 0x10800};
        memory.Write({}, 0x10000, vectorFrame.data(), sizeof(vectorFrame));
        request = original;
        runtime.Invoke(request, memory.Base(), cryptTarget);
        std::array<uint8_t, 16> vectorResult;
        memory.Read({}, 0x10800, vectorResult.data(), vectorResult.size());
        const std::array<uint8_t, 16> kat16 = {0x9a, 0xc7, 0xcc, 0x9a, 0x60, 0x9d, 0x1e, 0xf7, 0xb2, 0x93, 0x28, 0x99, 0xcd, 0xe4, 0x1b, 0x97};
        check(vectorResult == kat16, "RC4 128 bit RFC known answer incorrect");
        const auto crypt = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), cryptTarget); };
        for (const auto& args : std::array<std::array<uint32_t, 3>, 9>{{
            {0, 0, 0}, {0x12eff, 0, 0}, {0x11000, 1, 0}, {0x11000, 2, 0x12fff},
            {0x11000, 1, 0x11000}, {0x11000, 1, 0x11101}, {0x10000, 0, 0},
            {0x11000, 1, 0x10000}, {0x11000, (1u << 20) + 1, 0x10800}}}) {
          reject(setup(args[0], args[1], args[2]), crypt);
        }
        request = setup(0x11000, 1, 0x10800);
        request.host = &foreign;
        reject(request, crypt);
        request = setup(0x11000, 1, 0x10800);
        request.fs_base ^= 4;
        reject(request, crypt);
        {
          auto lease = memory.Borrow();
          reject(setup(0x11000, 1, 0x10800), crypt);
        }
        request = setup(0x11000, 1, 0x10800);
        std::thread other([&] { reject(request, crypt); });
        other.join();
        constexpr uint32_t maximum = 1u << 20;
        memory.Map(0x20000, maximum + 0x1000);
        memory.Commit(0x20000, maximum + 0x1000);
        request = setup(0x11000, 5, 0x10800);
        memory.Write({}, 0x10800, key.data(), key.size());
        invoke(request);
        State maximumState;
        memory.Read({}, 0x11000, maximumState.data(), maximumState.size());
        std::vector<uint8_t> expectedMaximum(maximum, 0x5a);
        crypto::Rc4Crypt(maximumState.data(), expectedMaximum.data(), expectedMaximum.size());
        std::vector<uint8_t> maximumInput(maximum + 2, 0x5a);
        memory.Write({}, 0x20000, maximumInput.data(), maximumInput.size());
        const std::array<uint32_t, 4> maximumFrame = {0, 0x11000, maximum, 0x20001};
        memory.Write({}, 0x10000, maximumFrame.data(), sizeof(maximumFrame));
        request = original;
        crypt(request);
        unchanged(request, original, 0x10010);
        memory.Read({}, 0x20000, maximumInput.data(), maximumInput.size());
        check(maximumInput.front() == 0x5a && maximumInput.back() == 0x5a &&
          std::equal(expectedMaximum.begin(), expectedMaximum.end(), maximumInput.begin() + 1), "RC4 maximum input or sentinels incorrect");
        State maximumResult;
        memory.Read({}, 0x11000, maximumResult.data(), maximumResult.size());
        check(maximumResult == maximumState, "RC4 maximum input state incorrect");
        request = setup(0x11000, 0, 0);
        crypt(request);
        unchanged(request, original, 0x10010);
      }
      for (const uint32_t output : {0u, 0x12effu, 0x13000u, 0xffffff00u, 0x10000u, 0x1000fu}) {
        reject(setup(output, 5, 0x10800), invoke);
      }
      for (const auto& input : std::array<std::array<uint32_t, 2>, 6>{{{0, 5}, {0x12fff, 2}, {0x13000, 1}, {0x10800, 0}, {0x10800, 257}, {0xfffffff0, 32}}}) {
        reject(setup(0x11000, input[1], input[0]), invoke);
      }
      for (const uint32_t stack : {0u, 0x12ff1u, 0x13000u, 0xfffffff0u}) {
        auto request = setup(0x11000, 5, 0x10800);
        request.esp = stack;
        reject(request, invoke);
      }
      auto request = setup(0x11000, 5, 0x10800);
      request.host = &foreign;
      reject(request, invoke);
      request = setup(0x11000, 5, 0x10800);
      request.fs_base ^= 4;
      reject(request, invoke);
      reject(setup(0x11000, 5, 0x10800), [&](X86Context& ctx) { runtime.Invoke(ctx, foreignMemory.Base(), target); });
      {
        auto lease = memory.Borrow();
        reject(setup(0x11000, 5, 0x10800), invoke);
      }
      request = setup(0x11000, 5, 0x10800);
      std::thread other([&] { reject(request, invoke); });
      other.join();
      request = setup(0x11000, 5, 0x10800);
      runtime.Cancel();
      reject(request, invoke);
    });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
  for (uint32_t cancellation = 0; cancellation < 2; ++cancellation) {
    constexpr uint32_t length = 1u << 20;
    browser::Runtime runtime({}, {});
    browser::Memory memory;
    memory.Map(0x10000, 0x2000);
    memory.Commit(0x10000, 0x2000);
    memory.Map(0x20000, length);
    memory.Commit(0x20000, length);
    browser::InstallKernelProvider(runtime, memory);
    const std::array<uint8_t, 5> key = {1, 2, 3, 4, 5};
    State initial;
    crypto::Rc4Key(initial.data(), key.data(), key.size());
    State expectedState = initial;
    std::vector<uint8_t> input(length, 0x5a);
    auto expectedData = input;
    crypto::Rc4Crypt(expectedState.data(), expectedData.data(), expectedData.size());
    memory.Write({}, 0x11000, initial.data(), initial.size());
    memory.Write({}, 0x20000, input.data(), input.size());
    const std::array<uint32_t, 4> frame = {0, 0x11000, length, 0x20000};
    memory.Write({}, 0x10000, frame.data(), sizeof(frame));
    X86Context ctx{};
    ctx.host = &runtime;
    ctx.esp = 0x10000;
    std::atomic<bool> entered = false, finished = false, torn = false, cancelled = false;
    std::atomic<uint32_t> observations = 0;
    std::thread observer([&] {
      std::vector<uint8_t> data(length);
      State state;
      while (!finished.load()) {
        auto lease = memory.Borrow();
        memory.Read({}, 0x11000, state.data(), state.size());
        memory.Read({}, 0x20000, data.data(), data.size());
        if (!((state == initial && data == input) || (state == expectedState && data == expectedData))) {
          torn.store(true);
        }
        ++observations;
      }
    });
    while (!observations.load()) { std::this_thread::yield(); }
    runtime.Launch(ctx, memory.Base(), [&](X86Context& request, uint8_t* base) {
      entered.store(true);
      try { runtime.Invoke(request, base, runtime.KernelTarget(339)); }
      catch (const browser::Cancelled&) { cancelled.store(true); }
    });
    while (!entered.load()) { std::this_thread::yield(); }
    if (cancellation) { runtime.Cancel(); }
    const auto failure = runtime.Join();
    finished.store(true);
    observer.join();
    if (failure) { std::rethrow_exception(failure); }
    check(!torn.load(), "RC4 publication torn under a shared lease");
    State finalState;
    std::vector<uint8_t> finalData(length);
    memory.Read({}, 0x11000, finalState.data(), finalState.size());
    memory.Read({}, 0x20000, finalData.data(), finalData.size());
    if (cancelled.load()) {
      check(finalState == initial && finalData == input, "cancelled RC4 operation partially published");
    } else {
      check(finalState == expectedState && finalData == expectedData, "RC4 operation did not publish complete output");
    }
  }
  std::puts("kernel-rc4-contracts-ok");
}
