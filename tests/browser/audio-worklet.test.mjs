import { test } from 'node:test';
import assert from 'node:assert/strict';

test('a stopped mission processor renders silence and lets the node go', async () => {
  let registered;
  globalThis.sampleRate = 48000;
  globalThis.AudioWorkletProcessor = class { port = {}; };
  globalThis.registerProcessor = (name, processor) => { registered = processor; };
  await import('../../src/browser/audio-worklet.mjs');
  const buffer = new SharedArrayBuffer(32 + 16384 * 6 * 4);
  const header = new Uint32Array(buffer, 0, 8);
  new Float32Array(buffer, 32).fill(0.5);
  header[0] = 1000;
  header[3] = 1;
  const processor = new registered({ processorOptions: { buffer, byteOffset: 0, samplesOffset: 32 } });
  const output = [[new Float32Array(128), new Float32Array(128)]];
  assert.equal(processor.process([], output), true);
  assert.equal(header[1], 128);
  processor.port.onmessage({ data: 'stop' });
  output[0][0].fill(1);
  assert.equal(processor.process([], output), false);
  assert.deepEqual([...new Set(output[0][0])], [0]);
  assert.equal(header[1], 128);
});
