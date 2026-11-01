#include "browser/virtual-memory.h"
#include "browser/image.h"
#include <array>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto fault = [](auto operation) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, "invalid virtual allocation accepted");
};

void QueryContracts() {
  for (uint32_t n = 0; n < 3; ++n) {
    std::vector<uint8_t> bytes(0x1400);
    std::memcpy(bytes.data(), "XBEH", 4);
    for (const auto& [offset, value] : std::vector<std::pair<uint32_t, uint32_t>>{
      {0x104, 0x10000}, {0x108, 0x400}, {0x10c, 0x4000}, {0x118, 0x10200},
      {0x11c, 1}, {0x120, 0x10240}, {0x128, 0x11000 ^ xbe::kRetailEntryKey},
      {0x158, 0x11040 ^ xbe::kRetailThunkKey}, {0x200, 12},
      {0x240, xbe::kExecutable | xbe::kPreload}, {0x244, 0x11000}, {0x248, 0x1000},
      {0x24c, 0x400}, {0x250, 0x1000}, {0x254, 0x10300}
    }) { std::memcpy(bytes.data() + offset, &value, 4); }
    auto image = browser::PrepareImage(std::move(bytes), {0x10000, 0x14000, 0x11000});
    browser::Runtime runtime({}, {});
    auto& memory = *image->memory;
    memory.Map(0x20000, 0x3000);
    memory.Commit(0x20000, 0x2000);
    memory.Ensure(0x40000, 0x3000);
    memory.Map(0x50000, 0x2000);
    browser::InstallVirtualMemory(runtime, *image);
    browser::BindImageImports(*image, runtime, {});
    check(runtime.KernelTarget(217), "virtual query export absent");
    fault([&] { browser::InstallVirtualMemory(runtime, memory); });
    const auto pages = memory.QueryPages(0x12001);
    check(pages.base == 0x12000 && pages.end == 0x14000 && pages.mappingBase == 0x10000 && pages.mapped && !pages.committed,
      "mapped uncommitted image query lost backing boundary");
    const auto gap = memory.QueryPages(0x14001);
    check(gap.base == 0x14000 && gap.end == 0x20000 && !gap.mapped && !gap.committed, "free query crossed mapped region");
    const auto alias = memory.QueryPages(0xf0000123);
    check(alias.base == 0xf0000000 && alias.end == 0xf4000000 && alias.mappingBase == 0xf0000000 && alias.committed,
      "kernel alias query lost commitment or mapping base");
    const auto tail = memory.QueryPages(0xffffffff);
    check(tail.base == 0xfffff000 && tail.end == (1ull << 32) && !tail.mapped, "top free page query wrapped");
    X86Context context{};
    context.host = &runtime;
    context.fs_base = 0x90000;
    context.eax = 0xabcdef01;
    context.ebx = 0x98765432;
    context.cf = 1;
    const auto frame = [&](X86Context& ctx, uint32_t address, uint32_t output) {
      ctx.last_icall = runtime.KernelTarget(217);
      const std::array<uint32_t, 3> request{0x371c3b, address, output};
      memory.Write({}, ctx.esp, request.data(), sizeof(request));
    };
    const auto reject = [&](X86Context ctx, uint8_t* base, uint32_t address, uint32_t output) {
      frame(ctx, address, output);
      const auto previous = ctx;
      std::array<uint8_t, 0x2000> before{}, after{};
      memory.Read({}, 0x20000, before.data(), before.size());
      fault([&] { runtime.Invoke(ctx, base, runtime.KernelTarget(217)); });
      memory.Read({}, 0x20000, after.data(), after.size());
      check(before == after && std::memcmp(&previous, &ctx, sizeof(ctx)) == 0, "invalid query published partial memory or context");
    };
    context.esp = 0x20000;
    reject(context, memory.Base(), 0, 0x20100);
    const auto query = [&](X86Context& ctx, uint32_t address, std::array<uint32_t, 7> expected) {
      ctx.esp = 0x20000;
      frame(ctx, address, 0x20100);
      const auto previous = ctx;
      std::array<uint8_t, 0x2000> before{}, after{};
      memory.Read({}, 0x20000, before.data(), before.size());
      runtime.Invoke(ctx, memory.Base(), runtime.KernelTarget(217));
      auto result = previous;
      result.eax = 0;
      result.esp += 12;
      check(std::memcmp(&ctx, &result, sizeof(ctx)) == 0, "query changed unrelated state or stdcall cleanup");
      std::array<uint32_t, 7> information{};
      memory.Read({}, 0x20100, information.data(), sizeof(information));
      check(information == expected, "query region fields or coalescing incorrect");
      std::memcpy(before.data() + 0x100, expected.data(), sizeof(expected));
      memory.Read({}, 0x20000, after.data(), after.size());
      check(before == after, "query wrote outside 28 byte output");
    };
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      query(ctx, 0, {0, 0, 0, 0x10000, 0x10000, 1, 0});
      query(ctx, 0x10007, {0x10000, 0x10000, 4, 0x2000, 0x1000, 4, 0x1000000});
      query(ctx, 0x11007, {0x11000, 0x10000, 4, 0x1000, 0x1000, 4, 0x1000000});
      query(ctx, 0x12000, {0x12000, 0x10000, 4, 0x2000, 0x2000, 0, 0x1000000});
      query(ctx, 0x14007, {0x14000, 0, 0, 0xc000, 0x10000, 1, 0});
      query(ctx, 0x40007, {0x40000, 0x40000, 4, 0x3000, 0x1000, 4, 0x20000});
      query(ctx, 0x50000, {0x50000, 0x50000, 4, 0x2000, 0x2000, 0, 0x20000});
      query(ctx, 0x10000007, {0x10000000, 0, 0, 0x1000000, 0x10000, 1, 0});
      const auto allocate = [&](uint32_t address, uint32_t size, uint32_t type, uint32_t protect = 4) {
        ctx.esp = 0x20000;
        const std::array<uint32_t, 6> request{0x12345678, 0x20200, 0, 0x20204, type, protect};
        memory.Write({}, 0x20200, &address, 4);
        memory.Write({}, 0x20204, &size, 4);
        memory.Write({}, ctx.esp, request.data(), sizeof(request));
        runtime.Invoke(ctx, base, runtime.KernelTarget(184));
        return ctx.eax;
      };
      check(allocate(0x10000003, 0x5ffd, 0x2000, 1) == 0, "no access reservation failed");
      uint32_t unreadable = 0;
      fault([&] { memory.Read(ctx, 0x10000000, &unreadable, 4); });
      query(ctx, 0x10000007, {0x10000000, 0x10000000, 1, 0x6000, 0x2000, 0, 0x20000});
      check(allocate(0x10001003, 0x1000, 0x1000) == 0, "query fixture partial commit failed");
      query(ctx, 0x10000007, {0x10000000, 0x10000000, 1, 0x1000, 0x2000, 0, 0x20000});
      query(ctx, 0x10001007, {0x10001000, 0x10000000, 1, 0x2000, 0x1000, 4, 0x20000});
      query(ctx, 0x10003007, {0x10003000, 0x10000000, 1, 0x3000, 0x2000, 0, 0x20000});
      check(allocate(0x10005000, 0x2000, 0x1000) == 0xc0000018, "cross reservation commit succeeded");
      query(ctx, 0x10005000, {0x10005000, 0x10000000, 1, 0x1000, 0x2000, 0, 0x20000});
      query(ctx, 0x10006000, {0x10006000, 0, 0, 0xffa000, 0x10000, 1, 0});
      for (const uint32_t output : {0u, 0x20101u, 0x21ff0u, 0xfffffff0u, 0x20000u, 0x1fffcu, 0x22000u}) {
        ctx.esp = 0x20000;
        reject(ctx, base, 0, output);
      }
      ctx.esp = 0x20000;
      {
        auto lease = memory.Borrow();
        reject(ctx, base, 0, 0x20100);
      }
      reject(ctx, nullptr, 0, 0x20100);
      auto foreign = ctx;
      foreign.fs_base += 4096;
      reject(foreign, base, 0, 0x20100);
      browser::Runtime other({}, {});
      foreign = ctx;
      foreign.host = &other;
      reject(foreign, base, 0, 0x20100);
      image->boundRuntime = &other;
      reject(ctx, base, 0, 0x20100);
      image->boundRuntime = &runtime;
      for (const uint32_t address : {0x7ffe0000u, 0x80000000u, 0xf0000000u, 0xffffffffu}) {
        frame(ctx, address, 0x20100);
        const auto previous = ctx;
        std::array<uint32_t, 7> before{}, after{};
        memory.Read({}, 0x20100, before.data(), sizeof(before));
        runtime.Invoke(ctx, base, runtime.KernelTarget(217));
        auto expected = previous;
        expected.eax = 0xc000000d;
        expected.esp += 12;
        memory.Read({}, 0x20100, after.data(), sizeof(after));
        check(before == after && std::memcmp(&expected, &ctx, sizeof(ctx)) == 0, "out of user range query published output");
      }
      for (const uint32_t stack : {0x21ff8u, 0x22000u, 0xfffffff8u}) {
        auto invalid = ctx;
        invalid.esp = stack;
        invalid.last_icall = runtime.KernelTarget(217);
        const auto previous = invalid;
        fault([&] { runtime.Invoke(invalid, base, runtime.KernelTarget(217)); });
        check(std::memcmp(&previous, &invalid, sizeof(invalid)) == 0, "invalid query stack changed context");
      }
      check(allocate(0x10010000, 0x40000, 0x2000) == 0, "concurrent query reservation failed");
      const size_t backing = memory.BackingBytes();
      std::atomic<bool> writerReady = false, readerReady = false, writerDone = false;
      auto writer = ctx;
      writer.fs_base += 4096;
      const std::array<uint32_t, 3> freeFrame{0x371c3b, 0, 0x20500};
      const std::array<uint32_t, 3> reservedFrame{0x371c3b, 0x10010000, 0x20500};
      memory.Write({}, 0x20400, freeFrame.data(), sizeof(freeFrame));
      runtime.Launch(writer, base, [&](X86Context& request, uint8_t* backingBase) {
        writerReady.store(true);
        while (!readerReady.load()) { runtime.Checkpoint(request); std::this_thread::yield(); }
        for (uint32_t n = 0; n < 64; ++n) {
          request.esp = 0x20800;
          const uint32_t address = 0x10010000 + n * 4096;
          const uint32_t length = 4096;
          const std::array<uint32_t, 6> allocation{0x12345678, 0x20900, 0, 0x20904, 0x1000, 4};
          memory.Write({}, 0x20900, &address, 4);
          memory.Write({}, 0x20904, &length, 4);
          memory.Write({}, request.esp, allocation.data(), sizeof(allocation));
          runtime.Invoke(request, backingBase, runtime.KernelTarget(184));
          check(request.eax == 0, "concurrent commit failed");
          const auto& alternate = (n % 2) ? freeFrame : reservedFrame;
          memory.Write({}, 0x20400, alternate.data(), sizeof(alternate));
          std::this_thread::yield();
        }
        writerDone.store(true);
      });
      while (!writerReady.load()) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
      readerReady.store(true);
      for (uint32_t n = 0; n < 2000; ++n) {
        ctx.esp = 0x20400;
        ctx.last_icall = runtime.KernelTarget(217);
        const auto previous = ctx;
        runtime.Invoke(ctx, base, runtime.KernelTarget(217));
        auto expected = previous;
        expected.eax = 0;
        expected.esp += 12;
        check(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0, "concurrent query changed unrelated state");
        std::array<uint32_t, 7> result{};
        memory.Read({}, 0x20500, result.data(), sizeof(result));
        if (!result[0]) {
          check(result == std::array<uint32_t, 7>{0, 0, 0, 0x10000, 0x10000, 1, 0}, "query mixed concurrent free frame and reservation snapshot");
        } else {
          check(result[0] == 0x10010000 && result[1] == 0x10010000 && result[2] == 4 && result[6] == 0x20000 &&
            result[3] && result[3] <= 0x40000 && !(result[3] % 4096) &&
            ((result[4] == 0x2000 && result[5] == 0 && result[3] == 0x40000) || (result[4] == 0x1000 && result[5] == 4)),
            "query published torn commit ledger or frame");
        }
      }
      while (!writerDone.load()) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
      query(ctx, 0x10010000, {0x10010000, 0x10010000, 4, 0x40000, 0x1000, 4, 0x20000});
      check(memory.BackingBytes() == backing, "query or concurrent commit grew backing");
      runtime.Cancel();
      ctx.esp = 0x20000;
      reject(ctx, base, 0, 0x20100);
    });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(runtime.Stop() == nullptr && runtime.ActiveWorkers() == 0, "query teardown retained worker");
  }
}

