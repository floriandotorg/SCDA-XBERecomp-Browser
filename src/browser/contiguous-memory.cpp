#include "browser/contiguous-memory.h"
#include <array>
#include <bit>

namespace browser {
namespace {

class ContiguousMemory {
  static constexpr uint32_t pageSize = 0x1000;
  static constexpr uint32_t arenaBase = 0x80100000;
  static constexpr uint32_t capacity = 63u << 20;
  std::array<uint32_t, capacity / pageSize> pages_{};
  std::array<bool, capacity / pageSize> persistent_{};

  size_t Owner(const X86Context& ctx, uint32_t address) const {
    if (address < arenaBase || address - arenaBase >= capacity || address % pageSize) {
      throw Fault(ctx, address, 0, "invalid contiguous allocation owner");
    }
    const size_t first = (address - arenaBase) / pageSize;
    if (!pages_[first] || pages_[first] == UINT32_MAX) {
      throw Fault(ctx, address, 0, "unknown contiguous allocation");
    }
    return first;
  }
public:
  uint32_t Usage() const {
    uint32_t pages = 0;
    for (const uint32_t count : pages_) {
      if (count != UINT32_MAX) { pages += count; }
    }
    return pages;
  }

  void Persist(Runtime& runtime, Memory& memory, X86Context& ctx) {
    if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "contiguous persistence requires no inherited backing lease"); }
    const uint64_t stackEnd = uint64_t(ctx.esp) + 16;
    if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 16, "contiguous persistence stack cleanup overflow"); }
    auto mutation = memory.Mutate();
    std::array<uint32_t, 4> frame;
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    const size_t first = Owner(ctx, frame[1]);
    const uint32_t size = pages_[first] * pageSize;
    if (frame[2] != size) { throw Fault(ctx, frame[1], frame[2], "persistence requires an exact live contiguous allocation"); }
    memory.CheckRange(ctx, frame[1], size);
    runtime.Checkpoint(ctx);
    persistent_[first] = (frame[3] & 0xff) != 0;
    ctx.esp = uint32_t(stackEnd);
  }

  std::vector<PersistentContiguousRange> SnapshotAfterJoin(Runtime& runtime, Memory& memory) {
    if (Memory::HasBorrow() || runtime.OwnsCurrentWorker() || runtime.ActiveWorkers()) {
      throw Fault({}, 178, 0, "contiguous persistence export requires quiescent joined workers without a backing lease");
    }
    runtime.Checkpoint({});
    if (runtime.Join()) { throw Fault({}, 178, 0, "contiguous persistence export requires successful workers"); }
    auto mutation = memory.Mutate();
    if (runtime.ActiveWorkers()) { throw Fault({}, 178, 0, "contiguous persistence export requires quiescent joined workers"); }
    runtime.Checkpoint({});
    std::vector<PersistentContiguousRange> ranges;
    for (size_t n = 0; n < pages_.size(); ++n) {
      if (!persistent_[n]) { continue; }
      const uint32_t address = arenaBase + uint32_t(n * pageSize);
      PersistentContiguousRange range{address, std::vector<uint8_t>(size_t(pages_[n]) * pageSize)};
      memory.Read({}, address, range.bytes.data(), range.bytes.size());
      ranges.push_back(std::move(range));
    }
    runtime.Checkpoint({});
    return ranges;
  }
  uint32_t Allocate(const X86Context& ctx, Memory& memory, uint32_t size, uint32_t low, uint32_t high, uint32_t alignment) {
    const uint64_t rounded = size ? (uint64_t(size) + pageSize - 1) / pageSize : 1;
    const uint32_t effectiveAlignment = alignment < pageSize ? pageSize : alignment;
    if (rounded > pages_.size() || (alignment && !std::has_single_bit(alignment)) || low > high) { return 0; }
    const size_t count = size_t(rounded);
    size_t available = 0;
    for (size_t n = 0; n < pages_.size(); ++n) {
      available = pages_[n] ? 0 : available + 1;
      if (available < count) { continue; }
      const size_t first = n + 1 - count;
      const uint64_t physical = 0x100000ull + first * pageSize;
      const uint64_t end = physical + count * pageSize - 1;
      if (physical < low || end > high || physical % effectiveAlignment) { continue; }
      const uint32_t result = arenaBase + uint32_t(first * pageSize);
      {
        auto lease = memory.Borrow();
        std::memset(memory.Span(ctx, result, count * pageSize), 0, count * pageSize);
      }
      pages_[first] = uint32_t(count);
      for (size_t n = first + 1; n < first + count; ++n) { pages_[n] = UINT32_MAX; }
      return result;
    }
    return 0;
  }

  void Release(const X86Context& ctx, uint32_t address) {
    const size_t first = Owner(ctx, address);
    const uint32_t count = pages_[first];
    persistent_[first] = false;
    for (size_t n = first; n < first + count; ++n) { pages_[n] = 0; }
  }

  void Invoke(Runtime& runtime, Memory& memory, X86Context& ctx, uint32_t ordinal) {
    if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "contiguous allocation requires no inherited backing lease"); }
    // 165 MmAllocateContiguousMemory, 166 its Ex form, 167 MmAllocateSystemMemory (versus), 171 MmFreeContiguousMemory,
    // 172 MmFreeSystemMemory (versus); system memory is page aligned contiguous memory, as native's mem::AllocSystem
    const uint32_t frameSize = ordinal == 166 ? 24 : ordinal == 167 || ordinal == 172 ? 12 : 8;
    const uint64_t stackEnd = uint64_t(ctx.esp) + frameSize;
    if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, frameSize, "contiguous allocation stack cleanup overflow"); }
    auto mutation = memory.Mutate();
    std::array<uint32_t, 6> frame{};
    memory.Read(ctx, ctx.esp, frame.data(), frameSize);
    runtime.Checkpoint(ctx);
    if (ordinal == 171) {
      Release(ctx, frame[1]);
    } else if (ordinal == 172) {
      Release(ctx, frame[1]);
      ctx.eax = frame[2];
    } else if (ordinal == 167) {
      ctx.eax = Allocate(ctx, memory, frame[1], 0, UINT32_MAX, 0x1000);
    } else {
      const uint32_t low = ordinal == 166 ? frame[2] : 0;
      const uint32_t high = ordinal == 166 ? frame[3] : UINT32_MAX;
      const uint32_t alignment = ordinal == 166 ? frame[4] : 0;
      if (ordinal == 166 && frame[5] != 4 && frame[5] != 0x204 && frame[5] != 0x404) {
        throw Fault(ctx, frame[5], 0, "unsupported contiguous allocation protection");
      }
      ctx.eax = Allocate(ctx, memory, frame[1], low, high, alignment);
    }
    ctx.esp = uint32_t(stackEnd);
  }
};

}

