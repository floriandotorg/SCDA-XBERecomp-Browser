import { ISO_LIMITS, IsoReadError } from './iso-reader.mjs';

export const ISO_MAILBOX = Object.freeze({
  headerWords: 5, headerBytes: 20, idle: 0, pending: 1, complete: 2, failed: 3, cancelled: 4, writing: 5,
});
const errorCodes = ['READ_FAILED', 'RANGE', 'CANCELLED', 'STALE', 'BUSY', 'SHORT_READ', 'CLOSED', 'REQUEST'];

export const isoMailboxHeader = (mailbox) => {
  if (!(mailbox instanceof SharedArrayBuffer) || mailbox.byteLength < ISO_MAILBOX.headerBytes ||
      mailbox.byteLength > ISO_MAILBOX.headerBytes + ISO_LIMITS.maxReadBytes || mailbox.growable) {
    throw new IsoReadError('MAILBOX', 'Invalid fixed size ISO mailbox.');
  }
  return new Int32Array(mailbox, 0, ISO_MAILBOX.headerWords);
};

export const createIsoMailbox = (capacity = ISO_LIMITS.maxReadBytes) => {
  if (!Number.isSafeInteger(capacity) || capacity < 1 || capacity > ISO_LIMITS.maxReadBytes) {
    throw new IsoReadError('LIMITS', 'Invalid ISO mailbox capacity.');
  }
  return new SharedArrayBuffer(ISO_MAILBOX.headerBytes + capacity);
};

export const finishIsoRead = (mailbox, requestId, generation, bytes, errorCode) => {
  const header = isoMailboxHeader(mailbox);
  if (Atomics.load(header, 1) !== requestId || Atomics.load(header, 2) !== generation ||
      Atomics.compareExchange(header, 0, ISO_MAILBOX.pending, ISO_MAILBOX.writing) !== ISO_MAILBOX.pending) {
    return false;
  }
  if (bytes && bytes.length <= mailbox.byteLength - ISO_MAILBOX.headerBytes) {
    new Uint8Array(mailbox, ISO_MAILBOX.headerBytes, bytes.length).set(bytes);
    Atomics.store(header, 3, bytes.length);
    Atomics.store(header, 0, ISO_MAILBOX.complete);
  } else {
    const errorNumber = errorCodes.indexOf(errorCode);
    Atomics.store(header, 4, errorNumber < 0 ? 0 : errorNumber);
    Atomics.store(header, 0, ISO_MAILBOX.failed);
  }
  Atomics.notify(header, 0);
  return true;
};

export const readIsoRange = (port, mailbox, offset, length, requestId, generation, timeoutMs = 5000) => {
  const header = isoMailboxHeader(mailbox);
  if (!Number.isSafeInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 30000 ||
      !Number.isSafeInteger(requestId) || requestId < 1 || requestId > 0x7fffffff ||
      !Number.isSafeInteger(generation) || generation < 1 || generation > 0x7fffffff) {
    throw new IsoReadError('REQUEST', 'Invalid ISO wait budget or request tags.');
  }
  if (!Number.isSafeInteger(offset) || offset < 0 || !Number.isSafeInteger(length) || length < 0 ||
      length > mailbox.byteLength - ISO_MAILBOX.headerBytes) {
    throw new IsoReadError('RANGE', 'Invalid ISO mailbox range.');
  }
  const previous = Atomics.load(header, 0);
  if (previous === ISO_MAILBOX.pending || previous === ISO_MAILBOX.writing) {
    throw new IsoReadError('BUSY', 'ISO mailbox still has an outstanding writer.');
  }
  const previousGeneration = Atomics.load(header, 2);
  if (generation < previousGeneration || (generation === previousGeneration && requestId <= Atomics.load(header, 1))) {
    throw new IsoReadError('REQUEST', 'ISO mailbox request tags must increase.');
  }
  Atomics.store(header, 1, requestId);
  Atomics.store(header, 2, generation);
  Atomics.store(header, 3, 0);
  Atomics.store(header, 4, 0);
  Atomics.store(header, 0, ISO_MAILBOX.pending);
  try {
    port.postMessage({ type: 'read', offset, length, requestId, generation });
  } catch {
    Atomics.store(header, 0, ISO_MAILBOX.cancelled);
    throw new IsoReadError('CLOSED', 'ISO completion port is unavailable.');
  }
  const deadline = performance.now() + timeoutMs;
  for (;;) {
    const state = Atomics.load(header, 0);
    if (state === ISO_MAILBOX.complete) {
      return new Uint8Array(mailbox, ISO_MAILBOX.headerBytes, Atomics.load(header, 3));
    }
    if (state === ISO_MAILBOX.failed) {
      throw new IsoReadError(errorCodes[Atomics.load(header, 4)] ?? 'READ_FAILED', 'ISO range request failed.');
    }
    const remaining = deadline - performance.now();
    if (remaining <= 0 || state === ISO_MAILBOX.cancelled) {
      Atomics.compareExchange(header, 0, ISO_MAILBOX.pending, ISO_MAILBOX.cancelled);
      port.postMessage({ type: 'cancel', requestId, generation });
      throw new IsoReadError('TIMEOUT', 'ISO range request exceeded its wait budget.');
    }
    Atomics.wait(header, 0, state, remaining);
  }
};
