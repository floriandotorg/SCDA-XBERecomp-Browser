#include "browser/kernel-provider.h"
#include "kernel/crypto.h"
#include <algorithm>
#include <array>
#include <bit>

namespace browser {
namespace {

using CriticalSection = std::array<uint32_t, 7>;

class GuestPool {
  static constexpr uint32_t base = KernelPoolBase;
  static constexpr uint32_t capacity = KernelPoolCapacity;
  static constexpr size_t allocationLimit = 4096;
  struct Block {
    uint32_t address;
    uint32_t size;
    bool allocated;
    uint32_t tag;
  };
  std::vector<Block> blocks_;
  size_t allocations_ = 0;
public:
  explicit GuestPool(Memory& memory) {
    blocks_.reserve(allocationLimit * 2 + 1);
    blocks_.push_back({base, capacity, false, 0});
    memory.Map(base, capacity);
    memory.Commit(base, capacity);
  }

  void Invoke(Runtime& runtime, Memory& memory, X86Context& ctx, uint32_t ordinal) {
    if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "pool requires no inherited backing lease"); }
    const uint32_t frameBytes = ordinal == 15 ? 12 : 8;
    const uint64_t stackEnd = uint64_t(ctx.esp) + frameBytes;
    if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, frameBytes, "pool stack cleanup overflow"); }
    auto lease = memory.Borrow();
    std::array<uint32_t, 3> frame{};
    memory.Read(ctx, ctx.esp, frame.data(), frameBytes);
    runtime.Checkpoint(ctx);
    uint32_t result = 0;
    if (ordinal == 15) {
      if (frame[1] <= capacity && allocations_ < allocationLimit) {
        const uint32_t size = std::max(16u, (frame[1] + 15) & ~15u);
        for (size_t n = 0; n < blocks_.size(); ++n) {
          auto& block = blocks_[n];
          if (block.allocated || block.size < size) { continue; }
          auto* bytes = memory.Span(ctx, block.address, size);
          const uint32_t remainder = block.size - size;
          result = block.address;
          block = {result, size, true, frame[2]};
          if (remainder) { blocks_.insert(blocks_.begin() + n + 1, {result + size, remainder, false, 0}); }
          ++allocations_;
          std::fill_n(bytes, size, uint8_t(0));
          break;
        }
      }
    } else {
      size_t index = 0;
      while (index < blocks_.size() && (!blocks_[index].allocated || blocks_[index].address != frame[1])) { ++index; }
      if (ordinal == 23) {
        if (index < blocks_.size()) { result = blocks_[index].size; }
      } else {
        if (index == blocks_.size()) { throw Fault(ctx, frame[1], 0, "pool free requires a live allocation base"); }
        blocks_[index].allocated = false;
        blocks_[index].tag = 0;
        --allocations_;
        if (index + 1 < blocks_.size() && !blocks_[index + 1].allocated) {
          blocks_[index].size += blocks_[index + 1].size;
          blocks_.erase(blocks_.begin() + index + 1);
        }
        if (index && !blocks_[index - 1].allocated) {
          blocks_[index - 1].size += blocks_[index].size;
          blocks_.erase(blocks_.begin() + index);
        }
      }
    }
    ctx.eax = result;
    ctx.esp = uint32_t(stackEnd);
  }
};

CriticalSection ReadSection(Memory& memory, const X86Context& ctx, uint32_t address) {
  if (!address || address % 4) { throw Fault(ctx, address, 28, "invalid critical section address"); }
  CriticalSection section;
  memory.Read(ctx, address, section.data(), sizeof(section));
  return section;
}

void CheckSection(const X86Context& ctx, uint32_t address, const CriticalSection& section) {
  const int32_t count = std::bit_cast<int32_t>(section[4]);
  if (section[0] != 0x40001 || section[1] > 1 || section[2] != address + 8 || section[3] != address + 8 ||
      count < -1 || section[5] > INT32_MAX || (section[5] == 0) != (section[6] == 0) ||
      int64_t(count) < int64_t(section[5]) - 1) {
    throw Fault(ctx, address, 28, "unsupported or corrupt critical section state");
  }
}