SnapshotContiguousMemory InstallContiguousMemory(Runtime& runtime, Memory& memory, QueryContiguousMemoryUsage* usage,
  ContiguousMemoryAccess* access) {
  if (runtime.ActiveWorkers() || runtime.KernelTarget(165) || runtime.KernelTarget(166) || runtime.KernelTarget(167) || runtime.KernelTarget(171) ||
      runtime.KernelTarget(172) || runtime.KernelTarget(178)) {
    throw Fault({}, 165, 0, "contiguous allocation installation requires an unused stopped runtime");
  }
  runtime.Checkpoint({});
  auto allocations = std::make_shared<ContiguousMemory>();
  for (const uint32_t ordinal : {165u, 166u, 167u, 171u, 172u, 178u}) {
    runtime.RegisterKernel(ordinal, [&runtime, &memory, allocations, ordinal](X86Context& ctx, uint8_t* base) {
      if (&Runtime::From(ctx) != &runtime || base != memory.Base() || !runtime.OwnsCurrentContext(ctx)) {
        throw Fault(ctx, ordinal, 0, "contiguous allocation requires its owned session worker");
      }
      if (ordinal == 178) {
        allocations->Persist(runtime, memory, ctx);
      } else {
        allocations->Invoke(runtime, memory, ctx, ordinal);
      }
    });
  }
  if (usage) {
    *usage = [&runtime, &memory, allocations](const X86Context& ctx, Memory& backing) {
      if (ctx.host != &runtime || &backing != &memory || !runtime.OwnsCurrentContext(ctx)) {
        throw Fault(ctx, 181, 0, "contiguous usage requires its owned session worker and backing");
      }
      auto lease = memory.Borrow();
      runtime.Checkpoint(ctx);
      return allocations->Usage();
    };
  }
  if (access) {
    const auto requireOwner = [&runtime, &memory](const X86Context& ctx) {
      if (ctx.host != &runtime || !runtime.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
        throw Fault(ctx, 165, 0, "contiguous SDK access requires its owned session worker without a backing lease");
      }
      runtime.Checkpoint(ctx);
    };
    access->allocate = [&runtime, &memory, allocations, requireOwner](const X86Context& ctx, uint32_t bytes, uint32_t alignment) {
      requireOwner(ctx);
      auto mutation = memory.Mutate();
      runtime.Checkpoint(ctx);
      return allocations->Allocate(ctx, memory, bytes, 0, UINT32_MAX, alignment);
    };
    access->release = [&runtime, &memory, allocations, requireOwner](const X86Context& ctx, uint32_t address) {
      requireOwner(ctx);
      auto mutation = memory.Mutate();
      runtime.Checkpoint(ctx);
      allocations->Release(ctx, address);
    };
  }
  return [&runtime, &memory, allocations] { return allocations->SnapshotAfterJoin(runtime, memory); };
}

}
