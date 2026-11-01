#include "browser/contiguous-memory.h"
#include <array>
#include <atomic>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
const auto fault = [](auto operation) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, "invalid contiguous allocation operation accepted");
};

const auto owned = [](browser::Runtime& runtime, browser::Memory& memory, X86Context context, auto operation) {
  std::exception_ptr failure;
  runtime.Launch(context, memory.Base(), [&](X86Context& current, uint8_t*) {
    try { operation(current); } catch (...) { failure = std::current_exception(); }
  });
  check(!runtime.Join() && runtime.ActiveWorkers() == 0, "owned contiguous fixture failed to join");
  if (failure) { std::rethrow_exception(failure); }
};

int main() {
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Memory memory;
    browser::Runtime runtime({}, {});
    browser::QueryContiguousMemoryUsage usage;
    browser::ContiguousMemoryAccess access;
    const auto snapshot = browser::InstallContiguousMemory(runtime, memory, &usage, &access);
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    X86Context context{};
    context.host = &runtime;
    context.esp = 0x80001000;
    context.eax = 0xabcdef01;
    context.ebx = 0x12345678;
    fault([&] { access.allocate(context, 1, 0); });
    uint32_t allocation = 0;
    owned(runtime, memory, context, [&](X86Context& current) {
      const auto before = current;
      allocation = access.allocate(current, 4097, 0x2000);
      check(allocation == 0x80100000 && usage(current, memory) == 2, "SDK allocation did not share contiguous ledger");
      check(!std::memcmp(&current, &before, sizeof(before)), "SDK allocator mutated guest call context");
      uint32_t value = 1;
      memory.Read(current, allocation + 0x70000000, &value, sizeof(value));
      check(value == 0, "SDK allocation lacks zeroed physical alias backing");
      const std::array<uint32_t, 4> frame{0x1234, allocation, 8192, 1};
      memory.Write(current, current.esp, frame.data(), sizeof(frame));
      runtime.Invoke(current, memory.Base(), runtime.KernelTarget(178));
      current = before;
      check(access.allocate(current, UINT32_MAX, 0) == 0 && access.allocate(current, 1, 3) == 0 && usage(current, memory) == 2,
        "SDK allocation failure consumed memory");
      auto alias = current;
      alias.fs_base = current.fs_base + 4096;
      fault([&] { access.allocate(alias, 1, 0); });
      fault([&] { access.release(alias, allocation); });
      {
        auto lease = memory.Borrow();
        fault([&] { access.allocate(current, 1, 0); });
        fault([&] { access.release(current, allocation); });
      }
      fault([&] { access.release(current, allocation + 4096); });
      check(usage(current, memory) == 2, "invalid SDK free changed ownership");
    });
    check(snapshot().size() == 1, "SDK allocation cannot persist through kernel API");
    owned(runtime, memory, context, [&](X86Context& current) {
      access.release(current, allocation);
      check(usage(current, memory) == 0, "SDK free did not update kernel ledger");
      fault([&] { access.release(current, allocation); });
      const std::array<uint32_t, 2> frame{0x1234, 1};
      memory.Write(current, current.esp, frame.data(), sizeof(frame));
      runtime.Invoke(current, memory.Base(), runtime.KernelTarget(165));
      check(current.eax == allocation && usage(current, memory) == 1, "kernel cannot reuse SDK released allocation");
      access.release(current, current.eax);
      check(usage(current, memory) == 0, "SDK cannot release kernel allocation");
    });
    check(snapshot().empty(), "SDK free retained persistence mark");
    owned(runtime, memory, context, [&](X86Context& current) {
      runtime.Cancel();
      fault([&] { access.allocate(current, 1, 0); });
      fault([&] { access.release(current, allocation); });
    });
    check(!runtime.Stop(), "SDK allocation teardown retained workers");
  }
  {
    browser::Memory memory;
    browser::Runtime runtime({}, {});
    browser::QueryContiguousMemoryUsage usage;
    const auto snapshot = browser::InstallContiguousMemory(runtime, memory, &usage);
    check(runtime.KernelTarget(166), "extended contiguous export absent");
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    X86Context context{};
    context.host = &runtime;
    context.esp = 0x80001000;
    context.eax = 0x12345678;
    context.ebx = 0xabcdef01;
    context.cf = 1;
    const auto invoke = [&](uint32_t ordinal, const std::array<uint32_t, 6>& frame) {
      memory.Write({}, context.esp, frame.data(), sizeof(frame));
      auto result = context;
      owned(runtime, memory, context, [&](X86Context& current) {
        runtime.Invoke(current, memory.Base(), runtime.KernelTarget(ordinal));
        result = current;
      });
      check(result.esp == context.esp + (ordinal == 166 ? 24 : ordinal == 178 ? 16 : 8) &&
        result.ebx == context.ebx && result.cf == context.cf, "extended allocation ABI changed guest state");
      std::array<uint32_t, 6> after{};
      memory.Read({}, context.esp, after.data(), sizeof(after));
      check(after == frame, "extended allocation changed frame");
      return result.eax;
    };
    const auto allocate = [&](uint32_t size, uint32_t low, uint32_t high, uint32_t alignment) {
      return invoke(166, {0xfffffff0, size, low, high, alignment, 4});
    };
    const auto release = [&](uint32_t address) { invoke(171, {0xfffffff0, address, 0, 0, 0, 0}); };
    const auto pages = [&] {
      uint32_t count = 0;
      owned(runtime, memory, context, [&](X86Context& current) { count = usage(current, memory); });
      return count;
    };
    check(allocate(1, 0x100001, 0x101fff, 0) == 0x80101000, "misaligned low bound incorrect");
    check(allocate(1, 0x100000, 0x100ffe, 1) == 0, "inclusive high bound ignored");
    const uint32_t first = allocate(0, 0, 0x100fff, 256);
    check(first == 0x80100000 && pages() == 2, "shared first pages or zero allocation incorrect");
    check(allocate(1, 0x102000, 0x102fff, 0x2000) == 0x80102000, "physical alignment incorrect");
    check(allocate(1, 0x4000000, UINT32_MAX, 0) == 0 && allocate(UINT32_MAX, 0, UINT32_MAX, 0) == 0 &&
      allocate(1, 0, UINT32_MAX, 3) == 0 && allocate(1, 0x300000, 0x2fffff, 0) == 0,
      "invalid range or alignment accepted");
    check(allocate(1, 0x3fff000, UINT32_MAX, 0) == 0x83fff000, "last physical page rejected");
    check(pages() == 4, "failed requests changed shared usage");
    invoke(178, {0xfffffff0, first, 0x1000, 1, 0, 0});
    check(snapshot().size() == 1, "extended allocation cannot persist");
    release(first);
    check(snapshot().empty() && invoke(165, {0xfffffff0, 1, 0, 0, 0, 0}) == first, "shared free or reuse failed");
    release(0x80101000);
    check(allocate(1, 0x100000, 0x101fff, 0) == 0x80101000, "fragmented first fit failed");
    owned(runtime, memory, context, [&](X86Context& current) {
      auto lease = memory.Borrow();
      fault([&] { runtime.Invoke(current, memory.Base(), runtime.KernelTarget(166)); });
    });
    const uint32_t combined = invoke(166, {0xfffffff0, 1, 0, UINT32_MAX, 0, 0x404});
    check(combined == 0x80103000 && pages() == 5, "write combined allocation did not share ledger");
    uint32_t marker = 0x12345678;
    memory.Write({}, combined, &marker, sizeof(marker));
    uint32_t value = 0;
    memory.Read({}, combined + 0x70000000, &value, sizeof(value));
    check(value == marker, "write combined allocation lost physical alias");
    release(combined);
    const std::array<uint32_t, 6> unsupported{0xfffffff0, 1, 0, UINT32_MAX, 0, 0x204};
    memory.Write({}, context.esp, unsupported.data(), sizeof(unsupported));
    owned(runtime, memory, context, [&](X86Context& current) {
      auto foreign = current;
      foreign.fs_base = 1;
      fault([&] { runtime.Invoke(foreign, memory.Base(), runtime.KernelTarget(166)); });
      fault([&] { runtime.Invoke(current, nullptr, runtime.KernelTarget(166)); });
      fault([&] { runtime.Invoke(current, memory.Base(), runtime.KernelTarget(166)); });
      check(current.esp == context.esp && current.eax == context.eax && current.ebx == context.ebx && current.cf == context.cf,
        "unsupported protection changed guest state");
    });
    check(pages() == 4, "rejected protection changed usage");
    auto invalid = context;
    invalid.esp = 0x83fffff0;
    owned(runtime, memory, context, [&](X86Context&) {
      fault([&] { runtime.Invoke(invalid, memory.Base(), runtime.KernelTarget(166)); });
    });
    check(invalid.esp == 0x83fffff0 && invalid.eax == context.eax && pages() == 4, "invalid frame changed state");
    runtime.Cancel();
    fault([&] { allocate(1, 0, UINT32_MAX, 0); });
    check(!runtime.Stop(), "extended allocation cancellation retained workers");
  }
  {
    browser::Memory memory;
    browser::Runtime runtime({}, {});
    const auto snapshot = browser::InstallContiguousMemory(runtime, memory);
    check(runtime.KernelTarget(178) && !runtime.KernelTarget(49), "persistence absent or firmware reboot fabricated");
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    X86Context context{};
    context.host = &runtime;
    context.esp = 0x80001000;
    context.eax = 0x12345678;
    context.ebx = 0xabcdef01;
    context.cf = 1;
    const auto invoke = [&](uint32_t ordinal, const std::array<uint32_t, 4>& frame) {
      memory.Write({}, context.esp, frame.data(), sizeof(frame));
      auto result = context;
      owned(runtime, memory, context, [&](X86Context& current) {
        runtime.Invoke(current, memory.Base(), runtime.KernelTarget(ordinal));
        result = current;
      });
      return result;
    };
    const uint32_t first = invoke(165, {0xfffffff0, 0x1000, 0, 0}).eax;
    const uint32_t second = invoke(165, {0xfffffff0, 0x2000, 0, 0}).eax;
    const auto mark = [&](uint32_t address, uint32_t size, uint32_t persist) {
      const std::array<uint32_t, 4> frame{0xfffffff0, address, size, persist};
      const auto result = invoke(178, frame);
      check(result.esp == context.esp + 16 && result.eax == context.eax && result.ebx == context.ebx && result.cf == context.cf,
        "void persistence stdcall clobbered guest state");
      std::array<uint32_t, 4> after{};
      memory.Read({}, context.esp, after.data(), sizeof(after));
      check(after == frame, "persistence frame changed");
    };
    check(snapshot().empty(), "fresh allocation unexpectedly persistent");
    mark(first, 0x1000, 1);
    mark(first, 0x1000, 1);
    mark(second, 0x2000, 0x100);
    check(snapshot().size() == 1, "BOOLEAN high bytes persisted another allocation");
    uint32_t value = 0x98765432;
    memory.Write({}, first + 0x70000000, &value, 4);
    auto exported = snapshot();
    check(exported.size() == 1 && exported[0].address == first && exported[0].bytes.size() == 0x1000,
      "persistence export lost exact allocation identity");
    uint32_t saved = 0;
    std::memcpy(&saved, exported[0].bytes.data(), sizeof(saved));
    check(saved == value, "persistence snapshotted before postmark guest writes");
    value = 0x11223344;
    memory.Write({}, first, &value, 4);
    std::memcpy(&saved, exported[0].bytes.data(), sizeof(saved));
    check(saved != value, "exported bytes alias mutable guest storage");
    for (const auto frame : std::array<std::array<uint32_t, 4>, 8>{{
      {0xfffffff0, first + 1, 0x1000, 1}, {0xfffffff0, first + 0x70000000, 0x1000, 1},
      {0xfffffff0, first, 0, 1}, {0xfffffff0, first, 0x2000, 0},
      {0xfffffff0, second + 0x1000, 0x1000, 1}, {0xfffffff0, second, 0x1000, 1},
      {0xfffffff0, first, UINT32_MAX, 1}, {0xfffffff0, 0x80000000, 0x1000, 1}}}) {
      memory.Write({}, context.esp, frame.data(), sizeof(frame));
      owned(runtime, memory, context, [&](X86Context& current) {
        fault([&] { runtime.Invoke(current, memory.Base(), runtime.KernelTarget(178)); });
        check(current.esp == context.esp && current.eax == context.eax && current.cf == context.cf,
          "invalid persistence range changed guest state");
      });
      check(snapshot().size() == 1, "invalid persistence range mutated marks");
    }
    for (const uint32_t stack : {0x83fffff8u, 0xfffffff8u}) {
      auto invalid = context;
      invalid.esp = stack;
      owned(runtime, memory, context, [&](X86Context&) {
        fault([&] { runtime.Invoke(invalid, memory.Base(), runtime.KernelTarget(178)); });
      });
      check(invalid.esp == stack && invalid.eax == context.eax && snapshot().size() == 1,
        "invalid persistence frame published mutation");
    }
    owned(runtime, memory, context, [&](X86Context& current) {
      fault([&] { snapshot(); });
      auto foreign = current;
      foreign.fs_base = 1;
      fault([&] { runtime.Invoke(foreign, memory.Base(), runtime.KernelTarget(178)); });
      auto lease = memory.Borrow();
      fault([&] { runtime.Invoke(current, memory.Base(), runtime.KernelTarget(178)); });
    });
    mark(first, 0x1000, 0);
    mark(first, 0x1000, 0);
    check(snapshot().empty(), "unmark did not release retained range");
    mark(second, 0x2000, 0x101);
    mark(first, 0x1000, 1);
    check(snapshot().size() == 2, "distinct persistent allocations merged");
    runtime.Launch(context, memory.Base(), [&](X86Context& parent, uint8_t*) {
      auto child = parent;
      child.esp += 0x100;
      child.fs_base = 1;
      runtime.Launch(child, memory.Base(), [&](X86Context& current, uint8_t*) {
        const std::array<uint32_t, 4> frame{0xfffffff0, first, 0x1000, 0};
        memory.Write(current, current.esp, frame.data(), sizeof(frame));
        runtime.Invoke(current, memory.Base(), runtime.KernelTarget(178));
      });
      const std::array<uint32_t, 4> frame{0xfffffff0, second, 0x2000, 0};
      memory.Write(parent, parent.esp, frame.data(), sizeof(frame));
      runtime.Invoke(parent, memory.Base(), runtime.KernelTarget(178));
    });
    check(!runtime.Join() && snapshot().empty(), "descendant persistence metadata did not share session ownership");
    mark(first, 0x1000, 1);
    mark(second, 0x2000, 1);
    {
      auto lease = memory.Borrow();
      fault([&] { snapshot(); });
    }
    invoke(171, {0xfffffff0, first, 0, 0});
    check(snapshot().size() == 1 && snapshot()[0].address == second, "free retained persistence authority");
    fault([&] { mark(first, 0x1000, 1); });
    check(invoke(165, {0xfffffff0, 0x1000, 0, 0}).eax == first && snapshot().size() == 1,
      "reused allocation inherited persistence");
    browser::Memory otherMemory;
    browser::Runtime otherRuntime({}, {});
    const auto otherSnapshot = browser::InstallContiguousMemory(otherRuntime, otherMemory);
    check(otherSnapshot().empty(), "another session inherited persistence");
    runtime.Cancel();
    fault([&] { mark(second, 0x2000, 0); });
    fault([&] { snapshot(); });
    check(!runtime.Stop() && !runtime.ActiveWorkers(), "persistence cancellation retained worker");
    check(otherSnapshot().empty(), "cancellation crossed session boundary");
  }
  {
    browser::Memory firstMemory;
    browser::Memory secondMemory;
    browser::Runtime firstRuntime({}, {});
    browser::Runtime secondRuntime({}, {});
    browser::InstallContiguousMemory(firstRuntime, firstMemory);
    browser::InstallContiguousMemory(secondRuntime, secondMemory);
    firstRuntime.PublishImageMemory(firstMemory, firstRuntime.KernelRevision(), [] {});
    secondRuntime.PublishImageMemory(secondMemory, secondRuntime.KernelRevision(), [] {});
    const auto allocate = [](browser::Runtime& runtime, browser::Memory& memory) {
      X86Context context{};
      context.host = &runtime;
      context.esp = 0x80001000;
      const std::array<uint32_t, 2> frame{0xfffffff0, 0x1000};
      memory.Write({}, context.esp, frame.data(), sizeof(frame));
      owned(runtime, memory, context, [&](X86Context& current) {
        runtime.Invoke(current, memory.Base(), runtime.KernelTarget(165));
        context = current;
      });
      return context.eax;
    };
    const uint32_t first = allocate(firstRuntime, firstMemory);
    const uint32_t second = allocate(secondRuntime, secondMemory);
    check(first == second && first == 0x80100000, "fresh simultaneous sessions shared allocator metadata");
    uint32_t value = 0x11223344;
    firstMemory.Write({}, first, &value, 4);
    secondMemory.Read({}, second + 0x70000000, &value, 4);
    check(value == 0, "simultaneous physical backing leaked between sessions");
    X86Context foreign{};
    foreign.host = &firstRuntime;
    foreign.esp = 0x80001000;
    fault([&] { firstRuntime.Invoke(foreign, secondMemory.Base(), firstRuntime.KernelTarget(165)); });
    check(foreign.esp == 0x80001000, "foreign backing changed frame");
  }
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Memory memory;
    memory.Map(0x10000, 0x3000);
    memory.Commit(0x10000, 0x2000);
    browser::Runtime runtime({}, {});
    const size_t backing = memory.BackingBytes();
    browser::InstallContiguousMemory(runtime, memory);
    check(runtime.KernelTarget(165) && runtime.KernelTarget(171), "contiguous exports absent");
    fault([&] { browser::InstallContiguousMemory(runtime, memory); });
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    X86Context context{};
    context.host = &runtime;
    context.esp = 0x10000;
    context.eax = 0x12345678;
    context.ebx = 0xabcdef01;
    context.cf = 1;
    context.fs_base = 0x20000;
    fault([&] { runtime.Invoke(context, memory.Base(), runtime.KernelTarget(165)); });
    owned(runtime, memory, context, [&](X86Context& current) {
      auto forged = current;
      forged.fs_base += 0x1000;
      fault([&] { runtime.Invoke(forged, memory.Base(), runtime.KernelTarget(165)); });
    });
    const auto invoke = [&](uint32_t ordinal, uint32_t argument, X86Context request) {
      const std::array<uint32_t, 2> frame{0xfffffff0, argument};
      memory.Write({}, request.esp, frame.data(), sizeof(frame));
      const uint32_t stack = request.esp;
      if (runtime.OwnsCurrentContext(request)) {
        runtime.Invoke(request, memory.Base(), runtime.KernelTarget(ordinal));
      } else {
        owned(runtime, memory, request, [&](X86Context& current) {
          runtime.Invoke(current, memory.Base(), runtime.KernelTarget(ordinal));
          request = current;
        });
      }
      check(request.esp == stack + 8 && request.ebx == context.ebx && request.cf == 1, "contiguous stdcall state clobbered");
      std::array<uint32_t, 2> after{};
      memory.Read({}, stack, after.data(), sizeof(after));
      check(after == frame, "contiguous input frame changed");
      return request.eax;
    };
    const auto allocate = [&](uint32_t size) { return invoke(165, size, context); };
    const auto release = [&](uint32_t address) { check(invoke(171, address, context) == context.eax, "void free changed EAX"); };
    uint32_t marker = 0x98765432;
    memory.Write({}, 0x800ffffc, &marker, 4);
    const uint32_t first = allocate(0x1000);
    check(first == 0x80100000, "first physical megabyte not reserved");
    const uint32_t second = allocate(0x1001);
    check(second == first + 0x1000, "page rounded contiguous allocation incorrect");
    uint32_t value = 1;
    memory.Read({}, second + 0x1ffc, &value, 4);
    check(value == 0, "rounded tail not backed and zeroed");
    memory.Read({}, 0xf00ffffc, &value, 4);
    check(value == marker, "allocator touched reserved physical memory");
    memory.Write({}, first, &marker, 4);
    memory.Read({}, 0xf0000000 + (first - 0x80000000), &value, 4);
    check(value == marker, "cached write missing from physical alias");
    value = 0xa5a55a5a;
    memory.Write({}, 0xf0000000 + (second - 0x80000000) + 0x1ffc, &value, 4);
    memory.Read({}, second + 0x1ffc, &marker, 4);
    check(value == marker, "write combine alias missing from allocation");
    owned(runtime, memory, context, [&](X86Context&) {
      auto lease = memory.Borrow();
      check(memory.Span({}, first, 0x1000) == memory.Span({}, first + 0x70000000, 0x1000), "physical aliases have separate backing");
      fault([&] { allocate(1); });
      fault([&] { release(first); });
    });
    for (const uint32_t address : {0u, 0x80000000u, first + 1, first + 0x70000000, second + 0x1000, 0x10000u}) {
      fault([&] { release(address); });
    }
    release(first);
    fault([&] { release(first); });
    check(memory.IsCommitted(first) && memory.IsCommitted(first + 0x70000000), "free changed shared physical mapping contract");
    check(allocate(0) == first, "zero request did not use desktop one page contract or free was not reclaimed");
    memory.Read({}, first + 0x70000000, &value, 4);
    check(value == 0, "reallocated page retained old alias bytes");
    memory.Read({}, second + 0x1ffc, &value, 4);
    check(value == marker, "free or reallocation erased adjacent allocation");
    check(allocate(UINT32_MAX) == 0 && allocate(0x4000001) == 0, "overflow or oversized request succeeded");
    for (const uint32_t stack : {0x11ffcu, 0x12000u, 0xfffffffcu}) {
      auto invalid = context;
      invalid.esp = stack;
      owned(runtime, memory, context, [&](X86Context&) {
        fault([&] { runtime.Invoke(invalid, memory.Base(), runtime.KernelTarget(165)); });
      });
      check(invalid.eax == context.eax && invalid.esp == stack, "invalid frame published success");
    }
    browser::Runtime other({}, {});
    other.RegisterKernel(171, [](X86Context&, uint8_t*) {});
    fault([&] { browser::InstallContiguousMemory(other, memory); });
    check(!other.KernelTarget(165), "conflicting free export partially installed allocator");
    browser::Runtime persistentConflict({}, {});
    persistentConflict.RegisterKernel(178, [](X86Context&, uint8_t*) {});
    fault([&] { browser::InstallContiguousMemory(persistentConflict, memory); });
    check(!persistentConflict.KernelTarget(165) && !persistentConflict.KernelTarget(171),
      "conflicting persistence export partially installed allocator");
    auto foreign = context;
    foreign.host = &other;
    fault([&] { runtime.Invoke(foreign, memory.Base(), runtime.KernelTarget(165)); });
    fault([&] { runtime.Invoke(context, nullptr, runtime.KernelTarget(165)); });
    release(first);
    release(second);
    std::array<uint32_t, 2> concurrent{};
    std::atomic<uint32_t> ready = 0;
    const auto worker = [&](uint32_t index) {
      auto request = context;
      request.esp = 0x10400 + index * 0x100;
      request.fs_base = index + 1;
      runtime.Launch(request, memory.Base(), [&, index](X86Context& current, uint8_t*) {
        concurrent[index] = invoke(165, 0x2000, current);
        ready.fetch_add(1);
        while (ready.load() < 2) { std::this_thread::yield(); }
        const uint32_t byte = index + 1;
        memory.Write(current, concurrent[index], &byte, 4);
      });
    };
    worker(0);
    worker(1);
    check(!runtime.Join() && runtime.ActiveWorkers() == 0, "contiguous workers failed or outlived Join");
    browser::Runtime live({}, {});
    std::atomic<bool> leave = false;
    live.Launch({}, memory.Base(), [&](X86Context&, uint8_t*) {
      while (!leave.load()) { std::this_thread::yield(); }
    });
    fault([&] { browser::InstallContiguousMemory(live, memory); });
    leave.store(true);
    check(!live.Join() && !live.KernelTarget(165), "live runtime accepted allocator installation");
    check(concurrent[0] && concurrent[1] && concurrent[0] != concurrent[1], "concurrent allocations overlap");
    for (uint32_t index = 0; index < 2; ++index) {
      memory.Read({}, concurrent[index] + 0x70000000, &value, 4);
      check(value == index + 1, "concurrent physical allocation lost alias ownership");
      release(concurrent[index]);
    }
    const uint32_t full = allocate(63u << 20);
    check(full == first && allocate(1) == 0, "physical capacity exhaustion fabricated success");
    release(full);
    check(allocate((63u << 20) + 1) == 0, "allocator crossed physical extent");
    check(allocate(1) == first, "exhaustion consumed free space");
    check(memory.BackingBytes() == backing, "physical alias allocator grew backing");
    runtime.Cancel();
    fault([&] { allocate(1); });
    fault([&] { release(first); });
    check(!runtime.Stop() && runtime.ActiveWorkers() == 0, "contiguous teardown retained workers");
  }
  std::puts("contiguous-memory-contracts-ok");
}