void InvokePerformanceCounter(Runtime& runtime, Memory& memory, X86Context& ctx) {
  if (!runtime.OwnsCurrentContext(ctx) || Memory::HasBorrow()) { throw Fault(ctx, 126, 0, "performance counter requires its current owned context without a backing lease"); }
  const uint64_t stackEnd = uint64_t(ctx.esp) + 4;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 4, "performance counter stack cleanup overflow"); }
  auto lease = memory.Borrow();
  uint32_t returnAddress;
  memory.Read(ctx, ctx.esp, &returnAddress, sizeof(returnAddress));
  const uint64_t elapsed = runtime.TimerTime(ctx);
  const uint64_t counter = elapsed / 80 * 27 + elapsed % 80 * 27 / 80;
  ctx.eax = uint32_t(counter);
  ctx.edx = uint32_t(counter >> 32);
  ctx.esp = uint32_t(stackEnd);
}

void InvokeSystemTime(Runtime& runtime, Memory& memory, X86Context& ctx) {
  if (!runtime.OwnsCurrentContext(ctx) || Memory::HasBorrow()) { throw Fault(ctx, 128, 0, "system time requires its current owned context without a backing lease"); }
  const uint64_t stackEnd = uint64_t(ctx.esp) + 8;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 8, "system time stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 2> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  if (!frame[1]) { throw Fault(ctx, frame[1], 8, "null system time output"); }
  memory.CheckRange(ctx, frame[1], 8);
  runtime.Checkpoint(ctx);
  const uint64_t time = Runtime::SystemTime();
  memory.Write(ctx, frame[1], &time, sizeof(time));
  ctx.esp = uint32_t(stackEnd);
}

void InvokeNonCachingFscPolicy(Runtime& runtime, Memory& memory, X86Context& ctx) {
  if (!runtime.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, 37, 0, "non-caching FSC policy requires its current owned context without a backing lease");
  }
  const uint64_t stackEnd = uint64_t(ctx.esp) + 8;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 8, "FSC policy stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 2> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  runtime.Checkpoint(ctx);
  ctx.eax = 0;
  ctx.esp = uint32_t(stackEnd);
}

void InvokeOfflineLinkState(Runtime& runtime, Memory& memory, X86Context& ctx) {
  const uint64_t stackEnd = uint64_t(ctx.esp) + 8;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 8, "link state stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 2> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  runtime.Checkpoint(ctx);
  ctx.eax = 0;
  ctx.esp = uint32_t(stackEnd);
}

void InvokeOfflinePhyInitialization(Runtime& runtime, Memory& memory, X86Context& ctx) {
  constexpr uint32_t hardwareUnavailable = 0x801f0001;
  const uint64_t stackEnd = uint64_t(ctx.esp) + 12;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 12, "PHY initialization stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 3> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  runtime.Checkpoint(ctx);
  ctx.eax = hardwareUnavailable;
  ctx.esp = uint32_t(stackEnd);
}

uint32_t TranslateStatus(uint32_t status) {
  switch (status) {
    case 0: return 0;
    case 0x103: return 997;
    case 0x102: return 1460;
    case 0xc000000f:
    case 0xc0000034: return 2;
    case 0xc000003a: return 3;
    case 0xc0000022:
    case 0xc0000121:
    case 0xc00000ba: return 5;
    case 0xc0000008: return 6;
    case 0xc0000017: return 8;
    case 0x80000006: return 18;
    case 0xc0000011: return 38;
    case 0xc00000bb: return 50;
    case 0xc000000d: return 87;
    case 0xc0000002: return 120;
    case 0xc0000023: return 122;
    case 0xc0000033: return 123;
    case 0xc0000035: return 183;
    case 0xc0000103: return 267;
    case 0xc000007f: return 112;
    case 0xc000009d: return 1167;
    default: return 317;
  }
}

void InvokeStatusTranslation(Runtime& runtime, Memory& memory, X86Context& ctx) {
  const uint64_t stackEnd = uint64_t(ctx.esp) + 8;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 8, "status translation stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 2> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  runtime.Checkpoint(ctx);
  ctx.eax = TranslateStatus(frame[1]);
  ctx.esp = uint32_t(stackEnd);
}

