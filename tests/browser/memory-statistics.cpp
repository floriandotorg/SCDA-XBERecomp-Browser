#include "browser/virtual-memory.h"
#include "browser/image.h"
#include "browser/kernel-provider.h"
#include "browser/section-provider.h"
#include "browser/contiguous-memory.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto rejected = [](const auto& operation) {
  bool refused = false;
  try { operation(); } catch (const browser::Fault&) { refused = true; }
  check(refused, "invalid statistics request accepted");
};
const auto fixture = [] {
  std::vector<uint8_t> bytes(0x3000);
  std::memcpy(bytes.data(), "XBEH", 4);
  for (const auto& [offset, value] : std::vector<std::pair<uint32_t, uint32_t>>{
    {0x104, 0x10000}, {0x108, 0x1000}, {0x10c, 0x6000}, {0x118, 0x10200},
    {0x11c, 2}, {0x120, 0x10240}, {0x128, 0x11000 ^ xbe::kRetailEntryKey},
    {0x158, 0x11040 ^ xbe::kRetailThunkKey}, {0x200, 12},
    {0x240, xbe::kExecutable | xbe::kPreload}, {0x244, 0x11000}, {0x248, 0x1000},
    {0x24c, 0x1000}, {0x250, 0x1000}, {0x254, 0x10400}, {0x25c, 0x10480}, {0x260, 0x10480},
    {0x278, xbe::kWritable}, {0x27c, 0x13000}, {0x280, 0x2000},
    {0x284, 0x2000}, {0x288, 0x1000}, {0x28c, 0x10400}, {0x294, 0x10482}, {0x298, 0x10484}
  }) { std::memcpy(bytes.data() + offset, &value, 4); }
  return bytes;
};
using Statistics = std::array<uint32_t, 9>;