void ReleaseContracts() {
  {
    browser::Runtime occupied({}, {});
    browser::Memory occupiedMemory;
    occupied.RegisterKernel(199, [](X86Context&, uint8_t*) {});
    fault([&] { browser::InstallVirtualMemory(occupied, occupiedMemory); });
    check(!occupied.KernelTarget(184) && !occupied.KernelTarget(217), "release installation partially published sibling exports");
  }
  for (uint32_t n = 0; n < 3; ++n) {
    std::vector<uint8_t> bytes(0x1400);
    std::memcpy(bytes.data(), "XBEH", 4);
    for (const auto& [offset, value] : std::vector<std::pair<uint32_t, uint32_t>>{
      {0x104, 0x10000}, {0x108, 0x400}, {0x10c, 0x4000}, {0x118, 0x10200},
      {0x11c, 1}, {0x120, 0x10240}, {0x128, 0x11000 ^ xbe::kRetailEntryKey},
      {0x158, 0x11040 ^ xbe::kRetailThunkKey}, {0x200, 12},
      {0x240, xbe::kExecutable | xbe::kPreload}, {0x244, 0x11000}, {0x248, 0x1000},
      {0x24c, 0x400}, {0x250, 0x1000}, {0x254, 0x10300},
      {0x440, 0x800000c7}, {0x444, 0x800000b8}, {0x448, 0x800000d9}, {0x44c, 0x800000b5}
    }) { std::memcpy(bytes.data() + offset, &value, 4); }
    auto image = browser::PrepareImage(std::move(bytes), {0x10000, 0x14000, 0x11000});
    browser::Runtime runtime({}, {});
    auto& memory = *image->memory;
    memory.Ensure(0x20000, 0x2000);
    memory.Ensure(0xd203f000, 0x1000);
    browser::InstallVirtualMemory(runtime, *image);
    check(runtime.KernelTarget(199), "virtual release export absent");
    browser::BindImageImports(*image, runtime, {});
    uint32_t target = 0;
    memory.Read({}, 0x11040, &target, 4);
    check(target == runtime.KernelTarget(199), "actual release thunk was not bound to owned provider");
    const size_t backing = memory.BackingBytes();
    X86Context context{};
    context.host = &runtime;
    context.fs_base = 0x90000;
    context.ebx = 0x12345678;
    context.eax = 0xaabbccdd;
    context.cf = 1;
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      const auto allocate = [&](uint32_t address, uint32_t length, uint32_t type) {
        ctx.esp = 0x20000;
        const std::array<uint32_t, 6> frame{0x12345678, 0x20200, 0, 0x20204, type, 4};
        memory.Write({}, 0x20200, &address, 4);
        memory.Write({}, 0x20204, &length, 4);
        memory.Write({}, ctx.esp, frame.data(), sizeof(frame));
        runtime.Invoke(ctx, base, runtime.KernelTarget(184));
        check(ctx.eax == 0, "release fixture allocation failed");
        memory.Read({}, 0x20200, &address, 4);
        return address;
      };
      const auto statistics = [&] {
        ctx.esp = 0x20000;
        const std::array<uint32_t, 2> frame{0x12345678, 0x20300};
        const uint32_t length = 36;
        memory.Write({}, 0x20300, &length, 4);
        memory.Write({}, ctx.esp, frame.data(), sizeof(frame));
        runtime.Invoke(ctx, base, runtime.KernelTarget(181));
        std::array<uint32_t, 9> result{};
        memory.Read({}, 0x20300, result.data(), sizeof(result));
        check(ctx.eax == 0, "release fixture statistics failed");
        return result;
      };
      const auto query = [&](uint32_t address, uint32_t state, uint32_t allocation, uint32_t length) {
        ctx.esp = 0x20000;
        const std::array<uint32_t, 3> frame{0x12345678, address, 0x20400};
        memory.Write({}, ctx.esp, frame.data(), sizeof(frame));
        runtime.Invoke(ctx, base, runtime.KernelTarget(217));
        std::array<uint32_t, 7> result{};
        memory.Read({}, 0x20400, result.data(), sizeof(result));
        check(ctx.eax == 0 && result[0] == address && result[1] == allocation && result[3] == length && result[4] == state,
          "release query did not reflect ledger and page transition");
      };
      const auto prepare = [&](uint32_t address, uint32_t length, uint32_t type = 0x8000,
          uint32_t baseOutput = 0xd203f3dc, uint32_t sizeOutput = 0xd203f3e0, uint32_t stack = 0xd203f3c4) {
        ctx.esp = stack;
        ctx.last_icall = target;
        const std::array<uint32_t, 4> frame{0x371c18, baseOutput, sizeOutput, type};
        memory.Write({}, 0xd203f3dc, &address, 4);
        memory.Write({}, 0xd203f3e0, &length, 4);
        memory.Write({}, stack, frame.data(), sizeof(frame));
      };
      const auto invoke = [&](uint32_t status, uint32_t address, uint32_t length) {
        const auto previous = ctx;
        std::array<uint8_t, 4096> before{}, after{};
        memory.Read({}, 0xd203f000, before.data(), before.size());
        runtime.Invoke(ctx, base, target);
        auto expected = previous;
        expected.eax = status;
        expected.esp += 16;
        check(std::memcmp(&ctx, &expected, sizeof(ctx)) == 0, "release changed unrelated registers or exact 16 byte cleanup");
        std::memcpy(before.data() + 0x3dc, &address, 4);
        std::memcpy(before.data() + 0x3e0, &length, 4);
        memory.Read({}, 0xd203f000, after.data(), after.size());
        check(before == after, "release changed frame, guards, or failed outputs");
      };
      const auto reject = [&](X86Context& request, uint8_t* backingBase) {
        const auto previous = request;
        std::array<uint8_t, 4096> before{}, after{};
        memory.Read({}, 0xd203f000, before.data(), before.size());
        const auto committed = memory.QueryUsage().privateCommittedPages;
        fault([&] { runtime.Invoke(request, backingBase, target); });
        memory.Read({}, 0xd203f000, after.data(), after.size());
        check(before == after && std::memcmp(&request, &previous, sizeof(request)) == 0 &&
          memory.QueryUsage().privateCommittedPages == committed, "invalid release published a partial transition");
      };
      const auto initial = statistics();
      const uint32_t actual = allocate(0x10110000, 0x12345, 0x3000);
      const auto allocated = statistics();
      check(allocated[2] + 0x13 == initial[2] && allocated[3] == 0x13000 && allocated[4] == 0x13000,
        "release fixture statistics lost committed reservation");
      uint32_t dirty = 0xdeadbeef;
      memory.Write({}, actual, &dirty, 4);
      for (const auto& [address, length, status] : std::vector<std::array<uint32_t, 3>>{
        {actual, 1, 0xc000000d}, {actual + 1, 0, 0xc000009f}, {actual + 4096, 0, 0xc000009f},
        {0, 0, 0xc000009f}, {0x10000, 0, 0xc000009f}, {0x80000000, 0, 0xc000009f},
        {0xf0000000, 0, 0xc000009f}, {0x81000000, 0, 0xc000009f}, {0xffffffff, 0, 0xc000009f}
      }) {
        prepare(address, length);
        invoke(status, address, length);
        check(memory.IsCommitted(actual), "failed release decommitted live reservation");
      }
      for (const uint32_t type : {0u, 0x1000u, 0xc000u, 0x8001u, 0x4001u}) {
        prepare(actual, 0, type);
        reject(ctx, base);
      }
      for (const uint32_t output : {0u, 0xd203f3ddu, 0x22000u, 0xfffffffcu, 0xd203f3c4u, 0xd203f3d0u, 0xd203f3e0u}) {
        prepare(actual, 0, 0x8000, output);
        reject(ctx, base);
      }
      for (const uint32_t output : {0u, 0xd203f3e1u, 0x22000u, 0xfffffffcu, 0xd203f3c4u, 0xd203f3dcu}) {
        prepare(actual, 0, 0x8000, 0xd203f3dc, output);
        reject(ctx, base);
      }
      prepare(actual, 0);
      {
        auto lease = memory.Borrow();
        reject(ctx, base);
      }
      reject(ctx, nullptr);
      auto foreign = ctx;
      foreign.fs_base += 4096;
      reject(foreign, base);
      browser::Runtime other({}, {});
      foreign = ctx;
      foreign.host = &other;
      reject(foreign, base);
      image->boundRuntime = &other;
      reject(ctx, base);
      image->boundRuntime = &runtime;
      for (const uint32_t stack : {actual, actual + 0x12ff0}) {
        prepare(actual, 0, 0x8000, 0xd203f3dc, 0xd203f3e0, stack);
        reject(ctx, base);
      }
      for (const uint32_t output : {actual, actual + 0x12ffc}) {
        prepare(actual, 0, 0x8000, output);
        memory.Write({}, output, &actual, 4);
        reject(ctx, base);
        prepare(actual, 0, 0x8000, 0xd203f3dc, output);
        const uint32_t zero = 0;
        memory.Write({}, output, &zero, 4);
        reject(ctx, base);
      }
      prepare(actual, 0);
      invoke(0, actual, 0x13000);
      check(!memory.IsCommitted(actual) && statistics() == initial, "release failed to return committed and reserved accounting");
      fault([&] { memory.Read({}, actual, &dirty, 4); });
      query(actual, 0x10000, 0, 0x11000000 - actual);
      prepare(actual, 0);
      invoke(0xc000009f, actual, 0);
      check(allocate(actual, 0x12345, 0x3000) == actual, "released exact address was not reusable");
      for (uint32_t offset = 0; offset < 0x13000; offset += 4) {
        memory.Read({}, actual + offset, &dirty, 4);
        check(!dirty, "reused allocation exposed stale copybuffer contents");
      }
      prepare(actual, 0);
      invoke(0, actual, 0x13000);
      const uint32_t reserved = allocate(0, 0x5000, 0x2000);
      query(reserved, 0x2000, reserved, 0x5000);
      prepare(reserved, 0);
      invoke(0, reserved, 0x5000);
      check(statistics() == initial, "reserve-only release lost accounting");
      const uint32_t partial = allocate(0, 0x5000, 0x2000);
      allocate(partial + 0x1000, 0x1000, 0x1000);
      allocate(partial + 0x4000, 0x1000, 0x1000);
      prepare(partial, 0);
      invoke(0, partial, 0x5000);
      check(statistics() == initial && !memory.IsCommitted(partial + 0x1000) && !memory.IsCommitted(partial + 0x4000),
        "partial commitment release retained committed pages");
      check(allocate(0, 0x1000, 0x1000) == partial, "implicit commit reservation was not reusable");
      prepare(partial, 0);
      invoke(0, partial, 0x1000);
      const uint32_t decommitted = allocate(0x10100000, 0x10000, 0x3000);
      dirty = 0xabcdef01;
      memory.Write({}, decommitted + 0x3000, &dirty, 4);
      for (const auto& [address, length, status] : std::vector<std::array<uint32_t, 3>>{
        {decommitted + 1, 0, 0xc000000d}, {decommitted + 4096, 0, 0xc000000d},
        {decommitted + 0xf000, 0x2000, 0xc0000018}, {decommitted + 0xffff, 2, 0xc0000018},
        {decommitted, 0xffffffff, 0xc000000d}, {0xfffff000, 0x2000, 0xc000000d},
        {0, 4096, 0xc000009f}, {0x10000, 4096, 0xc000009f}, {0x80000000, 4096, 0xc000009f},
        {0xf0000000, 4096, 0xc000009f}
      }) {
        prepare(address, length, 0x4000);
        invoke(status, address, length);
        check(memory.CommittedPages(decommitted, 0x10000) == 16, "invalid decommit changed committed pages");
      }
      for (const uint32_t output : {decommitted + 0x3000, decommitted + 0xfffc}) {
        prepare(decommitted + 0x3000, 0xd000, 0x4000, output);
        const uint32_t address = decommitted + 0x3000;
        memory.Write({}, output, &address, 4);
        reject(ctx, base);
        prepare(decommitted + 0x3000, 0xd000, 0x4000, 0xd203f3dc, output);
        const uint32_t length = 0xd000;
        memory.Write({}, output, &length, 4);
        reject(ctx, base);
      }
      prepare(decommitted + 0x3000, 0xd000, 0x4000, 0xd203f3dc, 0xd203f3e0, decommitted + 0x4000);
      reject(ctx, base);
      prepare(decommitted + 0x3000, 0xd000, 0x4000);
      {
        auto lease = memory.Borrow();
        reject(ctx, base);
      }
      invoke(0, decommitted + 0x3000, 0xd000);
      const auto tailDecommitted = statistics();
      check(tailDecommitted[2] + 3 == initial[2] && tailDecommitted[3] == 0x3000 && tailDecommitted[4] == 0x10000,
        "actual 13 page tail decommit lost retained reservation or usage");
      query(decommitted, 0x1000, decommitted, 0x3000);
      query(decommitted + 0x3000, 0x2000, decommitted, 0xd000);
      fault([&] { memory.Read({}, decommitted + 0x3000, &dirty, 4); });
      prepare(decommitted + 0x3000, 0xd000, 0x4000);
      invoke(0, decommitted + 0x3000, 0xd000);
      check(statistics() == tailDecommitted, "repeated decommit was not idempotent");
      allocate(decommitted + 0x3000, 0x1000, 0x1000);
      memory.Read({}, decommitted + 0x3000, &dirty, 4);
      check(!dirty, "recommit exposed decommitted copybuffer contents");
      prepare(decommitted + 0x2fff, 2, 0x4000);
      invoke(0, decommitted + 0x2000, 0x2000);
      check(memory.CommittedPages(decommitted, 0x10000) == 2, "unaligned decommit did not cover both spanning pages");
      prepare(decommitted, 0, 0x4000);
      invoke(0, decommitted, 0x10000);
      query(decommitted, 0x2000, decommitted, 0x10000);
      check(statistics()[4] == 0x10000 && !memory.CommittedPages(decommitted, 0x10000), "whole decommit discarded reservation");
      prepare(decommitted, 0, 0x4000);
      invoke(0, decommitted, 0x10000);
      prepare(decommitted, 0);
      invoke(0, decommitted, 0x10000);
      check(statistics() == initial, "decommit then release corrupted accounting");
      std::atomic<bool> writerReady = false, readerReady = false, writerDone = false;
      auto writer = ctx;
      writer.fs_base += 4096;
      runtime.Launch(writer, base, [&](X86Context& request, uint8_t* backingBase) {
        writerReady.store(true);
        while (!readerReady.load()) { runtime.Checkpoint(request); std::this_thread::yield(); }
        for (uint32_t n = 0; n < 64; ++n) {
          request.esp = 0x20800;
          const std::array<uint32_t, 2> inputs{0x10000000, 4096};
          const std::array<uint32_t, 6> allocation{0x12345678, 0x20900, 0, 0x20904, 0x3000, 4};
          memory.Write({}, 0x20900, inputs.data(), sizeof(inputs));
          memory.Write({}, request.esp, allocation.data(), sizeof(allocation));
          runtime.Invoke(request, backingBase, runtime.KernelTarget(184));
          check(request.eax == 0, "concurrent release fixture allocation failed");
          std::this_thread::yield();
          request.esp = 0x20800;
          const std::array<uint32_t, 4> decommit{0x371c18, 0x20900, 0x20904, 0x4000};
          memory.Write({}, 0x20900, inputs.data(), sizeof(inputs));
          memory.Write({}, request.esp, decommit.data(), sizeof(decommit));
          runtime.Invoke(request, backingBase, target);
          check(request.eax == 0, "concurrent decommit failed");
          std::this_thread::yield();
          request.esp = 0x20800;
          const std::array<uint32_t, 2> releaseInputs{0x10000000, 0};
          const std::array<uint32_t, 4> release{0x371c18, 0x20900, 0x20904, 0x8000};
          memory.Write({}, 0x20900, releaseInputs.data(), sizeof(releaseInputs));
          memory.Write({}, request.esp, release.data(), sizeof(release));
          const auto previous = request;
          runtime.Invoke(request, backingBase, target);
          auto expected = previous;
          expected.eax = 0;
          expected.esp += 16;
          expected.last_icall = target;
          check(std::memcmp(&request, &expected, sizeof(request)) == 0, "concurrent release changed unrelated state");
          std::array<uint32_t, 2> result{};
          memory.Read({}, 0x20900, result.data(), sizeof(result));
          check(result == inputs, "concurrent release published wrong freed range");
          std::this_thread::yield();
        }
        writerDone.store(true);
      });
      while (!writerReady.load()) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
      readerReady.store(true);
      for (uint32_t n = 0; n < 1000; ++n) {
        const auto usage = statistics();
        auto expected = initial;
        if (usage[3]) {
          --expected[2];
          expected[3] = 4096;
        }
        if (usage[4]) { expected[4] = 4096; }
        check(usage == expected, "statistics observed torn concurrent release ledger and page state");
        ctx.esp = 0x20000;
        const std::array<uint32_t, 3> frame{0x12345678, 0x10000000, 0x20400};
        memory.Write({}, ctx.esp, frame.data(), sizeof(frame));
        runtime.Invoke(ctx, base, runtime.KernelTarget(217));
        std::array<uint32_t, 7> result{};
        memory.Read({}, 0x20400, result.data(), sizeof(result));
        check(result == std::array<uint32_t, 7>{0x10000000, 0, 0, 0x1000000, 0x10000, 1, 0} ||
          result == std::array<uint32_t, 7>{0x10000000, 0x10000000, 4, 4096, 0x1000, 4, 0x20000} ||
          result == std::array<uint32_t, 7>{0x10000000, 0x10000000, 4, 4096, 0x2000, 0, 0x20000},
          "query observed torn concurrent release ledger and page state");
      }
      while (!writerDone.load()) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
      check(statistics() == initial, "concurrent release retained pages or reservations");
      const uint32_t raced = allocate(0, 4096, 0x3000);
      std::atomic<bool> raceReady = false, raceStart = false, raceDone = false;
      uint32_t writerStatus = 0xffffffff;
      writer = ctx;
      writer.fs_base += 4096;
      runtime.Launch(writer, base, [&](X86Context& request, uint8_t* backingBase) {
        request.esp = 0x20800;
        const std::array<uint32_t, 2> inputs{raced, 0};
        const std::array<uint32_t, 4> frame{0x371c18, 0x20900, 0x20904, 0x8000};
        memory.Write({}, 0x20900, inputs.data(), sizeof(inputs));
        memory.Write({}, request.esp, frame.data(), sizeof(frame));
        raceReady.store(true);
        while (!raceStart.load()) { runtime.Checkpoint(request); std::this_thread::yield(); }
        runtime.Invoke(request, backingBase, target);
        writerStatus = request.eax;
        raceDone.store(true);
      });
      while (!raceReady.load()) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
      prepare(raced, 0);
      raceStart.store(true);
      runtime.Invoke(ctx, base, target);
      const uint32_t readerStatus = ctx.eax;
      while (!raceDone.load()) { runtime.Checkpoint(ctx); std::this_thread::yield(); }
      check((!readerStatus && writerStatus == 0xc000009f) || (!writerStatus && readerStatus == 0xc000009f),
        "concurrent double release succeeded twice or lost the owned reservation");
      check(statistics() == initial && !memory.IsCommitted(raced), "concurrent double release corrupted accounting");
      check(memory.BackingBytes() == backing, "release or reuse grew bounded backing");
      const uint32_t cancelled = allocate(0, 0x1000, 0x3000);
      prepare(cancelled, 0);
      runtime.Cancel();
      reject(ctx, base);
      check(memory.IsCommitted(cancelled), "cancelled release mutated pages");
      prepare(cancelled, 4096, 0x4000);
      reject(ctx, base);
      check(memory.IsCommitted(cancelled), "cancelled decommit mutated pages");
    });
    if (auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(runtime.Stop() == nullptr && runtime.ActiveWorkers() == 0, "release retained workers");
  }
}