void InvokeDiagnosticVirtualDisplayQuery(Runtime& runtime, Memory& memory, X86Context& ctx) {
  if (!runtime.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, 2, 0, "virtual display query requires its current owned context without a backing lease");
  }
  const uint64_t stackEnd = uint64_t(ctx.esp) + 20;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 20, "virtual display query stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 5> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  if (frame[1] || frame[2] != 6 || frame[3]) {
    throw Fault(ctx, 2, 0, "only diagnostic virtual display option 6 with zero register base and parameter is supported; physical encoder operations are unavailable");
  }
  const uint32_t output = frame[4];
  if (!output || output % 4) { throw Fault(ctx, output, 4, "virtual display query requires an aligned non-null output"); }
  if (uint64_t(output) < stackEnd && uint64_t(output) + 4 > ctx.esp) {
    throw Fault(ctx, output, 4, "virtual display query output overlaps its input frame");
  }
  memory.CheckRange(ctx, output, 4);
  runtime.Checkpoint(ctx);
  constexpr uint32_t diagnosticVirtualDisplayCompatibilityProfile = KernelVideoPack | 0x00000100 | 0x00400000 | KernelVideoFlags;
  memory.Write(ctx, output, &diagnosticVirtualDisplayCompatibilityProfile, 4);
  ctx.esp = uint32_t(stackEnd);
}

void InvokeSettings(Runtime& runtime, Memory& memory, X86Context& ctx, const std::array<uint32_t, 3>& settings) {
  const uint64_t stackEnd = uint64_t(ctx.esp) + 24;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 24, "settings stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 6> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  uint32_t status = 0xc00000bb;
  if (frame[1] >= 7 && frame[1] <= 9) {
    const uint32_t value = settings[frame[1] - 7];
    const uint32_t type = 4;
    const uint32_t required = 4;
    const bool fits = frame[4] >= required;
    if (frame[2]) { memory.CheckRange(ctx, frame[2], 4); }
    if (frame[5]) { memory.CheckRange(ctx, frame[5], 4); }
    if (fits) {
      if (!frame[3]) { throw Fault(ctx, 0, 4, "settings value output is null"); }
      memory.CheckRange(ctx, frame[3], 4);
    }
    runtime.Checkpoint(ctx);
    if (fits) { memory.Write(ctx, frame[3], &value, 4); }
    if (frame[2]) { memory.Write(ctx, frame[2], &type, 4); }
    if (frame[5]) { memory.Write(ctx, frame[5], &required, 4); }
    status = fits ? 0 : 0xc0000023;
  } else {
    runtime.Checkpoint(ctx);
  }
  ctx.eax = status;
  ctx.esp = uint32_t(stackEnd);
}

void InvokeDesKeyParity(Runtime& runtime, Memory& memory, X86Context& ctx) {
  const uint64_t stackEnd = uint64_t(ctx.esp) + 12;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 12, "DES parity stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 3> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  if (frame[2]) {
    if (!frame[1]) { throw Fault(ctx, 0, frame[2], "DES parity buffer is null"); }
    memory.CheckRange(ctx, frame[1], frame[2]);
  }
  runtime.Checkpoint(ctx);
  std::array<uint8_t, 256> bytes;
  uint32_t offset = 0;
  while (offset < frame[2]) {
    const uint32_t count = std::min(uint32_t(bytes.size()), frame[2] - offset);
    memory.Read(ctx, frame[1] + offset, bytes.data(), count);
    for (uint32_t n = 0; n < count; ++n) {
      uint8_t parity = bytes[n];
      parity ^= parity >> 4;
      parity ^= parity >> 2;
      parity ^= parity >> 1;
      bytes[n] ^= (parity & 1) ^ 1;
    }
    memory.Write(ctx, frame[1] + offset, bytes.data(), count);
    offset += count;
  }
  ctx.esp = uint32_t(stackEnd);
}

