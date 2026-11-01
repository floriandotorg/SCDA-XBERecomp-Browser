#include "browser/cpu_runtime.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

const auto check = [](bool valid, const char* operation) {
  if (!valid) { throw std::runtime_error(operation); }
};
const auto fault = [](auto operation, const char* name) {
  bool rejected = false;
  try { operation(); } catch (const browser::Fault&) { rejected = true; }
  check(rejected, name);
};

int main() {
  try {
    browser::Memory memory;
    X86Context context{};
    memory.Map(0x10000, 4096);
    memory.Commit(0x10000, 4096);
    fault([&] { memory.Span(context, 0x10000, 4); }, "raw host spans require a backing lease");
    {
      auto lease = memory.Borrow();
      auto* span = memory.Span(context, 0x10000, 4);
      *span = 42;
      fault([&] { memory.Discard(0x10000, 4096); }, "borrowed storage cannot be released reentrantly");
      fault([&] { memory.Decommit(0x10000, 4096); }, "borrowed storage cannot be decommitted reentrantly");
      fault([&] { memory.Ensure(0x20000, 4096); }, "borrowed region index cannot be mutated reentrantly");
      check(*span == 42, "rejected transitions preserve borrowed bytes");
    }
    std::atomic<bool> requested = false, finished = false;
    std::thread transition;
    {
      auto lease = memory.Borrow();
      auto* span = memory.Span(context, 0x10000, 4);
      transition = std::thread([&] {
        requested.store(true);
        memory.Discard(0x10000, 4096);
        finished.store(true);
      });
      while (!requested.load()) { std::this_thread::yield(); }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      check(!finished.load() && *span == 42, "release must wait for outstanding host spans");
    }
    transition.join();
    check(finished.load(), "release proceeds after host span lease ends");
    fault([&] { uint32_t value; memory.Read(context, 0x10000, &value, 4); }, "released backing is inaccessible");
    std::atomic<bool> failed = false;
    std::thread mapper([&] {
      try {
        for (uint32_t n = 0; n < 2000; ++n) {
          const uint32_t address = 0x10000 + (n % 64) * 4096;
          memory.Ensure(address, 4096);
          memory.Decommit(address, 4096);
          memory.Commit(address, 4096);
          memory.Discard(address, 4096);
        }
      } catch (...) { failed.store(true); }
    });
    const auto increment = [&](uint32_t address) {
      try {
        for (uint32_t n = 0; n < 10000; ++n) {
          auto lease = memory.Borrow();
          auto* counter = reinterpret_cast<uint32_t*>(memory.Span(context, address, 4));
          __atomic_fetch_add(counter, 1u, __ATOMIC_SEQ_CST);
        }
      } catch (...) { failed.store(true); }
    };
    std::thread first(increment, 0x80000200);
    increment(0xF0000200);
    first.join();
    mapper.join();
    uint32_t total = 0;
    memory.Read(context, 0x80000200, &total, 4);
    check(!failed.load() && total == 20000, "concurrent mapping and leased atomic aliases preserve totals");
    check(memory.BackingBytes() == (64u << 20) + (64u << 20) / 4096, "concurrent release returns backing budget");
    std::printf("memory-concurrency-ok total=%u backing=%zu\n", total, memory.BackingBytes());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Memory concurrency fixture: %s\n", error.what());
    return 1;
  }
}
