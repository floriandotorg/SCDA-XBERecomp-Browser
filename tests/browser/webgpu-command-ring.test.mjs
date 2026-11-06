import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createMailbox, createCommandRing, ringDoorbell, RING_OFFSET, RING_HEADER_BYTES, RING_BYTES, Status } from '../../src/browser/webgpu-transport.mjs';

const post = (mailbox, command, opcode, resource, payload) => {
  const ring = new Int32Array(mailbox, RING_OFFSET, 8);
  const words = new Int32Array(mailbox, RING_OFFSET + RING_HEADER_BYTES, RING_BYTES / 4);
  const data = new Uint8Array(mailbox, RING_OFFSET + RING_HEADER_BYTES, RING_BYTES);
  const need = 16 + ((payload.byteLength + 15) & ~15);
  let write = ring[0] >>> 0;
  let position = write % RING_BYTES;
  if (RING_BYTES - position < need) {
    words.set([0, 0, 0, RING_BYTES - position - 16], position / 4);
    write = (write + RING_BYTES - position) >>> 0;
    position = 0;
  }
  words.set([command, opcode, resource, payload.byteLength], position / 4);
  data.set(payload, position + 16);
  Atomics.store(ring, 0, (write + need) | 0);
  ringDoorbell(mailbox);
};

test('posted commands drain in order with their exact payloads', async () => {
  const mailbox = createMailbox();
  const ring = createCommandRing(mailbox);
  const before = ring.doorbell();
  post(mailbox, 1, 14, 7, new Uint8Array([1, 2, 3]));
  post(mailbox, 2, 9, 8, new Uint8Array(0));
  assert.notEqual(ring.doorbell(), before);
  assert.ok(ring.pending());
  const seen = [];
  await ring.drain(ring.bound(), packet => { seen.push([packet.command, packet.opcode, packet.resource, [...packet.bytes]]); return { status: Status.Ok }; });
  assert.deepEqual(seen, [[1, 14, 7, [1, 2, 3]], [2, 9, 8, []]]);
  assert.ok(!ring.pending());
  assert.equal(new Int32Array(mailbox, RING_OFFSET, 8)[1], new Int32Array(mailbox, RING_OFFSET, 8)[0]);
});

test('entries that would cross the ring end restart at its beginning', async () => {
  const mailbox = createMailbox();
  const header = new Int32Array(mailbox, RING_OFFSET, 8);
  const start = RING_BYTES - 32;
  header[0] = start; header[1] = start;
  const ring = createCommandRing(mailbox);
  const payload = new Uint8Array(40).map((_, n) => n);
  post(mailbox, 3, 15, 1, payload);
  const seen = [];
  await ring.drain(ring.bound(), packet => { seen.push([packet.opcode, [...packet.bytes]]); return { status: Status.Ok }; });
  assert.deepEqual(seen, [[15, [...payload]]]);
  assert.equal(header[1] >>> 0, RING_BYTES + 16 + 48);
});

test('a failed posted command is retained for the producer', async () => {
  const mailbox = createMailbox();
  const ring = createCommandRing(mailbox);
  post(mailbox, 4, 14, 2, new Uint8Array(4));
  post(mailbox, 5, 14, 3, new Uint8Array(4));
  const message = new TextEncoder().encode('fixture failure');
  await ring.drain(ring.bound(), packet => packet.command === 4 ? { status: Status.Error, bytes: message } : { status: Status.Ok });
  const header = new Int32Array(mailbox, RING_OFFSET, 8);
  assert.equal(header[2], 1);
  assert.equal(header[3], Status.Error);
  assert.equal(new TextDecoder().decode(new Uint8Array(mailbox, RING_OFFSET + 64, header[4])), 'fixture failure');
});