void InvokeSha(Runtime& runtime, Memory& memory, X86Context& ctx, uint32_t ordinal) {
  if (!runtime.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, ordinal, 0, "SHA requires its current owned context without a backing lease");
  }
  const uint32_t frameBytes = ordinal == 335 ? 8 : ordinal == 336 ? 16 : 12;
  const uint64_t stackEnd = uint64_t(ctx.esp) + frameBytes;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, frameBytes, "SHA stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 4> frame{};
  memory.Read(ctx, ctx.esp, frame.data(), frameBytes);
  if (!frame[1]) { throw Fault(ctx, 0, KernelShaContextBytes, "SHA context output is null"); }
  if (uint64_t(frame[1]) < stackEnd && uint64_t(frame[1]) + KernelShaContextBytes > ctx.esp) {
    throw Fault(ctx, frame[1], KernelShaContextBytes, "SHA context output overlaps call frame");
  }
  memory.CheckRange(ctx, frame[1], KernelShaContextBytes);
  std::array<uint8_t, KernelShaContextBytes> context{};
  std::array<uint8_t, 20> digest{};
  if (ordinal == 335) {
    crypto::ShaInit(context.data());
  } else {
    memory.Read(ctx, frame[1], context.data(), context.size());
    if (ordinal == 336) {
      if (frame[3] && !frame[2]) { throw Fault(ctx, 0, frame[3], "SHA input is null"); }
      if (frame[3]) { memory.CheckRange(ctx, frame[2], frame[3]); }
      std::array<uint8_t, 4096> input;
      for (uint32_t n = 0; n < frame[3];) {
        runtime.Checkpoint(ctx);
        const uint32_t count = std::min<uint32_t>(input.size(), frame[3] - n);
        memory.Read(ctx, frame[2] + n, input.data(), count);
        crypto::ShaUpdate(context.data(), input.data(), count);
        n += count;
      }
    } else {
      if (!frame[2]) { throw Fault(ctx, 0, digest.size(), "SHA digest output is null"); }
      if (uint64_t(frame[2]) < stackEnd && uint64_t(frame[2]) + digest.size() > ctx.esp) {
        throw Fault(ctx, frame[2], digest.size(), "SHA digest output overlaps call frame");
      }
      memory.CheckRange(ctx, frame[2], digest.size());
      crypto::ShaFinal(context.data(), digest.data());
    }
  }
  runtime.Checkpoint(ctx);
  memory.Write(ctx, frame[1], context.data(), context.size());
  if (ordinal == 337) { memory.Write(ctx, frame[2], digest.data(), digest.size()); }
  ctx.esp = uint32_t(stackEnd);
}

void InvokeRc4Key(Runtime& runtime, Memory& memory, X86Context& ctx) {
  if (!runtime.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, 338, 0, "RC4 requires its current owned context without a backing lease");
  }
  constexpr uint32_t frameBytes = 16;
  constexpr uint32_t stateBytes = 258;
  const uint64_t stackEnd = uint64_t(ctx.esp) + frameBytes;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, frameBytes, "RC4 stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 4> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  if (!frame[1]) { throw Fault(ctx, 0, stateBytes, "RC4 state output is null"); }
  if (uint64_t(frame[1]) < stackEnd && uint64_t(frame[1]) + stateBytes > ctx.esp) {
    throw Fault(ctx, frame[1], stateBytes, "RC4 state output overlaps call frame");
  }
  if (!frame[2] || frame[2] > 256) { throw Fault(ctx, frame[3], frame[2], "RC4 key length must be between 1 and 256"); }
  if (!frame[3]) { throw Fault(ctx, 0, frame[2], "RC4 key is null"); }
  memory.CheckRange(ctx, frame[1], stateBytes);
  memory.CheckRange(ctx, frame[3], frame[2]);
  std::array<uint8_t, 256> key;
  std::array<uint8_t, stateBytes> state;
  memory.Read(ctx, frame[3], key.data(), frame[2]);
  crypto::Rc4Key(state.data(), key.data(), frame[2]);
  runtime.Checkpoint(ctx);
  memory.Write(ctx, frame[1], state.data(), state.size());
  ctx.esp = uint32_t(stackEnd);
}

