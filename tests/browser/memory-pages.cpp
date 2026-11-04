#include "browser/cpu_runtime.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <thread>

int main() {
  try {
    browser::Memory memory;
    X86Context context{};
    const auto check = [](bool valid) { if (!valid) { throw std::runtime_error("checked page translation changed guest memory"); } };
    const auto block = [&](auto operation) {
      memory.BeginBlock();
      try { operation(); } catch (...) { memory.EndBlock(); throw; }
      memory.EndBlock();
    };
    const auto rejects = [&](auto operation, const char* reason) {
      bool rejected = false;
      try { operation(); } catch (const browser::Fault& fault) { rejected = std::string(fault.what()).find(reason) != std::string::npos; }
      check(rejected);
    };
    memory.Map(0x10000, 8192);
    memory.Commit(0x10000, 8192);
    block([&] {
      memory.Store<uint32_t>(context, 0x10003, 0xdeadbeef);
      check(memory.Load<uint32_t>(context, 0x10003) == 0xdeadbeef);
      memory.Store<uint64_t>(context, 0x10ffc, 0x1234567887654321ull);
      check(memory.Load<uint64_t>(context, 0x10ffc) == 0x1234567887654321ull);
    });
    memory.Decommit(0x11000, 4096);
    block([&] {
      rejects([&] { memory.Load<uint32_t>(context, 0x11000); }, "uncommitted");
      rejects([&] { memory.Store<uint64_t>(context, 0x10ffc, 0); }, "uncommitted");
      check(memory.Load<uint32_t>(context, 0x10ffc) == 0x87654321);
      rejects([&] { memory.Load<uint64_t>(context, 0xfffffffc); }, "wraparound");
    });
    memory.Commit(0x11000, 4096);
    block([&] { check(memory.Load<uint32_t>(context, 0x11000) == 0); });
    memory.Discard(0x10000, 8192);
    block([&] { rejects([&] { memory.Load<uint32_t>(context, 0x10003); }, "unmapped"); });
    memory.Ensure(0x10000, 8192);
    block([&] { check(memory.Load<uint32_t>(context, 0x10003) == 0); });
    memory.Ensure(0x30000, 4096);
    block([&] {
      memory.Store<uint16_t>(context, 0x30001, 0xabcd);
      check(memory.Load<uint16_t>(context, 0x30001) == 0xabcd);
    });
    const auto before = memory.PhysicalWriteVersion(context, 0x80010000, 4096);
    const auto neighbor = memory.PhysicalWriteVersion(context, 0x80011000, 4096);
    block([&] {
      memory.Store<uint32_t>(context, 0x80010003, 1234);
      check(memory.Load<uint32_t>(context, 0xf0010003) == 1234);
    });
    const auto written = memory.PhysicalWriteVersion(context, 0xf0010000, 4096);
    check(written && before && *written > *before && memory.PhysicalWriteVersion(context, 0x80011000, 4096) == neighbor);
    block([&] { check(memory.Load<uint32_t>(context, 0x80010003) == 1234); });
    check(memory.PhysicalWriteVersion(context, 0xf0010000, 4096) == written);
    const std::array<uint32_t, 4> vector{1, 2, 3, 4};
    block([&] {
      memory.Store(context, 0xf0010ff8, vector);
      check(memory.Load<std::array<uint32_t, 4>>(context, 0x80010ff8) == vector);
    });
    check(memory.PhysicalWriteVersion(context, 0x80010000, 4096) == memory.PhysicalWriteVersion(context, 0xf0011000, 4096));
    {
      browser::Memory other;
      other.BeginBlock(); other.EndBlock();
      std::atomic<bool> requested = false, finished = false;
      std::thread reader;
      {
        auto held = other.Borrow();
        reader = std::thread([&] {
          auto owned = memory.Borrow();
          requested.store(true);
          const auto value = other.Load<uint32_t>(context, 0x80000000);
          finished.store(value == 0);
        });
        while (!requested.load()) { std::this_thread::yield(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(!finished.load());
      }
      reader.join();
      check(finished.load());
    }
    std::atomic<bool> failed = false;
    {
      browser::Memory other;
      std::atomic<bool> requested = false, finished = false;
      std::thread reader;
      {
        auto held = other.Mutate();
        reader = std::thread([&] {
          auto owned = memory.Mutate();
          requested.store(true);
          finished.store(other.Load<uint32_t>(context, 0x80000000) == 0);
        });
        while (!requested.load()) { std::this_thread::yield(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(!finished.load());
      }
      reader.join();
      check(finished.load() && !browser::Memory::HasBorrow());
      {
        auto owned = memory.Mutate();
        { auto nested = other.Mutate(); other.Store<uint32_t>(context, 0x80000000, 9); }
        memory.Store<uint32_t>(context, 0x80000100, 0);
        check(other.Load<uint32_t>(context, 0x80000000) == 9);
      }
    }
    const auto increment = [&](uint32_t address) {
      try {
        for (uint32_t n = 0; n < 10000; ++n) {
          block([&] { memory.Store(context, address, memory.Load<uint32_t>(context, address) + 1); });
        }
      } catch (...) { failed.store(true); }
    };
    std::thread mapper([&] {
      try {
        for (uint32_t n = 0; n < 500; ++n) {
          memory.Ensure(0x40000, 4096);
          memory.Decommit(0x40000, 4096);
          memory.Commit(0x40000, 4096);
          memory.Discard(0x40000, 4096);
        }
      } catch (...) { failed.store(true); }
    });
    std::thread worker(increment, 0x80000100);
    increment(0xf0000100);
    worker.join(); mapper.join();
    check(!failed.load() && memory.Load<uint32_t>(context, 0x80000100) == 20000 && !browser::Memory::HasBorrow());
    std::puts("memory-pages-contracts-ok");
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
