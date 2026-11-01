#include "browser/kernel-provider.h"
#include "kernel/crypto.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include "vectors.h"

const auto check = [](bool value, const char* reason) {
  if (!value) { std::fprintf(stderr, "%s\n", reason); throw std::runtime_error(reason); }
};

int main() {
  using Context = std::array<uint8_t, 116>;
  using Snapshot = std::vector<uint8_t>;
  Context initial{};
  const std::array<uint8_t, 20> iv = {1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0xfe, 0xdc,
    0xba, 0x98, 0x76, 0x54, 0x32, 0x10, 0xf0, 0xe1, 0xd2, 0xc3};
  std::copy(iv.begin(), iv.end(), initial.begin());
  std::array<uint8_t, 128> key;
  std::array<uint8_t, 4097> input;
  for (uint32_t n = 0; n < key.size(); ++n) { key[n] = uint8_t(n * 13); }
  for (uint32_t n = 0; n < input.size(); ++n) { input[n] = uint8_t(n * 17); }
  for (const auto& vector : coreVectors) {
    for (const uint32_t split : {0u, vector.inputBytes / 2, vector.inputBytes}) {
      Context state{};
      crypto::ShaInit(state.data());
      check(state == initial, "core SHA initial state or padding incorrect");
      crypto::ShaUpdate(state.data(), input.data(), split);
      crypto::ShaUpdate(state.data(), input.data() + split, vector.inputBytes - split);
      std::array<uint8_t, 20> digest;
      crypto::ShaFinal(state.data(), digest.data());
      check(digest == vector.sha, "core SHA known answer changed");
      crypto::Hmac(key.data(), vector.keyBytes, input.data(), split, input.data() + split,
        vector.inputBytes - split, digest.data());
      check(digest == vector.hmac, "core HMAC known answer changed");
    }
  }
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime runtime({}, {});
    browser::Runtime foreign({}, {});
    browser::Memory memory;
    browser::Memory foreignMemory;
    memory.Map(0x10000, 0x2000);
    memory.Commit(0x10000, 0x2000);
    memory.Map(0x12000, 0x2000);
    memory.Commit(0x12000, 0x1000);
    browser::InstallKernelProvider(runtime, memory);
    const auto target = runtime.KernelTarget(335);
    check(target, "SHA init provider target absent");
    const auto updateTarget = runtime.KernelTarget(336);
    check(updateTarget, "SHA update provider target absent");
    const auto finalTarget = runtime.KernelTarget(337);
    check(finalTarget, "SHA final provider target absent");
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
    original.xflags = 0x200000;
    original.top = 3;
    original.fcw = 0x37f;
    original.mxcsr = 0x1f80;
    original.xmm[7].q[1] = UINT64_MAX;
    original.st[4] = 123.5;
    original.mm[3] = 0xabcdef0123456789;
    original.thread_handle = 17;
    const auto snapshot = [&] {
      Snapshot bytes(0x3000);
      memory.Read({}, 0x10000, bytes.data(), bytes.size());
      return bytes;
    };
    const auto setup = [&](uint32_t output, uint32_t stack = 0x10000) {
      Snapshot bytes(0x3000, 0xa5);
      memory.Write({}, 0x10000, bytes.data(), bytes.size());
      const std::array<uint32_t, 2> frame = {0xfffffff0, output};
      memory.Write({}, stack, frame.data(), sizeof(frame));
      auto ctx = original;
      ctx.esp = stack;
      return ctx;
    };
    const auto unchanged = [&](const X86Context& ctx, const X86Context& before, uint32_t stack) {
      auto expected = before;
      expected.esp = stack;
      expected.last_icall = ctx.last_icall;
      check(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0, "SHA init clobbered CPU state");
    };
    const auto invoke = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), target); };
    const auto reject = [&](X86Context ctx, auto operation) {
      const auto bytes = snapshot();
      const auto before = ctx;
      bool rejected = false;
      try { operation(ctx); } catch (const browser::Fault&) { rejected = true; }
      check(rejected, "invalid SHA init request accepted");
      check(snapshot() == bytes, "SHA init fault partially published");
      unchanged(ctx, before, before.esp);
    };
    reject(setup(0x11000), invoke);
    runtime.Launch(original, memory.Base(), [&](X86Context&, uint8_t*) {
      for (const uint32_t output : {0x10008u, 0x10800u, 0x10801u, 0x10fffu, 0x11fffu, 0x12f8cu}) {
        auto ctx = setup(output);
        auto expected = snapshot();
        std::copy(initial.begin(), initial.end(), expected.begin() + output - 0x10000);
        invoke(ctx);
        check(snapshot() == expected, "SHA initial state, 116 byte bounds or sentinels incorrect");
        unchanged(ctx, original, 0x10008);
      }
      {
        auto ctx = setup(0x10f8c, 0x11000);
        auto expected = snapshot();
        std::copy(initial.begin(), initial.end(), expected.begin() + 0xf8c);
        invoke(ctx);
        check(snapshot() == expected, "SHA output ending at call frame rejected or overwritten");
        unchanged(ctx, original, 0x11008);
      }
      for (const uint32_t stack : {0x10001u, 0x12ff8u}) {
        auto ctx = setup(0x11000, stack);
        const auto before = ctx;
        invoke(ctx);
        unchanged(ctx, before, stack + 8);
      }
      for (const uint32_t output : {0u, 0x12f8du, 0x13000u, 0x14000u, 0xffffff8cu, UINT32_MAX,
          0x10000u, 0x10001u, 0x10004u, 0x10007u}) {
        reject(setup(output), invoke);
      }
      reject(setup(0x10fffu, 0x11000), invoke);
      for (const uint32_t stack : {0u, 0x12ff9u, 0x13000u, 0xfffffff8u, 0xfffffffcu}) {
        auto ctx = setup(0x11000);
        ctx.esp = stack;
        reject(ctx, invoke);
      }
      auto ctx = setup(0x11000);
      ctx.host = &foreign;
      reject(ctx, invoke);
      ctx.host = nullptr;
      reject(ctx, invoke);
      ctx = setup(0x11000);
      ctx.fs_base ^= 4;
      reject(ctx, invoke);
      reject(setup(0x11000), [&](X86Context& request) { runtime.Invoke(request, nullptr, target); });
      reject(setup(0x11000), [&](X86Context& request) { runtime.Invoke(request, foreignMemory.Base(), target); });
      {
        auto lease = memory.Borrow();
        reject(setup(0x11000), invoke);
      }
      setup(0x10800);
      for (const uint32_t output : {0x10800u, 0x11001u, 0x12000u}) {
        const std::array<uint32_t, 2> frame = {0, output};
        memory.Write({}, original.esp, frame.data(), sizeof(frame));
        auto request = original;
        invoke(request);
      }
      for (uint32_t n = 0; n < vectors.size(); ++n) {
        const uint32_t output = std::array<uint32_t, 3>{0x10800u, 0x11001u, 0x12000u}[n];
        Context state;
        memory.Read({}, output, state.data(), state.size());
        check(state == initial, "independent SHA context was changed");
        const std::array<uint8_t, 3> abc = {'a', 'b', 'c'};
        auto copy = state;
        crypto::ShaUpdate(copy.data(), abc.data(), n);
        memory.Write({}, output, copy.data(), copy.size());
        Context copied;
        memory.Copy({}, 0x12501, output, state.size());
        memory.Read({}, 0x12501, copied.data(), copied.size());
        std::array<uint8_t, 20> digest;
        crypto::ShaFinal(copied.data(), digest.data());
        check(digest == vectors[n], "copied guest SHA state is not genuine or independent");
        memory.Read({}, output, copied.data(), copied.size());
        check(copied == copy, "SHA context copy mutated original");
      }
      const auto invokeUpdate = [&](X86Context& ctx) { runtime.Invoke(ctx, memory.Base(), updateTarget); };
      const auto setupUpdate = [&](uint32_t output, uint32_t source, uint32_t length, uint32_t stack = 0x10000) {
        auto request = setup(output, stack);
        const std::array<uint32_t, 4> frame = {0xfffffff0, output, source, length};
        memory.Write({}, stack, frame.data(), sizeof(frame));
        return request;
      };
      auto unownedRequest = setupUpdate(0x11000, 0x10800, 1);
      std::thread foreignWorker([&] { reject(unownedRequest, invokeUpdate); });
      foreignWorker.join();
      for (const auto& vector : coreVectors) {
        for (const uint32_t split : {0u, vector.inputBytes / 2, vector.inputBytes}) {
          auto request = setupUpdate(0x12001, 0x10801, split);
          memory.Write({}, 0x10801, input.data(), vector.inputBytes);
          memory.Write({}, 0x12001, initial.data(), initial.size());
          auto expected = snapshot();
          auto state = initial;
          crypto::ShaUpdate(state.data(), input.data(), split);
          std::copy(state.begin(), state.end(), expected.begin() + 0x2001);
          invokeUpdate(request);
          check(snapshot() == expected, "SHA update initial split or bounds incorrect");
          unchanged(request, original, 0x10010);
          const std::array<uint32_t, 4> frame = {0, 0x12001, 0x10801 + split, vector.inputBytes - split};
          memory.Write({}, 0x10001, frame.data(), sizeof(frame));
          request = original;
          request.esp = 0x10001;
          const auto before = request;
          invokeUpdate(request);
          unchanged(request, before, 0x10011);
          memory.Read({}, 0x12001, state.data(), state.size());
          std::array<uint8_t, 20> digest;
          crypto::ShaFinal(state.data(), digest.data());
          check(digest == vector.sha, "guest SHA update split known answer incorrect");
        }
      }
      for (const uint32_t source : {0u, UINT32_MAX, 0x13000u, 0x11000u}) {
        auto request = setupUpdate(0x11000, source, 0);
        memory.Write({}, 0x11000, initial.data(), initial.size());
        const auto expected = snapshot();
        invokeUpdate(request);
        check(snapshot() == expected, "empty SHA update changed context");
        unchanged(request, original, 0x10010);
      }
      for (const uint32_t source : {0x10000u, 0x10fffu, 0x11000u, 0x11001u, 0x11073u}) {
        const uint32_t length = source == 0x10000 ? 16 : 256;
        auto request = setupUpdate(0x11000, source, length);
        memory.Write({}, 0x11000, initial.data(), initial.size());
        auto expected = snapshot();
        std::array<uint8_t, 256> captured;
        memory.Read({}, source, captured.data(), length);
        auto state = initial;
        crypto::ShaUpdate(state.data(), captured.data(), length);
        std::copy(state.begin(), state.end(), expected.begin() + 0x1000);
        invokeUpdate(request);
        check(snapshot() == expected, "SHA update source overlap not snapshot safe");
        unchanged(request, original, 0x10010);
      }
      setup(0x12000);
      for (const uint32_t output : {0x12000u, 0x12101u, 0x12200u}) {
        memory.Write({}, output, initial.data(), initial.size());
      }
      for (uint32_t n = 0; n < 3; ++n) {
        const uint32_t output = std::array<uint32_t, 3>{0x12000u, 0x12101u, 0x12200u}[n];
        memory.Write({}, 0x10801, "abc", 3);
        const std::array<uint32_t, 4> frame = {0, output, 0x10801, n};
        memory.Write({}, 0x10000, frame.data(), sizeof(frame));
        auto request = original;
        invokeUpdate(request);
        memory.Copy({}, 0x12501, output, initial.size());
        Context state;
        memory.Read({}, 0x12501, state.data(), state.size());
        std::array<uint8_t, 20> digest;
        crypto::ShaFinal(state.data(), digest.data());
        check(digest == vectors[n], "SHA update independent context hash incorrect");
      }
      memory.Copy({}, 0x12301, 0x12101, initial.size());
      memory.Write({}, 0x10801, "b", 1);
      const std::array<uint32_t, 4> forkFrame = {0, 0x12301, 0x10801, 1};
      memory.Write({}, 0x10000, forkFrame.data(), sizeof(forkFrame));
      auto forkRequest = original;
      invokeUpdate(forkRequest);
      Context fork;
      memory.Read({}, 0x12301, fork.data(), fork.size());
      std::array<uint8_t, 20> forkDigest;
      crypto::ShaFinal(fork.data(), forkDigest.data());
      check(forkDigest == vectors[2], "copied SHA context cannot fork independently");
      memory.Read({}, 0x12101, fork.data(), fork.size());
      crypto::ShaFinal(fork.data(), forkDigest.data());
      check(forkDigest == vectors[1], "forked SHA update changed original context");
      for (const uint32_t output : {0u, 0x12f8du, 0x13000u, 0xffffff8cu, UINT32_MAX,
          0x10000u, 0x10004u, 0x1000cu, 0x1000fu}) {
        reject(setupUpdate(output, 0, 0), invokeUpdate);
      }
      reject(setupUpdate(0x10fff, 0, 0, 0x11000), invokeUpdate);
      for (const auto& source : std::array<std::array<uint32_t, 2>, 7>{{
          {0, 1}, {0x12fff, 2}, {0x13000, 1}, {0x10800, UINT32_MAX},
          {0xfffffff0, 32}, {0x10800, 0x3000}, {0x12fff, 4097}}}) {
        reject(setupUpdate(0x12000, source[0], source[1]), invokeUpdate);
      }
      for (const uint32_t stack : {0u, 0x12ff1u, 0x13000u, 0xfffffff0u, 0xfffffffcu}) {
        auto request = setupUpdate(0x12000, 0, 0);
        request.esp = stack;
        reject(request, invokeUpdate);
      }
      auto updateRequest = setupUpdate(0x12000, 0, 0);
      updateRequest.host = &foreign;
      reject(updateRequest, invokeUpdate);
      updateRequest.host = nullptr;
      reject(updateRequest, invokeUpdate);
      updateRequest = setupUpdate(0x12000, 0, 0);
      updateRequest.fs_base ^= 4;
      reject(updateRequest, invokeUpdate);
      reject(setupUpdate(0x12000, 0, 0), [&](X86Context& request) { runtime.Invoke(request, nullptr, updateTarget); });
      reject(setupUpdate(0x12000, 0, 0), [&](X86Context& request) { runtime.Invoke(request, foreignMemory.Base(), updateTarget); });
      {
        auto lease = memory.Borrow();
        reject(setupUpdate(0x12000, 0, 0), invokeUpdate);
      }
      const auto invokeFinal = [&](X86Context& request) { runtime.Invoke(request, memory.Base(), finalTarget); };
      const auto setupFinal = [&](uint32_t output, uint32_t digest, uint32_t stack = 0x10000) {
        auto request = setup(output, stack);
        const std::array<uint32_t, 3> frame = {0xfffffff0, output, digest};
        memory.Write({}, stack, frame.data(), sizeof(frame));
        return request;
      };
      const auto call = [&](uint32_t ordinal, uint32_t context, uint32_t source, uint32_t length) {
        const std::array<uint32_t, 4> frame = {0, context, source, length};
        memory.Write({}, 0x10001, frame.data(), sizeof(frame));
        auto request = original;
        request.esp = 0x10001;
        const auto before = request;
        runtime.Invoke(request, memory.Base(), runtime.KernelTarget(ordinal));
        unchanged(request, before, 0x10001 + (ordinal == 335 ? 8 : ordinal == 336 ? 16 : 12));
      };
      for (const auto& vector : coreVectors) {
        for (const uint32_t split : {0u, vector.inputBytes / 2, vector.inputBytes}) {
          setup(0x12001);
          memory.Write({}, 0x10801, input.data(), vector.inputBytes);
          call(335, 0x12001, 0, 0);
          call(336, 0x12001, 0x10801, split);
          call(336, 0x12001, 0x10801 + split, vector.inputBytes - split);
          Context state;
          memory.Read({}, 0x12001, state.data(), state.size());
          std::array<uint8_t, 20> digest;
          crypto::ShaFinal(state.data(), digest.data());
          check(digest == vector.sha, "actual SHA API prefinal known answer incorrect");
          const std::array<uint32_t, 3> frame = {0, 0x12001, 0x12601};
          memory.Write({}, 0x10000, frame.data(), sizeof(frame));
          auto expected = snapshot();
          std::copy(state.begin(), state.end(), expected.begin() + 0x2001);
          std::copy(vector.sha.begin(), vector.sha.end(), expected.begin() + 0x2601);
          auto request = original;
          invokeFinal(request);
          check(snapshot() == expected, "actual SHA final context, digest or sentinels incorrect");
          unchanged(request, original, 0x1000c);
          call(335, 0x12001, 0, 0);
          memory.Read({}, 0x12001, state.data(), state.size());
          check(state == initial, "finalized SHA context cannot be reinitialized");
          memory.Read({}, 0x12601, digest.data(), digest.size());
          check(digest == vector.sha, "SHA reinit changed published digest");
        }
      }
      setup(0x12000);
      memory.Write({}, 0x10801, "ab", 2);
      call(335, 0x12000, 0, 0);
      call(336, 0x12000, 0x10801, 1);
      memory.Copy({}, 0x12301, 0x12000, initial.size());
      call(336, 0x12301, 0x10802, 1);
      call(335, 0x12401, 0, 0);
      for (uint32_t n = 0; n < 3; ++n) {
        const uint32_t context = std::array<uint32_t, 3>{0x12401u, 0x12000u, 0x12301u}[n];
        call(337, context, 0x12601, 0);
        std::array<uint8_t, 20> digest;
        memory.Read({}, 0x12601, digest.data(), digest.size());
        check(digest == vectors[n], "actual SHA final copied fork or independent context incorrect");
      }
      for (const uint32_t digest : {0x1000cu, 0x10fedu, 0x10fffu, 0x11000u, 0x11001u, 0x11060u,
          0x11073u, 0x11074u, 0x11fffu, 0x12fecu}) {
        auto request = setupFinal(0x11000, digest);
        memory.Write({}, 0x11000, initial.data(), initial.size());
        auto expected = snapshot();
        auto state = initial;
        std::array<uint8_t, 20> hash;
        crypto::ShaFinal(state.data(), hash.data());
        std::copy(state.begin(), state.end(), expected.begin() + 0x1000);
        std::copy(hash.begin(), hash.end(), expected.begin() + digest - 0x10000);
        invokeFinal(request);
        check(snapshot() == expected, "SHA final alias last digest wins or output bounds incorrect");
        unchanged(request, original, 0x1000c);
      }
      for (const uint32_t output : {0u, 0x12f8du, 0x13000u, 0xffffff8cu, UINT32_MAX,
          0x10000u, 0x10004u, 0x10008u, 0x1000bu}) {
        reject(setupFinal(output, 0x12600), invokeFinal);
      }
      for (const uint32_t digest : {0u, 0x12fedu, 0x13000u, 0xfffffff0u, UINT32_MAX,
          0x10000u, 0x10004u, 0x10008u, 0x1000bu}) {
        reject(setupFinal(0x12000, digest), invokeFinal);
      }
      reject(setupFinal(0x12000, 0x10fff, 0x11000), invokeFinal);
      reject(setupFinal(0x10fff, 0x12000, 0x11000), invokeFinal);
      for (const uint32_t stack : {0u, 0x12ff5u, 0x13000u, 0xfffffff4u, 0xfffffffcu}) {
        auto request = setupFinal(0x12000, 0x12600);
        request.esp = stack;
        reject(request, invokeFinal);
      }
      auto finalRequest = setupFinal(0x12000, 0x12600);
      std::thread finalForeignWorker([&] { reject(finalRequest, invokeFinal); });
      finalForeignWorker.join();
      finalRequest.host = &foreign;
      reject(finalRequest, invokeFinal);
      finalRequest.host = nullptr;
      reject(finalRequest, invokeFinal);
      finalRequest = setupFinal(0x12000, 0x12600);
      finalRequest.fs_base ^= 4;
      reject(finalRequest, invokeFinal);
      reject(setupFinal(0x12000, 0x12600), [&](X86Context& request) { runtime.Invoke(request, nullptr, finalTarget); });
      reject(setupFinal(0x12000, 0x12600), [&](X86Context& request) { runtime.Invoke(request, foreignMemory.Base(), finalTarget); });
      {
        auto lease = memory.Borrow();
        reject(setupFinal(0x12000, 0x12600), invokeFinal);
      }
      auto ctxBeforeCancel = setup(0x11000);
      auto updateBeforeCancel = setupUpdate(0x12000, 0, 0);
      auto finalBeforeCancel = setupFinal(0x12000, 0x12600);
      runtime.Cancel();
      reject(ctxBeforeCancel, invoke);
      reject(updateBeforeCancel, invokeUpdate);
      reject(finalBeforeCancel, invokeFinal);
    });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(runtime.ActiveWorkers() == 0, "SHA worker did not join");
    reject(setup(0x11000), invoke);
  }
  browser::Runtime runtime({}, {});
  browser::Memory memory;
  memory.Map(0x10000, 0x4000);
  memory.Commit(0x10000, 0x4000);
  browser::InstallKernelProvider(runtime, memory);
  memory.Write({}, 0x12001, input.data(), input.size());
  auto updated = initial;
  crypto::ShaUpdate(updated.data(), input.data(), input.size());
  X86Context ctx{};
  ctx.host = &runtime;
  ctx.esp = 0x10000;
  std::atomic<bool> complete = false;
  std::atomic<bool> torn = false;
  std::atomic<uint32_t> observations = 0;
  Context sentinel;
  sentinel.fill(0xa5);
  memory.Write({}, 0x11000, sentinel.data(), sentinel.size());
  std::thread observer([&] {
    while (!complete.load()) {
      Context current;
      memory.Read({}, 0x11000, current.data(), current.size());
      if (current != sentinel && current != initial && current != updated) { torn.store(true); }
      ++observations;
    }
  });
  while (!observations.load()) { std::this_thread::yield(); }
  runtime.Launch(ctx, memory.Base(), [&](X86Context& request, uint8_t* base) {
    for (uint32_t n = 0; n < 1000; ++n) {
      const std::array<uint32_t, 2> frame = {0, 0x11000};
      memory.Write({}, 0x11000, sentinel.data(), sentinel.size());
      memory.Write({}, 0x10000, frame.data(), sizeof(frame));
      request.esp = 0x10000;
      runtime.Invoke(request, base, runtime.KernelTarget(335));
      const std::array<uint32_t, 4> updateFrame = {0, 0x11000, 0x12001, uint32_t(input.size())};
      memory.Write({}, 0x10000, updateFrame.data(), sizeof(updateFrame));
      request.esp = 0x10000;
      runtime.Invoke(request, base, runtime.KernelTarget(336));
    }
  });
  const auto failure = runtime.Join();
  complete.store(true);
  observer.join();
  if (failure) { std::rethrow_exception(failure); }
  check(!torn.load(), "SHA publication was torn under concurrent observation");
  constexpr uint32_t cancellationBytes = 32u << 20;
  memory.Map(0x20000, cancellationBytes);
  memory.Commit(0x20000, cancellationBytes);
  memory.Write({}, 0x11000, initial.data(), initial.size());
  const std::array<uint32_t, 4> cancellationFrame = {0, 0x11000, 0x20000, cancellationBytes};
  memory.Write({}, 0x10000, cancellationFrame.data(), sizeof(cancellationFrame));
  std::atomic<bool> entered = false;
  std::atomic<bool> cancelled = false;
  runtime.Launch(ctx, memory.Base(), [&](X86Context& request, uint8_t* base) {
    entered.store(true);
    try { runtime.Invoke(request, base, runtime.KernelTarget(336)); }
    catch (const browser::Cancelled&) { cancelled.store(true); }
  });
  while (!entered.load()) { std::this_thread::yield(); }
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  runtime.Cancel();
  if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  Context afterCancellation;
  memory.Read({}, 0x11000, afterCancellation.data(), afterCancellation.size());
  check(cancelled.load() && afterCancellation == initial, "cancelled SHA update partially published state");
  std::puts("kernel-sha-contracts-ok");
}