void InvokeRc4Crypt(Runtime& runtime, Memory& memory, X86Context& ctx) {
  if (!runtime.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, 339, 0, "RC4 requires its current owned context without a backing lease");
  }
  constexpr uint32_t frameBytes = 16;
  constexpr uint32_t stateBytes = 258;
  constexpr uint32_t inputLimit = 1u << 20;
  const uint64_t stackEnd = uint64_t(ctx.esp) + frameBytes;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, frameBytes, "RC4 stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 4> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  if (!frame[1]) { throw Fault(ctx, 0, stateBytes, "RC4 state is null"); }
  if (uint64_t(frame[1]) < stackEnd && uint64_t(frame[1]) + stateBytes > ctx.esp) {
    throw Fault(ctx, frame[1], stateBytes, "RC4 state overlaps call frame");
  }
  if (frame[2] > inputLimit) { throw Fault(ctx, frame[3], frame[2], "RC4 input exceeds limit"); }
  if (frame[2] && !frame[3]) { throw Fault(ctx, 0, frame[2], "RC4 input is null"); }
  if (frame[2] && uint64_t(frame[3]) < stackEnd && uint64_t(frame[3]) + frame[2] > ctx.esp) {
    throw Fault(ctx, frame[3], frame[2], "RC4 input overlaps call frame");
  }
  if (frame[2] && uint64_t(frame[3]) < uint64_t(frame[1]) + stateBytes &&
      uint64_t(frame[1]) < uint64_t(frame[3]) + frame[2]) {
    throw Fault(ctx, frame[3], frame[2], "RC4 input overlaps state");
  }
  memory.CheckRange(ctx, frame[1], stateBytes);
  if (frame[2]) { memory.CheckRange(ctx, frame[3], frame[2]); }
  std::array<uint8_t, stateBytes> state;
  std::vector<uint8_t> data(frame[2]);
  memory.Read(ctx, frame[1], state.data(), state.size());
  if (frame[2]) { memory.Read(ctx, frame[3], data.data(), data.size()); }
  for (uint32_t n = 0; n < frame[2];) {
    runtime.Checkpoint(ctx);
    const uint32_t count = std::min(4096u, frame[2] - n);
    crypto::Rc4Crypt(state.data(), data.data() + n, count);
    n += count;
  }
  runtime.Checkpoint(ctx);
  memory.Write(ctx, frame[1], state.data(), state.size());
  if (frame[2]) { memory.Write(ctx, frame[3], data.data(), data.size()); }
  ctx.esp = uint32_t(stackEnd);
}

void InvokeHmac(Runtime& runtime, Memory& memory, X86Context& ctx) {
  const uint64_t stackEnd = uint64_t(ctx.esp) + 32;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 32, "HMAC stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint32_t, 8> frame;
  memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
  frame[2] = std::min(frame[2], 64u);
  for (const uint32_t n : {1u, 3u, 5u}) {
    if (frame[n + 1]) {
      if (!frame[n]) { throw Fault(ctx, 0, frame[n + 1], "HMAC input is null"); }
      memory.CheckRange(ctx, frame[n], frame[n + 1]);
    }
  }
  if (!frame[7]) { throw Fault(ctx, 0, 20, "HMAC digest output is null"); }
  memory.CheckRange(ctx, frame[7], 20);
  runtime.Checkpoint(ctx);
  std::array<uint8_t, 64> key{};
  if (frame[2]) { memory.Read(ctx, frame[1], key.data(), frame[2]); }
  std::array<uint8_t, 64> innerPad, outerPad;
  for (uint32_t n = 0; n < key.size(); ++n) {
    innerPad[n] = key[n] ^ 0x36;
    outerPad[n] = key[n] ^ 0x5c;
  }
  std::array<uint8_t, 116> context;
  std::array<uint8_t, 256> bytes;
  std::array<uint8_t, 20> digest;
  crypto::ShaInit(context.data());
  crypto::ShaUpdate(context.data(), innerPad.data(), innerPad.size());
  for (const uint32_t n : {3u, 5u}) {
    uint32_t offset = 0;
    while (offset < frame[n + 1]) {
      runtime.Checkpoint(ctx);
      const uint32_t count = std::min(uint32_t(bytes.size()), frame[n + 1] - offset);
      memory.Read(ctx, frame[n] + offset, bytes.data(), count);
      crypto::ShaUpdate(context.data(), bytes.data(), count);
      offset += count;
    }
  }
  crypto::ShaFinal(context.data(), digest.data());
  crypto::ShaInit(context.data());
  crypto::ShaUpdate(context.data(), outerPad.data(), outerPad.size());
  crypto::ShaUpdate(context.data(), digest.data(), digest.size());
  crypto::ShaFinal(context.data(), digest.data());
  runtime.Checkpoint(ctx);
  memory.Write(ctx, frame[7], digest.data(), digest.size());
  ctx.esp = uint32_t(stackEnd);
}

