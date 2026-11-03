#include "webgpu-transport.h"
#include <mutex>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

uint32_t webgpu_exchange(uint32_t opcode, uint32_t resource, const uint8_t* input, uint32_t size, uint8_t* output, uint32_t* metadata) {
  return MAIN_THREAD_EM_ASM_INT({
  const opcode = $0 >>> 0;
  const resource = $1 >>> 0;
  const input = $2 >>> 0;
  const size = $3 >>> 0;
  const output = $4 >>> 0;
  const metadata = $5 >>> 0;
  const mailbox = Module.webgpuMailbox;
  if (!mailbox) { return 2; }
  const header = new Int32Array(mailbox, 0, 16);
  const bytes = new Uint8Array(mailbox, 64);
  if (Atomics.load(header, 0) === 4) { return header[7]; }
  if (Atomics.compareExchange(header, 0, 0, 5) !== 0) { return 1; }
  bytes.set(HEAPU8.subarray(input, input + size));
  const command = (header[1] >>> 0) + 1;
  if (command > 4294967295) { header[7] = 2; Atomics.store(header, 0, 4); Atomics.notify(header, 0); return 2; }
  header[1] = command;
  header[2] = opcode;
  header[3] = resource;
  header[4] = size;
  header[5] = 0;
  Atomics.compareExchange(header, 0, 5, 1);
  Atomics.notify(header, 0);
  const deadline = Date.now() + 30000;
  while (Atomics.load(header, 0) < 3) {
    if (Date.now() >= deadline) { header[5] = 0; header[6] = 2; header[7] = 2; Atomics.store(header, 0, 4); Atomics.notify(header, 0); break; }
    const state = Atomics.load(header, 0);
    Atomics.wait(header, 0, state, 1000);
  }
  const length = header[5];
  if (length < 0 || length > 4194304 || header[6] < 0 || header[6] > 3 || (header[1] >>> 0) !== command || (header[3] >>> 0) !== resource) { header[5] = 0; header[6] = 1; header[7] = 1; Atomics.store(header, 0, 4); Atomics.notify(header, 0); return 1; }
  const terminal = Atomics.load(header, 0) === 4;
  if (!terminal) { HEAPU8.set(bytes.subarray(0, length), output); }
  HEAPU32[metadata >> 2] = header[1];
  HEAPU32[(metadata >> 2) + 1] = terminal ? 0 : length;
  const status = terminal ? header[7] : header[6];
  Atomics.compareExchange(header, 0, 3, 0);
  Atomics.notify(header, 0);
  return status;
  }, opcode, resource, input, size, output, metadata);
}
#endif

namespace browser::webgpu {
Result Transport::Execute(Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes) {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  Result result{Status::Error, 0, resource, {}};
  if (bytes.size() > MaxBytes) { return result; }
#ifdef __EMSCRIPTEN__
  result.bytes.resize(MaxBytes);
  uint32_t metadata[2]{};
  result.status = static_cast<Status>(webgpu_exchange(static_cast<uint32_t>(opcode), resource, bytes.data(), bytes.size(), result.bytes.data(), metadata));
  result.command = metadata[0];
  result.bytes.resize(metadata[1]);
#else
  result.status = Status::Closed;
#endif
  return result;
}
}
