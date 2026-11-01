import { IsoReadError } from './iso-reader.mjs';
import { readIsoRange, isoMailboxHeader } from './iso-mailbox.mjs';

export const createWasmIsoReader = ({ port, mailbox, generation, ownership, getHeap, timeoutMs = 5000 }) => {
  isoMailboxHeader(mailbox);
  if (!(ownership instanceof Int32Array) || !(ownership.buffer instanceof SharedArrayBuffer) ||
      ownership.length !== 1 || ownership.buffer.growable || typeof getHeap !== 'function' ||
      !Number.isSafeInteger(generation) || generation < 1 || generation > 0x7fffffff) {
    throw new IsoReadError('REQUEST', 'Invalid Wasm ISO runtime ownership.');
  }
  let requestId = 0;
  const requireOwner = () => {
    if (Atomics.load(ownership, 0) !== generation) {
      throw new IsoReadError('STALE', 'Wasm ISO read belongs to an inactive runtime.');
    }
  };
  const heapAt = (destination, length) => {
    const heap = getHeap();
    if (!(heap instanceof Uint8Array) || !Number.isSafeInteger(destination) || destination < 0 ||
        !Number.isSafeInteger(length) || length < 0 || destination > heap.byteLength ||
        length > heap.byteLength - destination) {
      throw new IsoReadError('DESTINATION', 'ISO destination is outside Wasm memory.');
    }
    return heap;
  };
  return (offset, destination, length) => {
    requireOwner();
    heapAt(destination, length);
    const bytes = readIsoRange(port, mailbox, offset, length, ++requestId, generation, timeoutMs);
    requireOwner();
    if (bytes.length !== length) {
      throw new IsoReadError('SHORT_READ', 'ISO completion did not fill the requested range.');
    }
    heapAt(destination, length).set(bytes, destination);
    return length;
  };
};