void InvokeIrql(Runtime& runtime, Memory& memory, X86Context& ctx, uint32_t ordinal) {
  const uint64_t stackEnd = uint64_t(ctx.esp) + 4;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 4, "IRQL stack cleanup overflow"); }
  if (!ctx.fs_base || ctx.fs_base % 4 || uint64_t(ctx.fs_base) + 0x24 > UINT32_MAX) {
    throw Fault(ctx, ctx.fs_base, 0x25, "invalid IRQL KPCR address");
  }
  auto lease = memory.Borrow();
  uint32_t returnAddress;
  memory.Read(ctx, ctx.esp, &returnAddress, 4);
  uint8_t current;
  const uint32_t address = ctx.fs_base + 0x24;
  memory.Read(ctx, address, &current, 1);
  const bool lower = ordinal == 161;
  const uint8_t requested = ordinal == 129 ? 2 : uint8_t(ctx.ecx);
  if (current > 31 || requested > 31 || (lower ? requested > current : requested < current)) {
    throw Fault(ctx, address, 1, "invalid IRQL transition");
  }
  runtime.Checkpoint(ctx);
  memory.Write(ctx, address, &requested, 1);
  ctx.eax = lower ? 0 : current;
  ctx.esp = uint32_t(stackEnd);
}

void InvokeCriticalSection(Runtime& runtime, Memory& memory, X86Context& ctx, uint32_t ordinal) {
  if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "critical section requires no inherited backing lease"); }
  const uint64_t stackEnd = uint64_t(ctx.esp) + 8;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, 8, "critical section stack cleanup overflow"); }
  uint32_t address;
  uint32_t owner = 0;
  {
    auto lease = memory.Borrow();
    std::array<uint32_t, 2> frame;
    memory.Read(ctx, ctx.esp, frame.data(), sizeof(frame));
    address = frame[1];
    if (ordinal != 291) {
      if (!ctx.fs_base || uint64_t(ctx.fs_base) + 0x2c > UINT32_MAX) { throw Fault(ctx, ctx.fs_base, 0x2c, "missing critical section thread identity"); }
      memory.Read(ctx, ctx.fs_base + 0x28, &owner, 4);
      if (!owner) { throw Fault(ctx, ctx.fs_base + 0x28, 4, "missing critical section ETHREAD"); }
      memory.CheckRange(ctx, owner, 1);
    }
  }
  bool reserved = false;
  try {
    for (;;) {
      uint64_t sequence;
      {
        auto lease = memory.Borrow();
        runtime.Checkpoint(ctx);
        auto section = ReadSection(memory, ctx, address);
        if (ordinal == 291) {
          if (section[0] == 0x40001 && (section[5] || section[6] || section[4] != UINT32_MAX)) {
            throw Fault(ctx, address, 28, "cannot initialize an active critical section");
          }
          section = {0x40001, 0, address + 8, address + 8, UINT32_MAX, 0, 0};
        } else {
          CheckSection(ctx, address, section);
          if (ordinal == 294) {
            if (section[6] != owner || !section[5]) { throw Fault(ctx, address, 28, "critical section release by nonowner"); }
            --section[5];
            --section[4];
            if (!section[5]) {
              section[6] = 0;
              section[1] = section[4] != UINT32_MAX;
            }
          } else {
            if (!reserved) {
              if (section[4] == INT32_MAX) { throw Fault(ctx, address, 28, "critical section lock count overflow"); }
              ++section[4];
              reserved = true;
              if (section[4] == 0 || section[6] == owner) {
                section[6] = owner;
                ++section[5];
                reserved = false;
              }
            }
            if (reserved && !section[6] && section[1]) {
              section[1] = 0;
              section[6] = owner;
              section[5] = 1;
              reserved = false;
            }
          }
        }
        memory.Write(ctx, address, section.data(), sizeof(section));
        if (!reserved) {
          if (ordinal == 294 && !section[5] && section[1]) { runtime.Wake(); }
          ctx.eax = 0;
          ctx.esp = uint32_t(stackEnd);
          return;
        }
        sequence = runtime.Sequence();
      }
      runtime.Wait(ctx, sequence, std::chrono::milliseconds(100));
    }
  } catch (...) {
    if (reserved) {
      auto lease = memory.Borrow();
      auto section = ReadSection(memory, ctx, address);
      CheckSection(ctx, address, section);
      --section[4];
      if (section[4] == UINT32_MAX) { section[1] = 0; }
      memory.Write(ctx, address, section.data(), sizeof(section));
    }
    throw;
  }
}

}

