#include "browser/string-provider.h"
#include <algorithm>
#include <array>

namespace browser {
namespace {

void UpperChar(Runtime& runtime, PreparedImage& image, X86Context& context, uint8_t* base) {
  if (!image.memory || !image.importsBound || image.boundRuntime != &runtime ||
      base != image.memory->Base() || !runtime.OwnsCurrentContext(context) || Memory::HasBorrow()) {
    throw Fault(context, 316, 0, "character uppercasing requires its owned image worker without a backing lease");
  }
  auto& memory = *image.memory;
  auto transition = memory.Mutate();
  runtime.Checkpoint(context);
  const uint64_t stackEnd = uint64_t(context.esp) + 8;
  if (stackEnd > UINT32_MAX) { throw Fault(context, context.esp, 8, "character uppercasing frame overflow"); }
  std::array<uint32_t, 2> frame;
  memory.Read(context, context.esp, frame.data(), sizeof(frame));
  const uint32_t value = frame[1] & 0xff;
  const uint32_t result = value >= 'a' && value <= 'z' ? value - ('a' - 'A') : value;
  runtime.Checkpoint(context);
  context.eax = result;
  context.esp = uint32_t(stackEnd);
}

void EqualString(Runtime& runtime, PreparedImage& image, X86Context& context, uint8_t* base) {
  if (!image.memory || !image.importsBound || image.boundRuntime != &runtime ||
      base != image.memory->Base() || !runtime.OwnsCurrentContext(context) || Memory::HasBorrow()) {
    throw Fault(context, 279, 0, "string equality requires its owned image worker without a backing lease");
  }
  auto& memory = *image.memory;
  auto transition = memory.Mutate();
  runtime.Checkpoint(context);
  const uint64_t stackEnd = uint64_t(context.esp) + 16;
  if (stackEnd > UINT32_MAX) { throw Fault(context, context.esp, 16, "string equality frame overflow"); }
  std::array<uint32_t, 4> frame;
  memory.Read(context, context.esp, frame.data(), sizeof(frame));
  std::array<uint32_t, 2> first, second;
  if (!frame[1] || !frame[2]) { throw Fault(context, 0, 8, "null counted string descriptor"); }
  memory.Read(context, frame[1], first.data(), sizeof(first));
  memory.Read(context, frame[2], second.data(), sizeof(second));
  const uint16_t length = uint16_t(first[0]);
  bool equal = length == uint16_t(second[0]);
  if (equal && length) {
    if (!first[1] || !second[1]) { throw Fault(context, 0, length, "null nonempty counted string buffer"); }
    memory.CheckRange(context, first[1], length);
    memory.CheckRange(context, second[1], length);
    const bool ignoreCase = uint8_t(frame[3]) != 0;
    std::array<uint8_t, 4096> firstBytes, secondBytes;
    uint32_t offset = 0;
    while (offset < length) {
      const uint32_t count = std::min(uint32_t(firstBytes.size()), uint32_t(length) - offset);
      memory.Read(context, first[1] + offset, firstBytes.data(), count);
      memory.Read(context, second[1] + offset, secondBytes.data(), count);
      for (uint32_t n = 0; n < count; ++n) {
        uint8_t left = firstBytes[n], right = secondBytes[n];
        if (ignoreCase) {
          if (left >= 0x80 || right >= 0x80) {
            throw Fault(context, 279, length, "unsupported non-ASCII ignore-case string equality");
          }
          if (left >= 'a' && left <= 'z') { left -= 'a' - 'A'; }
          if (right >= 'a' && right <= 'z') { right -= 'a' - 'A'; }
        }
        equal = equal && left == right;
      }
      offset += count;
      runtime.Checkpoint(context);
    }
  }
  runtime.Checkpoint(context);
  context.eax = equal;
  context.esp = uint32_t(stackEnd);
}

void InitializeAnsiString(Runtime& runtime, PreparedImage& image, X86Context& context, uint8_t* base) {
  if (!image.memory || !image.importsBound || image.boundRuntime != &runtime ||
      base != image.memory->Base() || !runtime.OwnsCurrentContext(context) || Memory::HasBorrow()) {
    throw Fault(context, 289, 0, "ANSI string initialization requires its owned image worker without a backing lease");
  }
  auto& memory = *image.memory;
  auto transition = memory.Mutate();
  runtime.Checkpoint(context);
  const uint64_t stackEnd = uint64_t(context.esp) + 12;
  if (stackEnd > UINT32_MAX) { throw Fault(context, context.esp, 12, "ANSI string initialization frame overflow"); }
  std::array<uint32_t, 3> frame;
  memory.Read(context, context.esp, frame.data(), sizeof(frame));
  const uint32_t destination = frame[1], source = frame[2];
  if (!destination) { throw Fault(context, destination, 8, "null ANSI_STRING destination"); }
  memory.CheckRange(context, destination, 8);
  uint32_t length = 0;
  if (source) {
    for (;;) {
      const uint64_t address = uint64_t(source) + length;
      if (address > UINT32_MAX) { throw Fault(context, source, length + 1, "ANSI string source wraps guest address space"); }
      uint8_t value;
      memory.Read(context, uint32_t(address), &value, 1);
      if (!value) { break; }
      if (length == UINT16_MAX - 1) {
        throw Fault(context, source, length + 1, "unsupported oversized ANSI string source");
      }
      ++length;
      if (!(length % 4096)) { runtime.Checkpoint(context); }
    }
    if (uint64_t(destination) < uint64_t(source) + length + 1 && uint64_t(source) < uint64_t(destination) + 8) {
      throw Fault(context, destination, 8, "unsupported ANSI_STRING destination aliasing source");
    }
  }
  const uint32_t maximum = source ? length + 1 : 0;
  const std::array<uint32_t, 2> descriptor = {length | (maximum << 16), source};
  runtime.Checkpoint(context);
  memory.Write(context, destination, descriptor.data(), sizeof(descriptor));
  context.esp = uint32_t(stackEnd);
}

}

void InstallStringProvider(Runtime& runtime, PreparedImage& image) {
  runtime.RegisterKernel(316, [&runtime, &image](X86Context& context, uint8_t* base) {
    UpperChar(runtime, image, context, base);
  });
  runtime.RegisterKernel(279, [&runtime, &image](X86Context& context, uint8_t* base) {
    EqualString(runtime, image, context, base);
  });
  runtime.RegisterKernel(289, [&runtime, &image](X86Context& context, uint8_t* base) {
    InitializeAnsiString(runtime, image, context, base);
  });
}

}