int main() try {
  {
    browser::Runtime runtime({}, {});
    browser::Memory memory;
    browser::InstallVirtualMemory(runtime, memory);
    check(!runtime.KernelTarget(181), "statistics exposed without an authenticated image");
  }
  {
    auto image = browser::PrepareImage(fixture(), {0x10000, 0x16000, 0x11000});
    browser::Runtime runtime({}, {});
    runtime.RegisterKernel(181, [](X86Context&, uint8_t*) {});
    rejected([&] { browser::InstallVirtualMemory(runtime, *image); });
    check(!runtime.KernelTarget(184) && !runtime.KernelTarget(217), "conflicting statistics export partially installed VM owner");
  }
  {
    auto image = browser::PrepareImage(fixture(), {0x10000, 0x16000, 0x11000});
    browser::Runtime runtime({}, {});
    browser::InstallContiguousMemory(runtime, *image->memory);
    rejected([&] { browser::InstallVirtualMemory(runtime, *image); });
    check(!runtime.KernelTarget(181) && !runtime.KernelTarget(184), "statistics silently ignored installed contiguous owner");
  }
  {
    auto image = browser::PrepareImage(fixture(), {0x10000, 0x16000, 0x11000});
    browser::Runtime runtime({}, {});
    image->memory->Ensure(0x20000, 4096);
    browser::InstallVirtualMemory(runtime, *image);
    browser::InstallContiguousMemory(runtime, *image->memory);
    browser::BindImageImports(*image, runtime, {});
    X86Context context{};
    context.host = &runtime;
    runtime.Launch(context, image->memory->Base(), [&](X86Context& ctx, uint8_t* base) {
      ctx.esp = 0x20000;
      ctx.last_icall = runtime.KernelTarget(181);
      const std::array<uint32_t, 2> frame{0x12345678, 0x20100};
      const uint32_t length = 36;
      image->memory->Write({}, ctx.esp, frame.data(), sizeof(frame));
      image->memory->Write({}, frame[1], &length, 4);
      rejected([&] { runtime.Invoke(ctx, base, runtime.KernelTarget(181)); });
    });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
  {
    auto image = browser::PrepareImage(fixture(), {0x10000, 0x16000, 0x11000});
    browser::Runtime runtime({}, {});
    auto& memory = *image->memory;
    memory.Ensure(0x20000, 4096);
    browser::InstallVirtualMemory(runtime, *image);
    browser::BindImageImports(*image, runtime, {});
    X86Context context{};
    context.host = &runtime;
    context.esp = 0x20000;
    context.last_icall = runtime.KernelTarget(181);
    const std::array<uint32_t, 2> frame{0x12345678, 0x20100};
    Statistics sentinel{};
    sentinel.fill(0xdeadbeef);
    sentinel[0] = 36;
    memory.Write({}, context.esp, frame.data(), sizeof(frame));
    memory.Write({}, frame[1], sentinel.data(), sizeof(sentinel));
    std::atomic<bool> entered = false, cancelled = false;
    {
      auto mutation = memory.Mutate();
      runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
        entered.store(true);
        try {
          runtime.Invoke(ctx, base, runtime.KernelTarget(181));
        } catch (const browser::Cancelled&) {
          cancelled.store(true);
          check(std::memcmp(&context, &ctx, sizeof(ctx)) == 0, "blocked cancelled snapshot changed context");
          return;
        }
        check(false, "cancelled snapshot published after waiting for backing");
      });
      while (!entered.load()) { std::this_thread::yield(); }
      while (image->threadLock.try_lock()) { image->threadLock.unlock(); std::this_thread::yield(); }
      runtime.Cancel();
    }
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    Statistics after{};
    memory.Read({}, frame[1], after.data(), sizeof(after));
    check(cancelled.load() && after == sentinel, "blocked snapshot ignored cancellation or partially published output");
  }
  {
    auto image = browser::PrepareImage(fixture(), {0x10000, 0x16000, 0x11000});
    browser::Runtime runtime({}, {});
    auto& memory = *image->memory;
    memory.Ensure(0x20000, 4096);
    browser::InstallVirtualMemory(runtime, *image);
    browser::BindImageImports(*image, runtime, {});
    X86Context context{};
    context.host = &runtime;
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      ctx.esp = 0x20000;
      const std::array<uint32_t, 2> frame{0x12345678, 0x20100};
      const uint32_t length = 36;
      memory.Write({}, ctx.esp, frame.data(), sizeof(frame));
      memory.Write({}, frame[1], &length, 4);
      runtime.Invoke(ctx, base, runtime.KernelTarget(181));
      Statistics result{};
      memory.Read({}, frame[1], result.data(), sizeof(result));
      check(ctx.eax == 0 && ctx.esp == 0x20008 && result == Statistics{36, 16384, 16381, 0, 0, 0, 0, 0, 2},
        "image without a contiguous provider could not report its actual private backing");
    });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
  for (uint32_t n = 0; n < 3; ++n) {
    auto image = browser::PrepareImage(fixture(), {0x10000, 0x16000, 0x11000});
    browser::Runtime runtime({}, {});
    auto& memory = *image->memory;
    memory.Map(0x20000, 0x4000);
    memory.Commit(0x20000, 0x3000);
    memory.Ensure(0x30000, 0x2000);
    browser::InstallKernelProvider(runtime, memory);
    browser::InstallSectionProvider(runtime, *image);
    browser::QueryContiguousMemoryUsage contiguousUsage;
    browser::InstallContiguousMemory(runtime, memory, &contiguousUsage);
    browser::InstallVirtualMemory(runtime, *image, contiguousUsage);
    browser::BindImageImports(*image, runtime, {});
    check(runtime.KernelTarget(181) && runtime.HasPublishedImage(), "statistics export absent from bound image");
    const auto first = browser::PrepareImageThread(*image, runtime, 0x10, 0x18001);
    check(first.stackTop - first.stackLimit == 0x19000, "thread fixture stack rounding changed");
    X86Context context = first.context;
    context.eax = 0xabcdef01;
    context.ebx = 0x98765432;
    context.edx = 0x99887766;
    context.cf = 1;
    const auto frame = [&](X86Context& ctx, uint32_t output) {
      ctx.last_icall = runtime.KernelTarget(181);
      const std::array<uint32_t, 2> request{0x12345678, output};
      memory.Write({}, ctx.esp, request.data(), sizeof(request));
    };
    const auto reject = [&](X86Context ctx, uint8_t* base, uint32_t output) {
      frame(ctx, output);
      const auto previous = ctx;
      std::array<uint8_t, 0x3000> before{}, after{};
      memory.Read({}, 0x20000, before.data(), before.size());
      rejected([&] { runtime.Invoke(ctx, base, runtime.KernelTarget(181)); });
      memory.Read({}, 0x20000, after.data(), after.size());
      check(before == after && std::memcmp(&previous, &ctx, sizeof(ctx)) == 0, "refused statistics request changed memory or context");
    };
    context.esp = 0x20000;
    reject(context, memory.Base(), 0x20100);
    rejected([&] { contiguousUsage(context, memory); });
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      uint32_t length = 36;
      memory.Write({}, 0x20100, &length, 4);
      const auto snapshot = [&] {
        ctx.esp = 0x20000;
        frame(ctx, 0x20100);
        const auto previous = ctx;
        std::array<uint8_t, 0x400> before{}, after{};
        memory.Read({}, 0x20000, before.data(), before.size());
        runtime.Invoke(ctx, base, runtime.KernelTarget(181));
        auto expected = previous;
        expected.eax = 0;
        expected.esp += 8;
        check(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0, "statistics clobbered state or one argument stdcall ABI");
        Statistics result{};
        memory.Read({}, 0x20100, result.data(), sizeof(result));
        std::memcpy(before.data() + 0x100, result.data(), sizeof(result));
        memory.Read({}, 0x20000, after.data(), after.size());
        check(before == after, "statistics wrote outside its 36 byte output");
        return result;
      };
      const uint32_t poolPages = browser::KernelPoolCapacity / 4096;
      const uint32_t initialPrivate = 2 + 3 + 2 + poolPages + 1 + 25;
      const Statistics initial{36, 16384, 16384 - initialPrivate, 0, 0, 0, poolPages, 25, 2};
      check(snapshot() == initial, "initial statistics counted physical aliases as allocated RAM or misclassified system pages");
      const size_t backing = memory.BackingBytes();
      const auto contiguous = [&](X86Context& request, uint32_t ordinal, uint32_t argument) {
        request.esp = 0x20800;
        const std::array<uint32_t, 2> allocation{0x12345678, argument};
        memory.Write({}, request.esp, allocation.data(), sizeof(allocation));
        runtime.Invoke(request, base, runtime.KernelTarget(ordinal));
        check(request.esp == 0x20808, "contiguous statistics fixture ABI changed");
        return request.eax;
      };
      const uint32_t contiguousAddress = contiguous(ctx, 165, 0x2001);
      check(contiguousAddress != 0 && contiguousUsage(ctx, memory) == 3, "contiguous owner did not count rounded live allocation");
      auto withContiguous = initial;
      withContiguous[2] -= 3;
      check(snapshot() == withContiguous, "live contiguous pages were missing or physically double counted");
      contiguous(ctx, 171, contiguousAddress);
      check(snapshot() == initial, "freed contiguous pages remained charged");
      browser::Memory foreignMemory;
      rejected([&] { contiguousUsage(ctx, foreignMemory); });
      const auto allocate = [&](uint32_t address, uint32_t size, uint32_t mode) {
        ctx.esp = 0x20000;
        const std::array<uint32_t, 6> request{0x12345678, 0x20200, 0, 0x20204, mode, 4};
        memory.Write({}, 0x20200, &address, 4);
        memory.Write({}, 0x20204, &size, 4);
        memory.Write({}, ctx.esp, request.data(), sizeof(request));
        runtime.Invoke(ctx, base, runtime.KernelTarget(184));
        check(ctx.eax == 0 && ctx.esp == 0x20018, "statistics allocation fixture failed");
      };
      allocate(0x10000000, 0x6000, 0x2000);
      auto expected = initial;
      expected[4] = 0x6000;
      check(snapshot() == expected, "reservation consumed physical pages or reported committed bytes");
      allocate(0x10001003, 0x1000, 0x1000);
      expected[2] -= 2;
      expected[3] = 0x2000;
      check(snapshot() == expected, "partial commit did not count exact arena pages");
      allocate(0x10001000, 0x1000, 0x1000);
      check(snapshot() == expected, "recommit double counted private pages");
      allocate(0x10010000, 0x3000, 0x3000);
      expected[2] -= 3;
      expected[3] += 0x3000;
      expected[4] += 0x3000;
      check(snapshot() == expected, "committed reservation absent from reserved byte count");
      const auto section = [&](uint32_t ordinal) {
        ctx.esp = 0x20000;
        const std::array<uint32_t, 2> request{0x12345678, 0x10278};
        memory.Write({}, ctx.esp, request.data(), sizeof(request));
        runtime.Invoke(ctx, base, runtime.KernelTarget(ordinal));
        check(ctx.eax == 0, "authenticated section fixture failed");
      };
      section(327);
      expected[2] -= 2;
      expected[8] += 2;
      check(snapshot() == expected, "loaded authenticated image pages missing");
      section(328);
      expected[2] += 2;
      expected[8] -= 2;
      check(snapshot() == expected, "unloaded image pages remained charged");
      memory.Ensure(0x40000, 0x3000);
      expected[2] -= 3;
      check(snapshot() == expected, "other private system pages were ignored or misclassified");
      memory.Decommit(0x40000, 0x1000);
      expected[2] += 1;
      check(snapshot() == expected, "system decommit did not restore available pages");
      memory.Discard(0x40000, 0x3000);
      expected[2] += 2;
      check(snapshot() == expected, "discarded system backing remained charged");
      const uint32_t changed = 0x1234abcd;
      memory.Write({}, 0x80000100, &changed, 4);
      memory.Write({}, 0xf0000100, &changed, 4);
      memory.Decommit(0x80000000, 4096);
      check(snapshot() == expected, "physical alias access or reservoir changed logical allocation");
      ctx.esp = 0x20000;
      const std::array<uint32_t, 3> poolAllocation{0x12345678, 0x3001, 0x74657374};
      memory.Write({}, ctx.esp, poolAllocation.data(), sizeof(poolAllocation));
      runtime.Invoke(ctx, base, runtime.KernelTarget(15));
      const uint32_t poolAddress = ctx.eax;
      check(poolAddress >= browser::KernelPoolBase, "pool fixture allocation failed");
      check(snapshot() == expected, "pool allocation counted existing resident pool pages twice");
      ctx.esp = 0x20000;
      const std::array<uint32_t, 2> poolFree{0x12345678, poolAddress};
      memory.Write({}, ctx.esp, poolFree.data(), sizeof(poolFree));
      runtime.Invoke(ctx, base, runtime.KernelTarget(17));
      check(snapshot() == expected, "freed resident pool pages vanished from pool charge");
      check(memory.BackingBytes() == backing, "statistics or fixture changed resident backing unexpectedly");
      image->image.base = 0x70000;
      image->image.image_size = 0x1000;
      check(snapshot() == expected, "statistics trusted mutable image metadata over authenticated source");
      for (const uint32_t invalid : {0u, 35u, 37u, 0xffffffffu}) {
        Statistics sentinel{};
        sentinel.fill(0x76543210);
        sentinel[0] = invalid;
        memory.Write({}, 0x20100, sentinel.data(), sizeof(sentinel));
        ctx.esp = 0x20000;
        frame(ctx, 0x20100);
        auto previous = ctx;
        runtime.Invoke(ctx, base, runtime.KernelTarget(181));
        previous.eax = 0xc000000d;
        previous.esp += 8;
        Statistics after{};
        memory.Read({}, 0x20100, after.data(), sizeof(after));
        check(after == sentinel && std::memcmp(&previous, &ctx, sizeof(ctx)) == 0, "invalid length mutated output or stdcall state");
      }
      length = 1;
      memory.Write({}, 0x22ffc, &length, 4);
      ctx.esp = 0x20000;
      frame(ctx, 0x22ffc);
      runtime.Invoke(ctx, base, runtime.KernelTarget(181));
      check(ctx.eax == 0xc000000d && ctx.esp == 0x20008, "invalid length unnecessarily touched the inaccessible output tail");
      length = 36;
      memory.Write({}, 0x20100, &length, 4);
      memory.Write({}, 0x22ffc, &length, 4);
      memory.Write({}, 0x22ff0, &length, 4);
      for (const uint32_t output : {0u, 0x20101u, 0x22ff0u, 0x22ffcu, 0xfffffff0u, 0x20000u, 0x1fffcu, 0x23000u}) {
        ctx.esp = 0x20000;
        reject(ctx, base, output);
      }
      ctx.esp = 0x20000;
      {
        auto lease = memory.Borrow();
        reject(ctx, base, 0x20100);
      }
      reject(ctx, nullptr, 0x20100);
      reject(ctx, foreignMemory.Base(), 0x20100);
      auto foreign = ctx;
      foreign.fs_base += 4096;
      reject(foreign, base, 0x20100);
      rejected([&] { contiguousUsage(foreign, memory); });
      browser::Runtime other({}, {});
      foreign = ctx;
      foreign.host = &other;
      reject(foreign, base, 0x20100);
      image->boundRuntime = &other;
      reject(ctx, base, 0x20100);
      image->boundRuntime = &runtime;
      image->importsBound = false;
      reject(ctx, base, 0x20100);
      image->importsBound = true;
      for (const uint32_t stack : {0x22ffcu, 0x23000u, 0xfffffffcu}) {
        auto invalid = ctx;
        invalid.esp = stack;
        const auto previous = invalid;
        rejected([&] { runtime.Invoke(invalid, base, runtime.KernelTarget(181)); });
        check(std::memcmp(&previous, &invalid, sizeof(invalid)) == 0, "invalid statistics frame changed context");
      }
      image->image = *image->AuthenticatedSource();
      std::atomic<bool> start = false, writerDone = false;
      std::atomic<uint32_t> transientReads = 0;
      auto writer = ctx;
      writer.fs_base += 4096;
      runtime.Launch(writer, base, [&](X86Context& request, uint8_t*) {
        while (!start.load()) { runtime.Checkpoint(request); std::this_thread::yield(); }
        browser::LaunchImageThread(*image, runtime, first.context, 0x14, 0x22000, [&](const browser::ImageThread&) {});
        for (uint32_t n = 0; n < 64; ++n) {
          uint32_t address;
          {
            auto mutation = memory.Mutate();
            const uint32_t allocationBase = 0x10020000 + n * 0x10000;
            const uint32_t size = 4096;
            const std::array<uint32_t, 6> allocation{0x12345678, 0x20940, 0, 0x20944, 0x3000, 4};
            request.esp = 0x20900;
            memory.Write({}, 0x20940, &allocationBase, 4);
            memory.Write({}, 0x20944, &size, 4);
            memory.Write({}, request.esp, allocation.data(), sizeof(allocation));
            runtime.Invoke(request, base, runtime.KernelTarget(184));
            check(request.eax == 0, "concurrent reservation and commitment failed");
            memory.Commit(0x10003000, 0x1000);
            memory.Commit(0x13000, 0x1000);
            address = contiguous(request, 165, 0x2001);
            check(address != 0, "concurrent contiguous allocation failed");
          }
          if (!n) {
            while (!transientReads.load()) { runtime.Checkpoint(request); std::this_thread::yield(); }
          }
          std::this_thread::yield();
          {
            auto mutation = memory.Mutate();
            memory.Decommit(0x10003000, 0x1000);
            memory.Decommit(0x13000, 0x1000);
            contiguous(request, 171, address);
          }
          std::this_thread::yield();
        }
        writerDone.store(true);
      });
      start.store(true);
      for (uint32_t n = 0; n < 1000 || !writerDone.load(); ++n) {
        const auto result = snapshot();
        check(result[4] >= expected[4] && result[4] <= expected[4] + 64 * 4096 && !(result[4] % 4096),
          "concurrent reservation ledger size was torn");
        const uint32_t reservedBytes = result[4] - expected[4];
        auto beforeChild = expected;
        auto afterChild = expected;
        afterChild[2] -= 35 + reservedBytes / 4096;
        afterChild[3] += reservedBytes;
        afterChild[4] += reservedBytes;
        afterChild[7] += 34;
        auto changedPages = afterChild;
        changedPages[2] -= 5;
        changedPages[3] += 4096;
        changedPages[8] += 1;
        check(result == beforeChild || result == afterChild || result == changedPages,
          "statistics mixed thread publication, committed categories and available pages from different snapshots");
        if (result == changedPages) { ++transientReads; }
        std::this_thread::yield();
      }
      check(transientReads.load() > 0, "concurrency fixture never exposed a live multi category allocation snapshot");
      expected[2] -= 35 + 64;
      expected[3] += 64 * 4096;
      expected[4] += 64 * 4096;
      expected[7] += 34;
      check(snapshot() == expected, "prepared child stack or KPCR accounting incorrect");
      const uint32_t overcommitted = contiguous(ctx, 165, 63u << 20);
      check(overcommitted != 0, "existing contiguous capacity was changed");
      ctx.esp = 0x20000;
      reject(ctx, base, 0x20100);
      contiguous(ctx, 171, overcommitted);
      check(snapshot() == expected, "overcommit refusal corrupted allocation or statistics ownership");
      runtime.Cancel();
      ctx.esp = 0x20000;
      reject(ctx, base, 0x20100);
    });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(runtime.Stop() == nullptr && runtime.ActiveWorkers() == 0, "statistics teardown retained workers");
  }
  std::puts("memory-statistics-contracts-ok");
} catch (const std::exception& error) {
  std::fprintf(stderr, "%s\n", error.what());
  return 1;
}