void InstallKernelProvider(Runtime& runtime, Memory& memory) {
  if (runtime.ActiveWorkers()) { throw Fault({}, 0, 0, "kernel provider installation requires a stopped runtime"); }
  runtime.Checkpoint({});
  auto pool = std::make_shared<GuestPool>(memory);
  const std::array<uint32_t, 3> settings = {1, KernelVideoFlags, 0};
  for (const uint32_t ordinal : {2u, 15u, 17u, 23u, 24u, 37u, 126u, 128u, 129u, 160u, 161u, 252u, 253u, 277u, 291u, 294u, 301u, 335u, 336u, 337u, 338u, 339u, 340u, 346u}) {
    runtime.RegisterKernel(ordinal, [&runtime, &memory, ordinal, pool, settings](X86Context& ctx, uint8_t* base) {
      if (&Runtime::From(ctx) != &runtime || base != memory.Base()) { throw Fault(ctx, ordinal, 0, "kernel provider session mismatch"); }
      if (ordinal == 2) { InvokeDiagnosticVirtualDisplayQuery(runtime, memory, ctx); }
      else if (ordinal == 15 || ordinal == 17 || ordinal == 23) { pool->Invoke(runtime, memory, ctx, ordinal); }
      else if (ordinal == 24) { InvokeSettings(runtime, memory, ctx, settings); }
      else if (ordinal == 37) { InvokeNonCachingFscPolicy(runtime, memory, ctx); }
      else if (ordinal == 126) { InvokePerformanceCounter(runtime, memory, ctx); }
      else if (ordinal == 128) { InvokeSystemTime(runtime, memory, ctx); }
      else if (ordinal == 129 || ordinal == 160 || ordinal == 161) { InvokeIrql(runtime, memory, ctx, ordinal); }
      else if (ordinal == 252) { InvokeOfflineLinkState(runtime, memory, ctx); }
      else if (ordinal == 253) { InvokeOfflinePhyInitialization(runtime, memory, ctx); }
      else if (ordinal == 301) { InvokeStatusTranslation(runtime, memory, ctx); }
      else if (ordinal == 335 || ordinal == 336 || ordinal == 337) { InvokeSha(runtime, memory, ctx, ordinal); }
      else if (ordinal == 338) { InvokeRc4Key(runtime, memory, ctx); }
      else if (ordinal == 339) { InvokeRc4Crypt(runtime, memory, ctx); }
      else if (ordinal == 340) { InvokeHmac(runtime, memory, ctx); }
      else if (ordinal == 346) { InvokeDesKeyParity(runtime, memory, ctx); }
      else { InvokeCriticalSection(runtime, memory, ctx, ordinal); }
    });
  }
}

}
