#include "browser/virtual-memory.h"
#include "browser/image.h"
#include "browser/kernel-provider.h"
#include <algorithm>
#include <array>

namespace browser {
namespace {

class VirtualMemory {
  static constexpr uint32_t arenaBase = 0x10000000;
  static constexpr uint32_t capacity = 16u << 20;
  static constexpr size_t reservationLimit = 1024;
  struct Reservation {
    uint32_t base;
    uint32_t size;
    uint32_t protect;
  };
  std::vector<Reservation> reservations_;
  std::vector<Reservation> stacks_;
public:
  explicit VirtualMemory(Memory& memory) {
    reservations_.reserve(reservationLimit);
    stacks_.reserve(reservationLimit);
    memory.Map(arenaBase, capacity);
  }

  void Stack(Runtime& runtime, Memory& memory, X86Context& ctx, uint32_t ordinal) {
    if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "kernel stack service requires no backing lease"); }
    const uint64_t stackEnd = uint64_t(ctx.esp) + 12;
    if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 12, "kernel stack frame overflow"); }
    auto mutation = memory.Mutate();
    runtime.Checkpoint(ctx);
    std::array<uint32_t, 3> frame;
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    uint32_t result = 0;
    if (ordinal == 169) {
      if (frame[1] <= (16u << 20) && stacks_.size() < reservationLimit) {
        const uint32_t size = ((frame[1] + 0xffff) & ~0xffffu) + 0x10000;
        uint32_t address = 0xc8000000;
        auto position = stacks_.begin();
        while (position != stacks_.end()) {
          if (uint64_t(address) + size <= position->base) { break; }
          address = position->base + position->size;
          ++position;
        }
        if (uint64_t(address) + size <= 0xd0000000) {
          memory.Map(address, size);
          try {
            if (size > 0x10000) { memory.Commit(address + 0x10000, size - 0x10000); }
            stacks_.insert(position, {address, size, 4});
          } catch (...) {
            memory.Discard(address, size);
            throw;
          }
          result = address + size;
        }
      }
    } else {
      const auto position = std::find_if(stacks_.begin(), stacks_.end(), [&](const auto& stack) {
        return stack.base + stack.size == frame[1];
      });
      if (position != stacks_.end()) {
        if (uint64_t(ctx.esp) < uint64_t(position->base) + position->size && stackEnd > position->base) {
          throw Fault(ctx, ctx.esp, 12, "kernel stack deletion overlaps its service frame");
        }
        memory.Discard(position->base, position->size);
        stacks_.erase(position);
      }
    }
    ctx.eax = result;
    ctx.esp = uint32_t(stackEnd);
  }

  void Statistics(Runtime& runtime, Memory& memory, X86Context& ctx, PreparedImage& image,
      const std::shared_ptr<const xbe::Image>& source, const QueryContiguousMemoryUsage& contiguousUsage) {
    if (!runtime.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
      throw Fault(ctx, 181, 0, "memory statistics requires its owned worker without a backing lease");
    }
    if (!contiguousUsage && runtime.KernelTarget(165)) {
      throw Fault(ctx, 181, 0, "memory statistics requires the installed contiguous usage owner");
    }
    std::lock_guard threads(image.threadLock);
    auto lease = memory.Borrow();
    if (image.memory.get() != &memory || !image.importsBound || image.boundRuntime != &runtime ||
        image.AuthenticatedSource() != source) {
      throw Fault(ctx, 181, 0, "memory statistics image session mismatch");
    }
    runtime.Checkpoint(ctx);
    const uint64_t stackEnd = uint64_t(ctx.esp) + 8;
    if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 8, "memory statistics stack cleanup overflow"); }
    std::array<uint32_t, 2> frame;
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    const uint32_t output = frame[1];
    if (!output || output % 4 || (uint64_t(output) < stackEnd && uint64_t(output) + 36 > ctx.esp)) {
      throw Fault(ctx, output, 36, "invalid memory statistics output");
    }
    uint32_t length;
    memory.Read(ctx, output, &length, sizeof(length));
    uint32_t status = 0xc000000d;
    if (length == 36) {
      memory.CheckRange(ctx, output, 36);
      const auto usage = memory.QueryUsage();
      const uint64_t allocated = uint64_t(usage.privateCommittedPages) + (contiguousUsage ? contiguousUsage(ctx, memory) : 0);
      if (allocated > usage.physicalPages) { throw Fault(ctx, 181, 0, "memory statistics physical budget overcommitted"); }
      uint32_t reserved = 0;
      for (const auto& reservation : reservations_) { reserved += reservation.size; }
      uint32_t stackPages = 0;
      for (const auto& stack : stacks_) { stackPages += memory.CommittedPages(stack.base, stack.size); }
      for (const auto& thread : image.threads) {
        if (thread.stackTop < thread.stackLimit) { throw Fault(ctx, 181, 0, "invalid prepared thread stack range"); }
        stackPages += memory.CommittedPages(thread.stackLimit, thread.stackTop - thread.stackLimit);
      }
      const std::array<uint32_t, 9> information{length, usage.physicalPages, uint32_t(usage.physicalPages - allocated),
        memory.CommittedPages(arenaBase, capacity) * 4096, reserved, 0,
        memory.CommittedPages(KernelPoolBase, KernelPoolCapacity), stackPages,
        memory.CommittedPages(source->base, source->image_size)};
      runtime.Checkpoint(ctx);
      memory.Write(ctx, output, information.data(), sizeof(information));
      status = 0;
    } else {
      runtime.Checkpoint(ctx);
    }
    ctx.eax = status;
    ctx.esp = uint32_t(stackEnd);
  }

  void Query(Runtime& runtime, Memory& memory, X86Context& ctx, const xbe::Image* image) {
    if (!runtime.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
      throw Fault(ctx, 217, 0, "virtual query requires its owned worker without a backing lease");
    }
    auto mutation = memory.Mutate();
    runtime.Checkpoint(ctx);
    const uint64_t stackEnd = uint64_t(ctx.esp) + 12;
    if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 12, "virtual query stack cleanup overflow"); }
    std::array<uint32_t, 3> frame;
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    const uint32_t output = frame[2];
    if (!output || output % 4 || (uint64_t(output) < stackEnd && uint64_t(output) + 28 > ctx.esp)) {
      throw Fault(ctx, output, 28, "invalid virtual query output");
    }
    memory.CheckRange(ctx, output, 28);
    constexpr uint32_t userEnd = 0x7ffe0000;
    uint32_t status = 0xc000000d;
    if (frame[1] < userEnd) {
      const uint32_t address = frame[1] & ~0xfffu;
      auto pages = memory.QueryPages(address);
      std::array<uint32_t, 7> information{address, 0, 0, 0, 0x10000, 1, 0};
      uint64_t end = std::min(pages.end, uint64_t(userEnd));
      if (address >= arenaBase && address < arenaBase + capacity) {
        end = arenaBase + capacity;
        const auto position = std::upper_bound(reservations_.begin(), reservations_.end(), address,
          [](uint32_t value, const Reservation& reservation) { return value < reservation.base; });
        if (position != reservations_.begin() && uint64_t((position - 1)->base) + (position - 1)->size > address) {
          const auto& reservation = *(position - 1);
          end = std::min(pages.end, uint64_t(reservation.base) + reservation.size);
          information[1] = reservation.base;
          information[2] = reservation.protect;
          information[4] = pages.committed ? 0x1000 : 0x2000;
          information[5] = pages.committed ? 4 : 0;
          information[6] = 0x20000;
        } else if (position != reservations_.end()) {
          end = position->base;
        }
      } else if (pages.mapped) {
        information[1] = pages.mappingBase;
        information[2] = 4;
        information[4] = pages.committed ? 0x1000 : 0x2000;
        information[5] = pages.committed ? 4 : 0;
        information[6] = 0x20000;
        if (image && address >= image->base && uint64_t(address) < uint64_t(image->base) + image->image_size) {
          information[1] = image->base;
          information[6] = 0x1000000;
        }
      }
      information[3] = uint32_t(end - address);
      runtime.Checkpoint(ctx);
      memory.Write(ctx, output, information.data(), sizeof(information));
      status = 0;
    }
    ctx.eax = status;
    ctx.esp = uint32_t(stackEnd);
  }

  void Release(Runtime& runtime, Memory& memory, X86Context& ctx) {
    if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "virtual release requires no inherited backing lease"); }
    const uint64_t stackEnd = uint64_t(ctx.esp) + 16;
    if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 16, "virtual release stack cleanup overflow"); }
    auto mutation = memory.Mutate();
    runtime.Checkpoint(ctx);
    std::array<uint32_t, 4> frame;
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    for (const uint32_t output : {frame[1], frame[2]}) {
      if (!output || output % 4 || (uint64_t(output) < stackEnd && uint64_t(output) + 4 > ctx.esp)) {
        throw Fault(ctx, output, 4, "invalid virtual release output");
      }
      memory.CheckRange(ctx, output, 4);
    }
    if (frame[1] == frame[2]) { throw Fault(ctx, frame[1], 4, "virtual release outputs overlap"); }
    uint32_t base, size;
    memory.Read(ctx, frame[1], &base, 4);
    memory.Read(ctx, frame[2], &size, 4);
    if (frame[3] != 0x8000 && frame[3] != 0x4000) {
      throw Fault(ctx, frame[3], 0, "unsupported virtual release mode");
    }
    const bool release = frame[3] == 0x8000;
    auto reservation = std::upper_bound(reservations_.begin(), reservations_.end(), base,
      [](uint32_t value, const Reservation& entry) { return value < entry.base; });
    if (reservation != reservations_.begin()) { --reservation; }
    const bool owned = reservation != reservations_.end() && base >= reservation->base &&
      uint64_t(base) < uint64_t(reservation->base) + reservation->size;
    const uint64_t requestedEnd = (uint64_t(base) + size + 0xfff) & ~uint64_t(0xfff);
    uint32_t status = 0;
    if ((release && size) || (!release && requestedEnd > (1ull << 32))) {
      status = 0xc000000d;
    } else if (!owned || (release && reservation->base != base)) {
      status = 0xc000009f;
    } else if (!release && !size && reservation->base != base) {
      status = 0xc000000d;
    } else if (!release && size && requestedEnd > uint64_t(reservation->base) + reservation->size) {
      status = 0xc0000018;
    } else {
      const uint32_t address = base & ~0xfffu;
      const uint64_t end = size ? requestedEnd : uint64_t(reservation->base) + reservation->size;
      for (const uint32_t output : {frame[1], frame[2]}) {
        if (uint64_t(output) < end && uint64_t(output) + 4 > address) {
          throw Fault(ctx, output, 4, "virtual release output overlaps decommitted pages");
        }
      }
      if (uint64_t(ctx.esp) < end && stackEnd > address) {
        throw Fault(ctx, ctx.esp, 16, "virtual release frame overlaps decommitted pages");
      }
      base = address;
      size = uint32_t(end - address);
    }
    runtime.Checkpoint(ctx);
    if (!status) {
      memory.Decommit(base, size);
      if (release) { reservations_.erase(reservation); }
      memory.Write(ctx, frame[1], &base, 4);
      memory.Write(ctx, frame[2], &size, 4);
    }
    ctx.eax = status;
    ctx.esp = uint32_t(stackEnd);
  }

  void Invoke(Runtime& runtime, Memory& memory, X86Context& ctx) {
    if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "virtual allocation requires no inherited backing lease"); }
    const uint64_t stackEnd = uint64_t(ctx.esp) + 24;
    if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 24, "virtual allocation stack cleanup overflow"); }
    auto mutation = memory.Mutate();
    std::array<uint32_t, 6> frame;
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    for (const uint32_t output : {frame[1], frame[3]}) {
      if (!output || output % 4 || (uint64_t(output) < stackEnd && uint64_t(output) + 4 > ctx.esp)) {
        throw Fault(ctx, output, 4, "invalid virtual allocation output");
      }
      memory.CheckRange(ctx, output, 4);
    }
    if (frame[1] == frame[3]) { throw Fault(ctx, frame[1], 4, "virtual allocation outputs overlap"); }
    uint32_t base, size;
    memory.Read(ctx, frame[1], &base, 4);
    memory.Read(ctx, frame[3], &size, 4);
    const uint32_t allocation = frame[4] & ~0x100000u;
    const bool topDown = frame[4] & 0x100000;
    if (frame[2] || (frame[5] != 4 && !(allocation == 0x2000 && frame[5] == 1)) ||
        (allocation != 0x1000 && allocation != 0x2000 && allocation != 0x3000)) {
      throw Fault(ctx, frame[4], 0, "unsupported virtual allocation mode");
    }
    runtime.Checkpoint(ctx);
    uint32_t status = 0;
    const bool commit = frame[4] & 0x1000;
    const bool reserve = (frame[4] & 0x2000) || !base;
    const uint64_t end = (uint64_t(base) + size + 0xfff) & ~uint64_t(0xfff);
    if (!size || end > (1ull << 32)) {
      status = 0xc000000d;
    } else if (reserve) {
      uint32_t address = base & ~0xffffu;
      const uint64_t length = end - address;
      if (length > capacity || reservations_.size() == reservationLimit) {
        status = 0xc0000017;
      } else {
        if (!base) {
          if (topDown) {
            address = uint32_t((uint64_t(arenaBase) + capacity - length) & ~uint64_t(0xffff));
            for (auto position = reservations_.rbegin(); position != reservations_.rend(); ++position) {
              if (address >= uint64_t(position->base) + position->size) { break; }
              if (uint64_t(address) + length > position->base) {
                address = uint32_t((uint64_t(position->base) - length) & ~uint64_t(0xffff));
              }
            }
          } else {
            address = arenaBase;
            for (const auto& reservation : reservations_) {
              if (uint64_t(address) + length <= reservation.base) { break; }
              address = (reservation.base + reservation.size + 0xffff) & ~0xffffu;
            }
          }
        }
        if (address < arenaBase || uint64_t(address) + length > uint64_t(arenaBase) + capacity) {
          status = base ? 0xc0000018 : 0xc0000017;
        } else {
          const auto position = std::lower_bound(reservations_.begin(), reservations_.end(), address,
            [](const Reservation& reservation, uint32_t value) { return reservation.base < value; });
          const bool previousOverlap = position != reservations_.begin() && uint64_t((position - 1)->base) + (position - 1)->size > address;
          if (previousOverlap || (position != reservations_.end() && uint64_t(address) + length > position->base)) {
            status = 0xc0000018;
          } else {
            if (commit) { memory.Commit(address, uint32_t(length)); }
            reservations_.insert(position, {address, uint32_t(length), frame[5]});
            base = address;
            size = uint32_t(length);
          }
        }
      }
    } else {
      const uint32_t address = base & ~0xfffu;
      const auto reservation = std::find_if(reservations_.begin(), reservations_.end(), [&](const Reservation& entry) {
        return address >= entry.base && uint64_t(address) < uint64_t(entry.base) + entry.size;
      });
      if (reservation == reservations_.end()) {
        status = 0xc0000018;
      } else if (end > uint64_t(reservation->base) + reservation->size) {
        status = 0xc0000018;
      } else {
        memory.Commit(address, uint32_t(end - address));
        base = address;
        size = uint32_t(end - address);
      }
    }
    if (!status) {
      memory.Write(ctx, frame[1], &base, 4);
      memory.Write(ctx, frame[3], &size, 4);
    }
    ctx.eax = status;
    ctx.esp = uint32_t(stackEnd);
  }
};

}

