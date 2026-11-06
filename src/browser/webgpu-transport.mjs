export const MAX_BYTES = 4 * 1024 * 1024;
export const HEADER_BYTES = 64;
export const Opcode = Object.freeze({ Configure: 1, CreateTexture: 2, CreatePipeline: 3, Draw: 4, Readback: 5, Destroy: 6, Flush: 7, Shutdown: 8, Present: 9, Clear: 10, Copy: 11, SetGamma: 12, UploadDepth: 13, DrawBatch: 14, RenderBatch: 15, UploadBuffer: 16, UploadDrawBatch: 17, ReadbackStart: 18, ReadbackCollect: 19, Blit: 20, Multisample: 21 });
export const Status = Object.freeze({ Ok: 0, Error: 1, Closed: 2, Lost: 3 });
export const READBACK_SLOTS = 4;
export const RING_OFFSET = (READBACK_SLOTS + 1) * (HEADER_BYTES + MAX_BYTES);
export const RING_HEADER_BYTES = 1024;
export const RING_BYTES = 16 * 1024 * 1024;
export const createMailbox = () => new SharedArrayBuffer(RING_OFFSET + RING_HEADER_BYTES + RING_BYTES);
const ringHeader = mailbox => {
  const buffer = mailbox.buffer ?? mailbox;
  return buffer.byteLength >= RING_OFFSET + RING_HEADER_BYTES + RING_BYTES ? new Int32Array(buffer, RING_OFFSET, 8) : undefined;
};
export const ringDoorbell = mailbox => {
  const ring = ringHeader(mailbox);
  if (ring) { Atomics.add(ring, 5, 1); Atomics.notify(ring, 5); }
};
export const createCommandRing = mailbox => {
  const buffer = mailbox.buffer ?? mailbox;
  const ring = ringHeader(mailbox);
  if (!ring) { return undefined; }
  const data = new Uint8Array(buffer, RING_OFFSET + RING_HEADER_BYTES, RING_BYTES);
  const words = new Int32Array(buffer, RING_OFFSET + RING_HEADER_BYTES, RING_BYTES / 4);
  let read = Atomics.load(ring, 1) >>> 0;
  return {
    doorbell: () => Atomics.load(ring, 5),
    wait: async value => {
      if (typeof Atomics.waitAsync === 'function') { await Atomics.waitAsync(ring, 5, value).value; }
      else { await delay(); }
    },
    pending: () => (Atomics.load(ring, 0) >>> 0) !== read,
    bound: () => Atomics.load(ring, 0) >>> 0,
    async drain(bound, execute) {
      while (read !== bound) {
        const position = read % RING_BYTES;
        const command = words[position / 4] >>> 0, opcode = words[position / 4 + 1], resource = words[position / 4 + 2] >>> 0, length = words[position / 4 + 3];
        if (length < 0 || length > MAX_BYTES || position + 16 + length > RING_BYTES) { throw new Error('invalid posted GPU command'); }
        if (opcode) {
          const result = await execute({ command, opcode, resource, bytes: data.subarray(position + 16, position + 16 + length) });
          if (result && result.status !== Status.Ok && !Atomics.load(ring, 2)) {
            const message = (result.bytes ?? new Uint8Array()).subarray(0, RING_HEADER_BYTES - 64);
            new Uint8Array(buffer, RING_OFFSET + 64, message.byteLength).set(message);
            ring[4] = message.byteLength; ring[3] = result.status;
            Atomics.store(ring, 2, 1);
          }
        }
        read = (read + 16 + ((length + 15) & ~15)) >>> 0;
        Atomics.store(ring, 1, read | 0);
        Atomics.notify(ring, 1);
      }
    },
  };
};
const views = mailbox => {
  const buffer = mailbox.buffer ?? mailbox;
  const offset = mailbox.byteOffset ?? 0;
  if (!(buffer instanceof SharedArrayBuffer) || buffer.byteLength < offset + HEADER_BYTES + MAX_BYTES) {
    throw new Error('invalid bounded GPU mailbox');
  }
  return { header: new Int32Array(buffer, offset, 16), bytes: new Uint8Array(buffer, offset + HEADER_BYTES, MAX_BYTES) };
};
const delay = () => new Promise(resolve => setTimeout(resolve, 1));
export const waitForMailboxRequest = async mailbox => {
  const { header } = views(mailbox);
  for (;;) {
    const state = Atomics.load(header, 0);
    if (state === 1) { return true; }
    if (state === 4) { return false; }
    if (typeof Atomics.waitAsync === 'function') {
      await Atomics.waitAsync(header, 0, state).value;
    } else {
      await delay();
    }
  }
};
export const closeMailbox = (mailbox, status = Status.Closed) => {
  const { header } = views(mailbox);
  Atomics.store(header, 7, status);
  Atomics.store(header, 6, status);
  Atomics.store(header, 5, 0);
  Atomics.store(header, 0, 4);
  Atomics.notify(header, 0);
  ringDoorbell(mailbox);
};
export class MailboxClient {
  constructor(mailbox, { timeoutMs = 30000 } = {}) {
    this.mailbox = mailbox;
    this.sequence = 0;
    this.timeoutMs = timeoutMs;
  }
  async execute(opcode, resource = 0, input = new Uint8Array()) {
    const { header, bytes } = views(this.mailbox);
    if (input.byteLength > MAX_BYTES) {
      throw new Error('GPU packet exceeds bounded capacity');
    }
    const state = Atomics.load(header, 0);
    if (state === 4) {
      throw new Error('GPU service closed');
    }
    if (state !== 0 || Atomics.compareExchange(header, 0, 0, 5) !== 0) {
      throw new Error('GPU mailbox busy');
    }
    this.sequence = Math.max(this.sequence, header[1] >>> 0);
    if (this.sequence >= 0xffffffff) {
      closeMailbox(this.mailbox);
      throw new Error('GPU command sequence exhausted');
    }
    bytes.set(input);
    const command = ++this.sequence;
    header[1] = command;
    header[2] = opcode;
    header[3] = resource;
    header[4] = input.byteLength;
    header[5] = 0;
    Atomics.compareExchange(header, 0, 5, 1);
    Atomics.notify(header, 0);
    ringDoorbell(this.mailbox);
    const deadline = Date.now() + this.timeoutMs;
    while (Atomics.load(header, 0) < 3) {
      if (Date.now() >= deadline) {
        closeMailbox(this.mailbox);
        break;
      }
      const waiting = Atomics.load(header, 0);
      if (waiting >= 3) { break; }
      if (typeof Atomics.waitAsync === 'function') {
        await Atomics.waitAsync(header, 0, waiting, Math.max(1, deadline - Date.now())).value;
      } else {
        await delay();
      }
    }
    if (header[5] < 0 || header[5] > MAX_BYTES || header[6] < 0 || header[6] > Status.Lost || (header[1] >>> 0) !== command || (header[3] >>> 0) !== resource) {
      closeMailbox(this.mailbox, Status.Error);
    }
    const terminal = Atomics.load(header, 0) === 4;
    const result = { command, resource, status: terminal ? header[7] : header[6], bytes: terminal ? new Uint8Array() : bytes.slice(0, header[5]) };
    Atomics.compareExchange(header, 0, 3, 0);
    Atomics.notify(header, 0);
    return result;
  }
  close() {
    closeMailbox(this.mailbox);
  }
}
export const serveMailbox = async (mailbox, execute) => {
  const { header, bytes } = views(mailbox);
  if (Atomics.compareExchange(header, 0, 1, 2) !== 1) {
    return false;
  }
  const command = header[1] >>> 0;
  const resource = header[3] >>> 0;
  const length = header[4];
  let result;
  try {
    if (length < 0 || length > MAX_BYTES) {
      throw new Error('GPU request exceeds bounded capacity');
    }
    const packet = { command, opcode: header[2], resource, bytes: bytes.slice(0, length) };
    result = await execute(packet);
    if (!result || !Number.isInteger(result.status) || result.status < 0 || result.status > Status.Lost) {
      throw new Error('invalid GPU reply status');
    }
    if ((result.bytes?.byteLength ?? 0) > MAX_BYTES) {
      throw new Error('GPU reply exceeds bounded capacity');
    }
  } catch (error) {
    result = { status: Status.Error, bytes: new TextEncoder().encode(String(error)).slice(0, MAX_BYTES) };
  }
  if (Atomics.load(header, 0) !== 2 || (header[1] >>> 0) !== command || (header[3] >>> 0) !== resource) {
    return true;
  }
  bytes.set(result.bytes ?? new Uint8Array());
  header[5] = result.bytes?.byteLength ?? 0;
  header[6] = result.status;
  Atomics.compareExchange(header, 0, 2, 3);
  Atomics.notify(header, 0);
  return true;
};
