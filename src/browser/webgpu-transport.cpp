#include "webgpu-transport.h"
#include <cstring>
#include <string>
#include <mutex>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(uint32_t, webgpu_exchange_local, (uint32_t opcode, uint32_t resource, const uint8_t* input, uint32_t size, uint8_t* output, uint32_t* metadata, uint32_t posted), {
  opcode >>>= 0; resource >>>= 0; input >>>= 0; size >>>= 0; output >>>= 0; metadata >>>= 0;
  const mailbox = Module.webgpuMailbox;
  if (!mailbox) { return 4; }
  const header = new Int32Array(mailbox, 0, 16);
  const bytes = new Uint8Array(mailbox, 64);
  const close = status => { header[5] = 0; header[6] = status; header[7] = status; Atomics.store(header, 0, 4); Atomics.notify(header, 0); };
  const settle = (command, resource) => {
    const deadline = Date.now() + 30000;
    while (Atomics.load(header, 0) < 3) {
      if (Date.now() >= deadline) { close(2); break; }
      const state = Atomics.load(header, 0);
      Atomics.wait(header, 0, state, 1000);
    }
    const length = header[5];
    if (length < 0 || length > 4194304 || header[6] < 0 || header[6] > 3 || (header[1] >>> 0) !== command || (header[3] >>> 0) !== resource) { close(1); return undefined; }
    const terminal = Atomics.load(header, 0) === 4;
    return { status: terminal ? header[7] : header[6], length: terminal ? 0 : length, terminal };
  };
  const deliver = reply => {
    if (!reply.terminal) { HEAPU8.set(bytes.subarray(0, reply.length), output); }
    HEAPU32[metadata >> 2] = header[1];
    HEAPU32[(metadata >> 2) + 1] = reply.length;
    Atomics.compareExchange(header, 0, 3, 0);
    Atomics.notify(header, 0);
    return reply.status;
  };
  const ringOffset = 5 * (64 + 4194304), ringBytes = 16777216;
  const ring = mailbox.byteLength >= ringOffset + 1024 + ringBytes ? new Int32Array(mailbox, ringOffset, 8) : undefined;
  const failedRing = () => {
    const length = Math.min(Math.max(ring[4], 0), 960);
    HEAPU8.set(new Uint8Array(mailbox, ringOffset + 64, length), output);
    HEAPU32[metadata >> 2] = header[1];
    HEAPU32[(metadata >> 2) + 1] = length;
    return ring[3] || 1;
  };
  if (Atomics.load(header, 8)) {
    const reply = settle(header[1] >>> 0, header[9] >>> 0);
    Atomics.store(header, 8, 0);
    if (!reply) { return 1; }
    if (reply.status !== 0) { return deliver(reply); }
    Atomics.compareExchange(header, 0, 3, 0);
    Atomics.notify(header, 0);
  }
  if (Atomics.load(header, 0) === 4) { return header[7]; }
  if (ring && Atomics.load(ring, 2)) { return failedRing(); }
  if (ring && posted) {
    const data = new Uint8Array(mailbox, ringOffset + 1024, ringBytes);
    const words = new Int32Array(mailbox, ringOffset + 1024, ringBytes / 4);
    const need = 16 + ((size + 15) & ~15);
    const deadline = Date.now() + 30000;
    let write = ring[0] >>> 0, position = write % ringBytes, skip = ringBytes - position < need ? ringBytes - position : 0;
    for (;;) {
      const read = Atomics.load(ring, 1) >>> 0;
      if (ringBytes - ((write - read) >>> 0) >= need + skip) { break; }
      if (Atomics.load(header, 0) === 4) { return header[7]; }
      if (Atomics.load(ring, 2)) { return failedRing(); }
      if (Date.now() >= deadline) { close(2); return 2; }
      Atomics.wait(ring, 1, read | 0, 1000);
    }
    if (skip) {
      words[position / 4] = 0; words[position / 4 + 1] = 0; words[position / 4 + 2] = 0; words[position / 4 + 3] = skip - 16;
      write = (write + skip) >>> 0; position = 0;
    }
    const command = (header[1] >>> 0) + 1;
    if (command > 4294967295) { close(2); return 2; }
    header[1] = command;
    words[position / 4] = command; words[position / 4 + 1] = opcode; words[position / 4 + 2] = resource; words[position / 4 + 3] = size;
    data.set(HEAPU8.subarray(input, input + size), position + 16);
    Atomics.store(ring, 0, (write + need) | 0);
    Atomics.add(ring, 5, 1);
    Atomics.notify(ring, 5);
    HEAPU32[metadata >> 2] = command;
    HEAPU32[(metadata >> 2) + 1] = 0;
    return 0;
  }
  if (Atomics.compareExchange(header, 0, 0, 5) !== 0) { return 1; }
  bytes.set(HEAPU8.subarray(input, input + size));
  const command = (header[1] >>> 0) + 1;
  if (command > 4294967295) { header[7] = 2; Atomics.store(header, 0, 4); Atomics.notify(header, 0); return 2; }
  header[1] = command;
  header[2] = opcode;
  header[3] = resource;
  header[4] = size;
  header[5] = 0;
  if (posted) { header[9] = resource; Atomics.store(header, 8, 1); }
  Atomics.compareExchange(header, 0, 5, 1);
  Atomics.notify(header, 0);
  if (ring) { Atomics.add(ring, 5, 1); Atomics.notify(ring, 5); }
  if (posted) {
    HEAPU32[metadata >> 2] = command;
    HEAPU32[(metadata >> 2) + 1] = 0;
    return 0;
  }
  const reply = settle(command, resource);
  return reply ? deliver(reply) : 1;
});
EM_JS(uint32_t, webgpu_collect_local, (uint32_t slot, uint32_t resource, uint8_t* output, uint32_t* metadata, uint32_t poll), {
  slot >>>= 0; resource >>>= 0; output >>>= 0; metadata >>>= 0;
  const mailbox = Module.webgpuMailbox;
  if (!mailbox) { return 2; }
  const base = 64 + 4194304 + slot * (64 + 4194304);
  if (slot >= 4 || mailbox.byteLength < base + 64 + 4194304) { return 1; }
  const header = new Int32Array(mailbox, base, 4);
  HEAPU32[metadata >> 2] = 0;
  HEAPU32[(metadata >> 2) + 1] = 0;
  const deadline = Date.now() + 30000;
  for (;;) {
    if (Atomics.load(header, 0) === 2) {
      if ((header[1] >>> 0) === resource) { break; }
      Atomics.store(header, 0, 0);
    }
    if (poll) { return 0; }
    if (Date.now() >= deadline) { return 2; }
    Atomics.wait(header, 0, 0, 1000);
  }
  const length = header[2];
  if (length < 0 || length > 4194304) { return 1; }
  HEAPU8.set(new Uint8Array(mailbox, base + 64, length), output);
  HEAPU32[(metadata >> 2) + 1] = length;
  const status = header[3];
  Atomics.store(header, 0, 0);
  return status;
});
uint32_t webgpu_exchange(uint32_t opcode, uint32_t resource, const uint8_t* input, uint32_t size, uint8_t* output, uint32_t* metadata, bool posted) {
  const auto local = webgpu_exchange_local(opcode, resource, input, size, output, metadata, posted);
  if (local != 4) { return local; }
  return MAIN_THREAD_EM_ASM_INT({
    const result = webgpu_exchange_local($0, $1, $2, $3, $4, $5, 0);
    return result === 4 ? 2 : result;
  }, opcode, resource, input, size, output, metadata);
}
#endif

