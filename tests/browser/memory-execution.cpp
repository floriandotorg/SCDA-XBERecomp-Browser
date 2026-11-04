#include "browser/cpu_runtime.h"
#include <atomic>
#include <cstdio>
#include <thread>

int main() {
  try {
    browser::Memory memory;
    memory.Ensure(0x10000, 8192);
    const auto check = [](bool valid) { if (!valid) { throw std::runtime_error("guest execution ownership contract changed"); } };
    const auto rejects = [&](auto operation) {
      bool rejected = false;
      try { operation(); } catch (const browser::Fault&) { rejected = true; }
      check(rejected);
    };
    X86Context context{};
    {
      browser::Memory::Execution execution(&memory);
      check(browser::Memory::HasBorrow());
      memory.Store<uint32_t>(context, 0x10000, 17);
      {
        browser::Memory::Execution nested(&memory);
        browser::Memory::BlockScope block(memory);
        check(block.Load<uint32_t>(context, 0x10000) == 17);
        block.Store<uint32_t>(context, 0xf0010000, 31);
        rejects([&] { browser::Memory::PauseExecution pause; });
      }
      check(browser::Memory::HasBorrow() && memory.Load<uint32_t>(context, 0x80010000) == 31);
      rejects([&] { memory.Decommit(0x10000, 4096); });
      std::atomic<bool> requested = false, finished = false;
      std::thread writer([&] {
        requested.store(true);
        memory.Store<uint32_t>(context, 0x10000, 37);
        finished.store(true);
      });
      while (!requested.load()) { std::this_thread::yield(); }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      check(!finished.load());
      {
        browser::Memory::PauseExecution pause;
        check(!browser::Memory::HasBorrow());
        writer.join();
        check(finished.load());
        memory.Decommit(0x11000, 4096);
        memory.Commit(0x11000, 4096);
        {
          browser::Memory::PauseExecution nested;
          check(!browser::Memory::HasBorrow());
        }
        check(!browser::Memory::HasBorrow());
        {
          browser::Memory::Execution callback(&memory);
          check(browser::Memory::HasBorrow());
          memory.Store<uint32_t>(context, 0x11000, 41);
        }
        check(!browser::Memory::HasBorrow());
      }
      check(browser::Memory::HasBorrow() && memory.Load<uint32_t>(context, 0x10000) == 37 && memory.Load<uint32_t>(context, 0x11000) == 41);
      try { browser::Memory::PauseExecution pause; throw 1; } catch (int) {}
      check(browser::Memory::HasBorrow());
      context.edi = 0x10000; context.ecx = 2048; context.eax = 43;
      memory.StoreRepeated(context, 4);
      check(!context.ecx && browser::Memory::HasBorrow() && memory.Load<uint32_t>(context, 0x11ffc) == 43);
      rejects([&] { memory.Load<uint64_t>(context, 0xfffffffc); });
    }
    check(!browser::Memory::HasBorrow());
    {
      auto lease = memory.Borrow();
      browser::Memory::PauseExecution pause;
      check(browser::Memory::HasBorrow());
      rejects([&] { browser::Memory::Execution execution(&memory); });
    }
    const std::array<X86FuncMapping, 2> mappings{{{0x11000, +[](X86Context& ctx, uint8_t* base) {
      if (!browser::Memory::HasBorrow()) { throw std::runtime_error("generated invocation has no execution protection"); }
      browser::Memory::From(base).Store<uint32_t>(ctx, 0x10000, 59);
    }}, {0x12000, +[](X86Context& ctx, uint8_t* base) {
      if (ctx.ecx) {
        --ctx.ecx;
        browser::Runtime::From(ctx).Invoke(ctx, base, 0x12000);
        return;
      }
      ctx.eax = 67;
    }}}};
    browser::Runtime runtime(mappings, {}, [] { return 1000000; }, 2);
    runtime.RegisterKernel(1, [&](X86Context& ctx, uint8_t*) {
      check(!browser::Memory::HasBorrow());
      memory.Ensure(0x20000, 4096);
      ctx.eax = 47;
    });
    runtime.InstallSdkObserver(memory, [&](X86Context&, uint32_t, uint64_t) {
      check(!browser::Memory::HasBorrow());
      memory.Decommit(0x20000, 4096);
      return false;
    });
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] {});
    const auto thunk = runtime.HostThunk(+[](X86Context& ctx, uint8_t*) {
      if (browser::Memory::HasBorrow()) { throw std::runtime_error("host thunk retained guest backing"); }
      ctx.edx = 53;
    });
    context.host = &runtime;
    runtime.Launch(context, memory.Base(), [&](X86Context& ctx, uint8_t* base) {
      runtime.Invoke(ctx, base, 0x11000);
      check(!browser::Memory::HasBorrow() && memory.Load<uint32_t>(ctx, 0x10000) == 59);
#ifdef __wasm_tail_call__
      ctx.ecx = 100000;
#else
      ctx.ecx = 128;
#endif
      runtime.Invoke(ctx, base, 0x12000);
      check(!ctx.ecx && ctx.eax == 67 && !browser::Memory::HasBorrow());
      browser::Memory::Execution execution(&memory);
      runtime.Kernel(ctx, base, 1);
      check(browser::Memory::HasBorrow() && ctx.eax == 47);
      browser::Runtime::ObserveSdkEntry(ctx, base, 1);
      check(browser::Memory::HasBorrow());
      runtime.Invoke(ctx, base, thunk);
      check(browser::Memory::HasBorrow() && ctx.edx == 53);
      const auto sequence = runtime.Sequence();
      runtime.Wait(ctx, sequence, std::chrono::milliseconds(1));
      check(browser::Memory::HasBorrow());
      runtime.Poll(ctx, base);
      check(browser::Memory::HasBorrow());
      {
        browser::Runtime unrelated({}, {});
      }
      check(browser::Memory::HasBorrow());
      std::atomic<bool> progressed = false;
      std::thread writer([&] {
        memory.Store<uint32_t>(ctx, 0x10000, 61);
        progressed.store(true);
      });
      for (uint32_t n = 0; n < 65536 && !progressed.load(); ++n) { runtime.Checkpoint(ctx); }
      const bool bounded = progressed.load();
      {
        browser::Memory::PauseExecution pause;
        writer.join();
      }
      check(bounded && memory.Load<uint32_t>(ctx, 0x10000) == 61);
    });
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
    check(!browser::Memory::HasBorrow());
    std::puts("memory-execution-contracts-ok");
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
