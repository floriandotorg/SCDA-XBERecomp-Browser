import { test } from 'node:test';
import assert from 'node:assert/strict';

const fixture = async () => {
  const { AudioQueue } = await import('../../src/browser/audio-transport.mjs');
  const buffer = new SharedArrayBuffer(32 + 16384 * 6 * 4);
  const header = new Uint32Array(buffer, 0, 8);
  const samples = new Float32Array(buffer, 32);
  header[3] = 1;
  const queue = new AudioQueue({ buffer, byteOffset: 0, samplesOffset: 32 });
  return { queue, buffer, header, samples };
};

test('native six channel audio folds centre and surrounds, omits LFE and clips after summing', async () => {
  const { queue, header, samples } = await fixture();
  samples.set([0.1, -0.2, 0.2, 99, 0.3, -0.4, 2, -2, 0, 0, 0, 0]);
  header[0] = 2;
  const left = new Float32Array(2), right = new Float32Array(2);
  queue.render(left, right);
  assert.ok(Math.abs(left[0] - (0.1 + 0.707 * 0.5)) < 0.000001);
  assert.ok(Math.abs(right[0] - (-0.2 - 0.707 * 0.2)) < 0.000001);
  assert.equal(left[1], 1);
  assert.equal(right[1], -1);
  assert.equal(header[1], 2);
});

test('wraparound consumes only committed frames and underrun fills silence', async () => {
  const { queue, header, samples } = await fixture();
  header[1] = 0xffffffff;
  header[0] = 1;
  samples[16383 * 6] = 0.25;
  samples[0] = 0.5;
  const left = new Float32Array(4), right = new Float32Array(4);
  queue.render(left, right);
  assert.deepEqual([...left], [0.25, 0.5, 0, 0]);
  assert.deepEqual([...right], [0, 0, 0, 0]);
  assert.equal(header[1], 1);
  assert.equal(header[5], 2);
  queue.render(left, right);
  assert.deepEqual([...left], [0, 0, 0, 0]);
  assert.equal(header[5], 6);
});

test('suspended output drains stale audio without replay and corrupt queues fail silent', async () => {
  const { queue, header, samples } = await fixture();
  samples.fill(0.5);
  header[0] = 10;
  header[3] = 0;
  const left = new Float32Array(2), right = new Float32Array(2);
  queue.render(left, right);
  assert.deepEqual([...left], [0, 0]);
  assert.equal(header[1], 10);
  header[3] = 1;
  queue.render(left, right);
  assert.deepEqual([...left], [0, 0]);
  header[0] = 16395;
  queue.render(left, right);
  assert.deepEqual([...left], [0, 0]);
  assert.equal(header[7], 1);
});

test('transport rejects nonshared, unaligned, overlapping and truncated storage', async () => {
  const { AudioQueue } = await import('../../src/browser/audio-transport.mjs');
  assert.throws(() => new AudioQueue({ buffer: new ArrayBuffer(500000), byteOffset: 0, samplesOffset: 32 }));
  for (const [byteOffset, samplesOffset] of [[1, 32], [0, 0], [0, 28], [0, 33], [-4, 32], [0, 500000]]) {
    assert.throws(() => new AudioQueue({ buffer: new SharedArrayBuffer(500000), byteOffset, samplesOffset }));
  }
});
