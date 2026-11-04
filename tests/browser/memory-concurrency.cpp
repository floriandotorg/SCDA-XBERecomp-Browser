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
    {
      browser::Memory other;
      std::atomic<bool> requested = false, finished = false;
      std::thread reader;
      {
        auto otherLease = other.Borrow();
        reader = std::thread([&] {
          auto lease = memory.Borrow();
          requested.store(true);
          uint32_t value = 0;
          other.Read(context, 0x80000000, &value, 4);
          finished.store(value == 0);
        });
        while (!requested.load()) { std::this_thread::yield(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(!finished.load(), "a lease on another memory must not bypass its lock");
      }
      reader.join();
      check(finished.load(), "a foreign memory read resumes after its owner's lease");
      {
        auto lease = memory.Borrow();
        {
          auto nested = other.Borrow();
          uint32_t value = 3;
          other.Write(context, 0x80000000, &value, 4);
        }
        fault([&] { uint32_t value; memory.Read(context, 0xdead0000, &value, 4); }, "leased reads retain address validation");
        const uint32_t value = 7;
        memory.Write(context, 0x10000, &value, 4);
      }
      check(!browser::Memory::HasBorrow(), "nested leases and faults restore thread ownership");
    }
    fault([&] { memory.Span(context, 0x10000, 4); }, "raw host spans require a backing lease");
    const auto physicalBefore = memory.PhysicalWriteVersion(context, 0x80010000, 8192);
    check(physicalBefore && !memory.PhysicalWriteVersion(context, 0x10000, 4), "physical tracking must not invent versions for private virtual storage");
    const uint32_t trackedValue = 0x12345678;
    memory.Write(context, 0x80010000, &trackedValue, 4);
    const auto physicalWritten = memory.PhysicalWriteVersion(context, 0xf0010000, 4096);
    check(physicalWritten && *physicalWritten > *physicalBefore && memory.PhysicalWriteVersion(context, 0x80011000, 4096) == physicalBefore,
      "physical writes must invalidate exactly their aliased pages");
    uint32_t trackedRead = 0;
    memory.Read(context, 0x80010000, &trackedRead, 4);
    check(trackedRead == trackedValue && memory.PhysicalWriteVersion(context, 0x80010000, 4096) == physicalWritten,
      "read-only copies must not invalidate physical texture versions");
    {
      auto lease = memory.Borrow();
      auto* tracked = memory.Span(context, 0xf0011000, 4);
      tracked[0] = 99;
    }
    const auto physicalSpan = memory.PhysicalWriteVersion(context, 0x80010000, 8192);
    check(physicalSpan && *physicalSpan > *physicalWritten && memory.PhysicalWriteVersion(context, 0xf0011000, 4096) == physicalSpan,
      "mutable leased spans must invalidate aliased physical ranges before they can be cached");
    fault([&] { memory.PhysicalWriteVersion(context, 0xfffffff0, 64); }, "version queries reject wrapped guest spans");

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
