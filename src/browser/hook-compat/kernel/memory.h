#pragma once
#include <cstdint>

// Browser stand-in for src/kernel/memory.h: the allocations and queries game hooks make, over the bound session's
// memory and its contiguous allocator
namespace mem {

constexpr uint32_t kPage = 0x1000;
constexpr uint32_t kContiguousBase = 0x80000000;

bool IsCommitted(uint32_t va);
// contiguous memory the guest can see, kept until the session ends; 0 when it is exhausted
uint32_t AllocSystem(uint32_t size, uint32_t align = 16);
uint32_t AllocContiguous(uint32_t size, uint32_t lowest, uint32_t highest, uint32_t align);
inline uint32_t PhysicalFromVirtual(uint32_t va) { return va & 0x1FFFFFFF; }
inline uint32_t VirtualFromPhysical(uint32_t pa) { return kContiguousBase | pa; }

}  // namespace mem