namespace browser::webgpu {
namespace {
Result Exchange(Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes, bool posted) {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  Result result{Status::Error, 0, resource, {}};
  if (bytes.size() > MaxBytes) { return result; }
#ifdef __EMSCRIPTEN__
  static std::vector<uint8_t> reply(MaxBytes);
  uint32_t metadata[2]{};
  if (opcode == Opcode::ReadbackCollect) {
    uint32_t slot = 0, poll = 0;
    if (bytes.size() != 8) { return result; }
    std::memcpy(&slot, bytes.data(), 4);
    std::memcpy(&poll, bytes.data() + 4, 4);
    const uint32_t status = webgpu_collect_local(slot, resource, reply.data(), metadata, poll);
    result.status = static_cast<Status>(status);
    if (status) {
      const std::string reason = status == 2 ? "asynchronous readback slot timed out" : "asynchronous readback slot failed";
      result.bytes.assign(reason.begin(), reason.end());
      return result;
    }
    result.bytes.assign(reply.begin(), reply.begin() + std::min<size_t>(metadata[1], reply.size()));
    return result;
  }
  result.status = static_cast<Status>(webgpu_exchange(static_cast<uint32_t>(opcode), resource, bytes.data(), bytes.size(), reply.data(), metadata, posted));
  result.command = metadata[0];
  if (metadata[1] > reply.size()) { return {Status::Error, 0, resource, {}}; }
  result.bytes.assign(reply.begin(), reply.begin() + metadata[1]);
#else
  (void)posted;
  result.status = Status::Closed;
#endif
  return result;
}
}

Result Transport::Execute(Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes) { return Exchange(opcode, resource, bytes, false); }
Result Transport::Post(Opcode opcode, uint32_t resource, std::span<const uint8_t> bytes) { return Exchange(opcode, resource, bytes, true); }
}
