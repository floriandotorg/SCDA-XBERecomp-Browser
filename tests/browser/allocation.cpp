#include "browser/cpu_runtime.h"
#include "kernel/memory.h"
#include "core/log.h"

void logx::Write(Level, const char*, ...) {}
#include <atomic>
#include <cstdio>
#include <thread>
#include <stdexcept>

const auto check = [](bool valid, const char* operation) {
  if (!valid) { throw std::runtime_error(operation); }
};

int main() {
  try {
    check(mem::Init(), "compact allocator initialization");
    auto& memory = browser::Memory::From(mem::Base());
    X86Context ctx{};
    {
      auto lease = memory.Borrow();
      bool rejected = false;
      try { mem::Query(); } catch (const browser::Fault&) { rejected = true; }
      check(rejected, "allocator lock cannot be acquired while retaining a host backing lease");
    }
    const size_t physicalBacking = memory.BackingBytes();
    uint32_t address = 0, size = 512u << 20;
    check(mem::AllocateVirtual(address, size, mem::kMemReserve, 4) == 0 && address == mem::kUserLow, "reserve first fit");
    check(memory.BackingBytes() == physicalBacking, "reservation must not allocate backing");
    mem::RegionInfo info{};
    check(mem::QueryVirtual(address + 123, info) && info.base == address && info.size == size && info.state == mem::kMemReserve && info.alloc_protect == 4, "reserved query metadata");
    uint32_t committed = address + 0x1001, committedSize = 0x1FFF;
    check(mem::AllocateVirtual(committed, committedSize, mem::kMemCommit, 4) == 0 && committed == address + 0x1000 && committedSize == 0x2000, "commit page rounding");
    uint32_t value = 0x12345678;
    memory.Write(ctx, committed, &value, sizeof(value));
    uint32_t repeated = committed, repeatedSize = 4;
    check(mem::AllocateVirtual(repeated, repeatedSize, mem::kMemCommit, 4) == 0, "repeated commit");
    uint32_t observed = 0;
    memory.Read(ctx, committed, &observed, sizeof(observed));
    check(observed == value, "repeated commit preserves data");
    check(mem::QueryVirtual(committed + 1, info) && info.size == 0x2000 && info.state == mem::kMemCommit && info.alloc_base == address, "committed query run");
    uint32_t invalid = address + size - 1, invalidSize = 2;
    check(mem::FreeVirtual(invalid, invalidSize, mem::kMemDecommit) == 0xC0000018, "decommit cannot escape reservation");
    uint32_t decommitted = committed, decommittedSize = 0x1000;
    check(mem::FreeVirtual(decommitted, decommittedSize, mem::kMemDecommit) == 0 && !mem::IsCommitted(committed), "decommit page state");
    repeated = committed; repeatedSize = 4;
    check(mem::AllocateVirtual(repeated, repeatedSize, mem::kMemCommit, 4) == 0, "recommit");
    memory.Read(ctx, committed, &observed, sizeof(observed));
    check(observed == 0, "recommit zeroes backing");
    uint32_t conflict = address + 1, conflictSize = 1;
    check(mem::AllocateVirtual(conflict, conflictSize, mem::kMemReserve, 4) == 0xC0000018, "overlapping reservation rejected");
    uint32_t overflow = 0xFFFFF000, overflowSize = 0x2000;
    check(mem::AllocateVirtual(overflow, overflowSize, mem::kMemReserve | mem::kMemCommit, 4) == 0xC000000D, "virtual address overflow rejected");
    uint32_t releasedSize = 0;
    check(mem::FreeVirtual(address, releasedSize, mem::kMemRelease) == 0 && releasedSize == size, "release reservation");
    check(memory.BackingBytes() == physicalBacking, "release returns backing budget");
    uint32_t topDown = 0, topDownSize = 1;
    check(mem::AllocateVirtual(topDown, topDownSize, mem::kMemReserve | mem::kMemTopDown, 4) == 0 && topDown == mem::kUserHigh - 0x10000, "top down placement");
    mem::FreeVirtual(topDown, topDownSize, mem::kMemRelease);
    for (uint32_t n = 0; n < 32; ++n) {
      uint32_t allocation = 0, allocationSize = 0x10000;
      check(mem::AllocateVirtual(allocation, allocationSize, mem::kMemReserve | mem::kMemCommit, 4) == 0 && allocation == mem::kUserLow, "released virtual range reused");
      mem::FreeVirtual(allocation, allocationSize, mem::kMemRelease);
    }
    check(!mem::Commit(0xFFFFF000, 0x2000), "raw commit overflow rejected");
    check(!mem::Commit(0x83FFF000, 0x2000) && !mem::Commit(0xF3FFF000, 0x2000), "physical commit cannot claim bytes outside either alias");
    check(mem::AllocSystem(0xFFFFFFFF, 16) == 0, "system size overflow rejected");
    check(mem::AllocSystem(16, 24) == 0, "system non power of two alignment rejected");
    check(mem::AllocContiguous(0xFFFFFFFF, 0, 0xFFFFFFFF, 0x1000) == 0, "physical size overflow rejected");
    check(mem::AllocStack(0xFFFFFFFF) == 0, "stack size overflow rejected");
    const uint32_t system = mem::AllocSystem(17, 64);
    check(system == mem::kSystemBase && mem::AllocationSize(system) == 32, "system size and alignment");
    memory.Write(ctx, system, &value, sizeof(value));
    mem::FreeSystem(system);
    check(mem::IsCommitted(system) && mem::AllocSystem(17, 64) == system, "system free retains commitment and reuses address");
    memory.Read(ctx, system, &observed, sizeof(observed));
    check(observed == 0, "system reuse zeroes bytes");
    const uint32_t stack = mem::AllocStack(1);
    check(stack == mem::kStackBase + 0x20000 && !mem::IsCommitted(mem::kStackBase) && mem::IsCommitted(stack - 1), "stack top and guard");
    mem::FreeStack(stack);
    check(!mem::IsCommitted(stack - 1), "stack free decommits pages");
    const uint32_t physical = mem::AllocContiguous(1, 0, 0xFFFFFFFF, 0x10000);
    check(physical == mem::kContiguousBase + mem::kKernelDataSize && mem::AllocationSize(physical) == mem::kPage, "physical allocation excludes kernel megabyte");
    memory.Write(ctx, physical, &value, sizeof(value));
    memory.Read(ctx, mem::kWriteCombineBase + mem::PhysicalFromVirtual(physical), &observed, sizeof(observed));
    check(observed == value, "allocator physical aliases");
    mem::FreeContiguous(physical);
    check(mem::IsCommitted(physical), "physical free retains commitment");
    const size_t beforeFailure = memory.BackingBytes();
    uint32_t excessive = 0, excessiveSize = 64u << 20;
    check(mem::AllocateVirtual(excessive, excessiveSize, mem::kMemReserve | mem::kMemCommit, 4) == 0xC0000017, "backing budget returns no memory");
    check(excessive == 0 && excessiveSize == (64u << 20) && memory.BackingBytes() == beforeFailure, "failed commit rolls back reservation and outputs");
    uint32_t afterFailure = 0, afterFailureSize = 1;
    check(mem::AllocateVirtual(afterFailure, afterFailureSize, mem::kMemCommit, 4) == 0 && afterFailure == mem::kUserLow, "failed allocation leaves free range reusable");
    mem::ReserveImage(0x51001, 0x2000);
    check(mem::QueryVirtual(0x51001, info) && info.alloc_base == 0x50000 && info.size == 0xF000 && info.state == mem::kMemCommit, "image reservation alignment and commitment");
    bool imageRejected = false;
    try { mem::ReserveImage(0x51001, 1); } catch (const std::exception&) { imageRejected = true; }
    check(imageRejected, "overlapping image reservation rejected");
    imageRejected = false;
    try { mem::ReserveImage(0x1000000, 64u << 20); } catch (const std::exception&) { imageRejected = true; }
    check(imageRejected && mem::QueryVirtual(0x1000000, info) && info.state == 0x10000, "image backing failure cannot publish reservation");
    std::atomic<bool> concurrencyFailed = false;
    const auto allocate = [&](uint32_t worker) {
      try {
        X86Context workerContext{};
        for (uint32_t n = 0; n < 512; ++n) {
          uint32_t address = 0, size = 4096;
          check(mem::AllocateVirtual(address, size, mem::kMemReserve | mem::kMemCommit, 4) == 0, "concurrent virtual allocation");
          const uint32_t expected = worker * 1000 + n;
          {
            auto lease = memory.Borrow();
            auto* span = reinterpret_cast<uint32_t*>(memory.Span(workerContext, address, sizeof(uint32_t)));
            *span = expected;
            check(*span == expected, "concurrent host span remains stable while allocator waits");
          }
          uint32_t observed = 0;
          memory.Read(workerContext, address, &observed, sizeof(observed));
          check(observed == expected, "concurrent allocations have independent bytes");
          check(mem::FreeVirtual(address, size, mem::kMemRelease) == 0, "concurrent virtual release");
        }
      } catch (...) { concurrencyFailed.store(true); }
    };
    std::thread first(allocate, 1);
    allocate(2);
    first.join();
    check(!concurrencyFailed.load(), "concurrent allocator and host span contracts");
    const auto stats = mem::Query();
    check(stats.total_pages == 16384 && stats.virtual_committed == 32, "allocator statistics");
    std::printf("allocation-contracts-ok backing=%zu\n", memory.BackingBytes());
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "Allocator fixture: %s\n", error.what());
    return 1;
  }
}