namespace {

void Install(Runtime& runtime, Memory& memory, PreparedImage* image, QueryContiguousMemoryUsage contiguousUsage) {
  if (runtime.ActiveWorkers() || runtime.KernelTarget(169) || runtime.KernelTarget(170) || runtime.KernelTarget(184) || runtime.KernelTarget(199) || runtime.KernelTarget(217) || Memory::HasBorrow() ||
      (image && (runtime.KernelTarget(181) || (runtime.KernelTarget(165) && !contiguousUsage)))) {
    throw Fault({}, 184, 0, "virtual memory installation requires an unused stopped runtime");
  }
  if (image && (!image->AuthenticatedSource() || image->importsBound || image->boundRuntime || image->diagnosticRuntime)) {
    throw Fault({}, 217, 0, "virtual memory installation requires an unpublished authenticated image");
  }
  runtime.Checkpoint({});
  auto allocations = std::make_shared<VirtualMemory>(memory);
  const auto source = image ? image->AuthenticatedSource() : nullptr;
  const auto validate = [&runtime, &memory, image, source](X86Context& ctx, uint8_t* base) {
    if (&Runtime::From(ctx) != &runtime || base != memory.Base() ||
        (image && (image->memory.get() != &memory || !image->importsBound || image->boundRuntime != &runtime ||
          image->AuthenticatedSource() != source || !runtime.OwnsCurrentContext(ctx)))) {
      throw Fault(ctx, 217, 0, "virtual memory session mismatch");
    }
  };
  runtime.RegisterKernel(184, [&runtime, &memory, allocations, validate](X86Context& ctx, uint8_t* base) {
    validate(ctx, base);
    allocations->Invoke(runtime, memory, ctx);
  });
  for (const uint32_t ordinal : {169u, 170u}) {
    runtime.RegisterKernel(ordinal, [&runtime, &memory, allocations, validate, ordinal](X86Context& ctx, uint8_t* base) {
      validate(ctx, base);
      allocations->Stack(runtime, memory, ctx, ordinal);
    });
  }
  runtime.RegisterKernel(199, [&runtime, &memory, allocations, validate](X86Context& ctx, uint8_t* base) {
    validate(ctx, base);
    allocations->Release(runtime, memory, ctx);
  });
  runtime.RegisterKernel(217, [&runtime, &memory, allocations, validate, source](X86Context& ctx, uint8_t* base) {
    validate(ctx, base);
    allocations->Query(runtime, memory, ctx, source.get());
  });
  if (image) {
    runtime.RegisterKernel(181, [&runtime, &memory, image, allocations, validate, source,
        contiguousUsage = std::move(contiguousUsage)](X86Context& ctx, uint8_t* base) {
      validate(ctx, base);
      allocations->Statistics(runtime, memory, ctx, *image, source, contiguousUsage);
    });
  }
}

}

void InstallVirtualMemory(Runtime& runtime, Memory& memory) { Install(runtime, memory, nullptr, {}); }

void InstallVirtualMemory(Runtime& runtime, PreparedImage& image, QueryContiguousMemoryUsage contiguousUsage) {
  if (!image.memory) { throw Fault({}, 217, 0, "virtual memory installation requires image memory"); }
  Install(runtime, *image.memory, &image, std::move(contiguousUsage));
}

}