int main() {
  ReleaseContracts();
  QueryContracts();
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime runtime({}, {});
    browser::Memory memory;
    memory.Map(0x10000, 0x3000);
    memory.Commit(0x10000, 0x2000);
    browser::InstallVirtualMemory(runtime, memory);
    check(runtime.KernelTarget(184), "virtual allocation export absent");
    X86Context context{};
    context.host = &runtime;
    context.esp = 0x10000;
    context.eax = 0xabcdef01;
    context.ebx = 0x98765432;
    context.cf = 1;
    const auto call = [&](uint32_t base, uint32_t size, uint32_t type, uint32_t protect = 4, uint32_t zeroBits = 0, uint32_t baseOutput = 0x10100, uint32_t sizeOutput = 0x10104) {
      memory.Write({}, 0x10100, &base, 4);
      memory.Write({}, 0x10104, &size, 4);
      const std::array<uint32_t, 6> frame{0xfffffff0, baseOutput, zeroBits, sizeOutput, type, protect};
      memory.Write({}, context.esp, frame.data(), sizeof(frame));
      auto request = context;
      runtime.Invoke(request, memory.Base(), runtime.KernelTarget(184));
      check(request.esp == context.esp + 24 && request.ebx == context.ebx && request.cf == 1, "virtual allocation clobbered stdcall state");
      std::array<uint32_t, 6> after{};
      memory.Read({}, context.esp, after.data(), sizeof(after));
      check(after == frame, "virtual allocation changed input frame");
      memory.Read({}, 0x10100, &base, 4);
      memory.Read({}, 0x10104, &size, 4);
      return std::array<uint32_t, 3>{request.eax, base, size};
    };
    const size_t backing = memory.BackingBytes();
    const auto reserved = call(0, 0x10001, 0x2000);
    check(reserved[0] == 0 && reserved[1] % 0x10000 == 0 && reserved[2] == 0x11000, "reservation rounding incorrect");
    check(!memory.IsCommitted(reserved[1]), "reserve committed pages");
    uint32_t value = 0xa5a55a5a;
    fault([&] { memory.Read({}, reserved[1], &value, 4); });
    const auto committed = call(reserved[1] + 0x1003, 0x1000, 0x1000);
    check(committed == std::array<uint32_t, 3>{0, reserved[1] + 0x1000, 0x2000}, "commit rounding incorrect");
    check(!memory.IsCommitted(reserved[1]) && memory.IsCommitted(reserved[1] + 0x1000) && !memory.IsCommitted(reserved[1] + 0x3000), "commit changed adjacent pages");
    memory.Read({}, committed[1], &value, 4);
    check(value == 0, "fresh commit was not zeroed");
    value = 0xa5a55a5a;
    memory.Write({}, committed[1], &value, 4);
    check(call(committed[1], 1, 0x1000)[0] == 0, "recommit failed");
    memory.Read({}, committed[1], &value, 4);
    check(value == 0xa5a55a5a, "recommit erased live contents");
    check(call(reserved[1], 1, 0x2000)[0] == 0xc0000018, "overlapping reservation accepted");
    check(call(reserved[1] + 0x10000, 0x2000, 0x1000)[0] == 0xc0000018, "cross reservation commit accepted");
    check(call(0x70000000, 1, 0x1000)[0] == 0xc0000018, "unowned commit did not return conflicting addresses");
    check(call(0, 0, 0x3000) == std::array<uint32_t, 3>{0xc000000d, 0, 0}, "zero size published outputs");
    check(call(0xfffff000, 0x2000, 0x3000)[0] == 0xc000000d, "overflow accepted");
    for (const uint32_t type : {0u, 0x4000u, 0x103000u}) { fault([&] { call(0, 0x1000, type); }); }
    for (const uint32_t protect : {0u, 1u, 2u, 0x104u, 0x40u}) { fault([&] { call(0, 0x1000, 0x3000, protect); }); }
    fault([&] { call(0, 0x1000, 0x3000, 4, 1); });
    fault([&] { call(0, 0x1000, 0x2000, 2); });
    fault([&] { call(reserved[1], 0x1000, 0x1000, 1); });
    for (const uint32_t output : {0u, 0x12000u, 0x10101u, 0xfffffffcu, 0x10000u, 0x10104u}) {
      fault([&] { call(0, 0x1000, 0x3000, 4, 0, output); });
    }
    {
      auto lease = memory.Borrow();
      fault([&] { call(0, 0x1000, 0x3000); });
    }
    const auto combined = call(0, 0x1000, 0x3000);
    check(combined[0] == 0 && combined[1] == reserved[1] + 0x20000 && memory.IsCommitted(combined[1]), "failed requests consumed address space or reserve and commit failed");
    const auto implicit = call(0, 1, 0x1000);
    check(implicit[0] == 0 && memory.IsCommitted(implicit[1]) && implicit[1] != combined[1], "null base commit did not reserve");
    for (const uint32_t stack : {0x11ff0u, 0x12000u, 0xfffffff0u}) {
      auto invalid = context;
      invalid.esp = stack;
      fault([&] { runtime.Invoke(invalid, memory.Base(), runtime.KernelTarget(184)); });
      check(invalid.esp == stack && invalid.eax == context.eax, "invalid stack published result");
    }
    auto foreign = context;
    browser::Runtime other({}, {});
    foreign.host = &other;
    fault([&] { runtime.Invoke(foreign, memory.Base(), runtime.KernelTarget(184)); });
    fault([&] { runtime.Invoke(context, nullptr, runtime.KernelTarget(184)); });
    std::array<uint32_t, 2> concurrent{};
    const auto allocate = [&](uint32_t index) {
      auto request = context;
      request.esp = 0x10400 + index * 0x100;
      const uint32_t output = request.esp + 0x40;
      const uint32_t sizeOutput = output + 4;
      const std::array<uint32_t, 2> inputs{0, 0x1000};
      const std::array<uint32_t, 6> frame{0xfffffff0, output, 0, sizeOutput, 0x3000, 4};
      memory.Write({}, output, inputs.data(), sizeof(inputs));
      memory.Write({}, request.esp, frame.data(), sizeof(frame));
      runtime.Invoke(request, memory.Base(), runtime.KernelTarget(184));
      check(request.eax == 0, "concurrent allocation failed");
      memory.Read({}, output, &concurrent[index], 4);
    };
    std::thread first([&] { allocate(0); });
    std::thread second([&] { allocate(1); });
    first.join();
    second.join();
    check(concurrent[0] != concurrent[1] && memory.IsCommitted(concurrent[0]) && memory.IsCommitted(concurrent[1]), "concurrent allocation overlapped or lost commit");
    const auto full = call(0, 16u << 20, 0x2000);
    check(full[0] == 0xc0000017 && full[1] == 0 && full[2] == (16u << 20), "exhaustion published partial success");
    check(memory.BackingBytes() == backing, "bounded arena grew backing");
    runtime.Cancel();
    fault([&] { call(0, 0x1000, 0x3000); });
    check(runtime.Stop() == nullptr && runtime.ActiveWorkers() == 0, "virtual allocation teardown retained workers");
  }
  std::puts("virtual-memory-contracts-ok");
}
