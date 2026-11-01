#include "browser/dispatcher-wait-abi.h"

namespace browser {
namespace {

void ReadWords(Memory& memory, const X86Context& ctx, uint32_t address, std::span<uint32_t> words) {
  std::array<uint8_t, 256> bytes{};
  memory.Read(ctx, address, bytes.data(), words.size_bytes());
  for (size_t n = 0; n < words.size(); ++n) {
    words[n] = 0;
    for (size_t byte = 0; byte < 4; ++byte) { words[n] |= uint32_t(bytes[n * 4 + byte]) << (byte * 8); }
  }
}

}

void InvokeDispatcherWait(DispatcherHandles& handles, Memory& memory, X86Context& ctx,
  uint32_t ordinal, std::chrono::milliseconds watchdog) {
  if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "dispatcher wait ABI requires no backing lease"); }
  size_t arguments;
  switch (ordinal) {
    case 233: arguments = 3; break;
    case 234: arguments = 4; break;
    case 235: arguments = 6; break;
    default: throw Fault(ctx, ordinal, 0, "unsupported dispatcher wait ordinal");
  }
  const uint64_t stackEnd = uint64_t(ctx.esp) + (arguments + 1) * 4;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, (arguments + 1) * 4, "dispatcher wait stack cleanup overflow"); }
  std::array<uint32_t, 7> frame{};
  ReadWords(memory, ctx, ctx.esp, std::span(frame.data(), arguments + 1));
  const uint32_t alertable = frame[arguments - 1] & 0xFF;
  if (alertable) { throw Fault(ctx, 0, 0, "alertable guest waits are not implemented"); }
  if (ordinal != 233 && frame[arguments - 2] > 1) { throw Fault(ctx, 0, 0, "invalid guest wait processor mode"); }
  std::array<uint32_t, 64> guestHandles{};
  size_t count = 1;
  auto mode = Dispatcher::WaitMode::Any;
  if (ordinal == 235) {
    count = frame[1];
    if (count == 0 || count > guestHandles.size()) { throw Fault(ctx, frame[2], count, "invalid guest handle array count"); }
    if (frame[3] > 1) { throw Fault(ctx, 0, 0, "invalid guest wait type"); }
    if (frame[2] == 0) { throw Fault(ctx, 0, count * 4, "null guest handle array"); }
    mode = frame[3] == 0 ? Dispatcher::WaitMode::All : Dispatcher::WaitMode::Any;
    ReadWords(memory, ctx, frame[2], std::span(guestHandles.data(), count));
  } else {
    guestHandles[0] = frame[1];
  }
  const auto status = handles.WaitGuestStatus(memory, ctx, std::span(guestHandles.data(), count), mode,
    frame[arguments], false, watchdog);
  ctx.eax = status;
  ctx.esp = uint32_t(stackEnd);
}

}
