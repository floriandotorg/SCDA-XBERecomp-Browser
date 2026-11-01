#include "browser/dispatcher-object-abi.h"
#include <bit>

namespace browser {
namespace {

void WriteWord(Memory& memory, const X86Context& ctx, uint32_t address, uint32_t value) {
  std::array<uint8_t, 4> bytes{};
  for (size_t n = 0; n < bytes.size(); ++n) { bytes[n] = uint8_t(value >> (n * 8)); }
  memory.Write(ctx, address, bytes.data(), bytes.size());
}

}

void InvokeDispatcherObject(DispatcherHandles& handles, Memory& memory, X86Context& ctx, uint32_t ordinal) {
  if (Memory::HasBorrow()) { throw Fault(ctx, ctx.esp, 0, "dispatcher object ABI requires no inherited backing lease"); }
  size_t arguments;
  switch (ordinal) {
    case 186: case 187: arguments = 1; break;
    case 185: case 209: case 213: case 214: case 221: case 225: arguments = 2; break;
    case 192: case 194: case 197: case 222: arguments = 3; break;
    case 189: case 193: arguments = 4; break;
    case 229: arguments = 8; break;
    default: throw Fault(ctx, ordinal, 0, "unsupported dispatcher object ordinal");
  }
  const uint64_t stackEnd = uint64_t(ctx.esp) + (arguments + 1) * 4;
  if (stackEnd > UINT32_MAX) { throw Fault(ctx, ctx.esp, (arguments + 1) * 4, "dispatcher object stack cleanup overflow"); }
  auto lease = memory.Borrow();
  std::array<uint8_t, 36> bytes{};
  memory.Read(ctx, ctx.esp, bytes.data(), (arguments + 1) * 4);
  std::array<uint32_t, 9> frame{};
  for (size_t n = 0; n <= arguments; ++n) {
    for (size_t byte = 0; byte < 4; ++byte) { frame[n] |= uint32_t(bytes[n * 4 + byte]) << (byte * 8); }
  }
  const bool creating = ordinal == 189 || ordinal == 192 || ordinal == 193 || ordinal == 194;
  const bool duplicating = ordinal == 197;
  const bool querying = ordinal == 209 || ordinal == 213 || ordinal == 214;
  const uint32_t output = creating ? frame[1] : (duplicating ? frame[2] : (arguments > 1 ? frame[arguments] : 0));
  if ((creating || duplicating) && !output) { throw Fault(ctx, output, 4, "null dispatcher handle output"); }
  if (querying && !output) { throw Fault(ctx, output, 8, "null dispatcher information output"); }
  if (duplicating && frame[3] > 1) { throw Fault(ctx, frame[3], 0, "unsupported dispatcher duplication options"); }
  if (creating) {
    if (frame[2]) { throw Fault(ctx, frame[2], 0, "dispatcher object attributes are not implemented"); }
    if ((ordinal == 189 || ordinal == 194) && frame[3] > 1) { throw Fault(ctx, frame[3], 0, "invalid guest dispatcher type"); }
    if (ordinal == 192 && (frame[3] & 0xFF) && !ctx.thread_handle) {
      throw Fault(ctx, 0, 0, "initial mutant ownership requires a nonzero context owner");
    }
  }
  int64_t dueTime = 0;
  if (ordinal == 229) {
    if (frame[3] || frame[4] > 1 || frame[5] || (frame[6] & 0xFF)) {
      throw Fault(ctx, ordinal, 0, "timer APC, context and resume behavior are not implemented or mode is invalid");
    }
    if (!frame[2]) { throw Fault(ctx, 0, 8, "null guest timer due time"); }
    std::array<uint8_t, 8> dueBytes{};
    memory.Read(ctx, frame[2], dueBytes.data(), dueBytes.size());
    uint64_t bits = 0;
    for (size_t n = 0; n < dueBytes.size(); ++n) { bits |= uint64_t(dueBytes[n]) << (n * 8); }
    dueTime = std::bit_cast<int64_t>(bits);
  }
  const bool byteOutput = ordinal == 185 || ordinal == 229;
  if (output) { memory.CheckRange(ctx, output, querying ? 8 : (byteOutput ? 1 : 4)); }
  uint32_t result = 0;
  uint32_t additional = 0;
  switch (ordinal) {
    case 185: result = handles.CancelTimerState(frame[1]).signaled; break;
    case 186: handles.ResetEvent(frame[1]); break;
    case 187: handles.Close(frame[1]); break;
    case 189: result = handles.CreateEvent(frame[3] == 0, (frame[4] & 0xFF) != 0); break;
    case 192: result = handles.CreateMutant((frame[3] & 0xFF) ? ctx.thread_handle : 0); break;
    case 193: result = handles.CreateSemaphore(std::bit_cast<int32_t>(frame[3]), std::bit_cast<int32_t>(frame[4])); break;
    case 194: result = handles.CreateTimer(frame[3] == 0); break;
    case 197: result = handles.Duplicate(frame[1], frame[3] == 1); break;
    case 209: {
      const auto information = handles.QueryEvent(frame[1]);
      result = information.notification ? 0 : 1;
      additional = uint32_t(information.signal);
      break;
    }
    case 213: {
      const auto information = handles.QueryMutant(frame[1], ctx.thread_handle);
      result = uint32_t(information.count);
      additional = uint32_t(information.ownedByCaller) | (uint32_t(information.abandoned) << 8);
      break;
    }
    case 214: {
      const auto information = handles.QuerySemaphore(frame[1]);
      result = uint32_t(information.count);
      additional = uint32_t(information.limit);
      break;
    }
    case 221: result = uint32_t(handles.ReleaseMutant(frame[1], ctx.thread_handle)); break;
    case 222: result = uint32_t(handles.ReleaseSemaphore(frame[1], std::bit_cast<int32_t>(frame[2]))); break;
    case 225: result = uint32_t(handles.SetEvent(frame[1])); break;
    case 229: result = handles.SetTimerGuestState(frame[1], dueTime, std::bit_cast<int32_t>(frame[7])).signaled; break;
  }
  if (output) {
    if (byteOutput) {
      const uint8_t value = uint8_t(result);
      memory.Write(ctx, output, &value, 1);
    } else {
      WriteWord(memory, ctx, output, result);
      if (querying) { WriteWord(memory, ctx, output + 4, additional); }
    }
  }
  ctx.eax = 0;
  ctx.esp = uint32_t(stackEnd);
}

}
