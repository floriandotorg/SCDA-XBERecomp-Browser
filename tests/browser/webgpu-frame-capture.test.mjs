import test from 'node:test';
import assert from 'node:assert/strict';
import { createGPUCommandQueue } from '../../src/browser/webgpu-service.mjs';
import { MAX_BYTES, Opcode, Status } from '../../src/browser/webgpu-transport.mjs';

const fixture = () => {
  const calls = [];
  let finish;
  const bytes = new Uint8Array(16).fill(73);
  const service = {
    execute: async packet => {
      calls.push(packet.opcode);
      if (packet.opcode === Opcode.Draw) { await new Promise(resolve => { finish = resolve; }); }
      return { status: Status.Ok, bytes: packet.opcode === Opcode.Readback ? bytes : new Uint8Array() };
    },
    shutdown: () => { finish?.(); },
  };
  const queue = createGPUCommandQueue(service, () => ({ width: 2, height: 2 }));
  return { queue, calls, bytes, finish: () => finish() };
};
const packet = opcode => ({ opcode, resource: 1, command: 7, bytes: new Uint8Array() });

test('frame capture requires a completed presentation and bounded dimensions', async () => {
  const source = fixture();
  await assert.rejects(source.queue.capture(), /presentation/i);
  assert.deepEqual(source.calls, []);
  const queue = createGPUCommandQueue({ execute: async () => ({ status: Status.Ok, bytes: new Uint8Array() }), shutdown: () => {} }, () => ({ width: MAX_BYTES, height: 2 }));
  await queue.execute(packet(Opcode.Present));
  await assert.rejects(queue.capture(), /dimensions|budget/i);
});

test('capture serializes between guest commands and owns its completed pixels', async () => {
  const source = fixture();
  await source.queue.execute(packet(Opcode.Present));
  const draw = source.queue.execute(packet(Opcode.Draw));
  await Promise.resolve();
  const capture = source.queue.capture();
  await assert.rejects(source.queue.capture(), /pending/i);
  const flush = source.queue.execute(packet(Opcode.Flush));
  assert.deepEqual(source.calls, [Opcode.Present, Opcode.Draw]);
  source.finish();
  await draw;
  const frame = await capture;
  await flush;
  assert.deepEqual(source.calls, [Opcode.Present, Opcode.Draw, Opcode.Readback, Opcode.Flush]);
  assert.equal(frame.width, 2);
  assert.equal(frame.height, 2);
  assert.deepEqual(frame.pixels, new Uint8Array(16).fill(73));
  source.bytes.fill(0);
  assert.equal(frame.pixels[0], 73);
});

test('retirement rejects queued capture and further work', async () => {
  const source = fixture();
  await source.queue.execute(packet(Opcode.Present));
  const draw = source.queue.execute(packet(Opcode.Draw));
  await Promise.resolve();
  const capture = source.queue.capture();
  const rejected = assert.rejects(capture, /closed|retired/i);
  source.queue.shutdown();
  await draw;
  await rejected;
  assert.deepEqual(source.calls, [Opcode.Present, Opcode.Draw]);
  await assert.rejects(source.queue.capture(), /closed|retired/i);
  await assert.rejects(source.queue.execute(packet(Opcode.Flush)), /closed|retired/i);
});

test('capture rejects failed or malformed readback without publishing pixels', async () => {
  for (const result of [{ status: Status.Error, bytes: new Uint8Array() }, { status: Status.Ok, bytes: new Uint8Array(15) }]) {
    const queue = createGPUCommandQueue({ execute: async packet => packet.opcode === Opcode.Readback ? result : { status: Status.Ok }, shutdown: () => {} }, () => ({ width: 2, height: 2 }));
    await queue.execute(packet(Opcode.Present));
    await assert.rejects(queue.capture(), /readback|capture/i);
  }
});

test('reconfiguration requires a new completed presentation', async () => {
  const source = fixture();
  await source.queue.execute(packet(Opcode.Present));
  await source.queue.execute(packet(Opcode.Configure));
  await assert.rejects(source.queue.capture(), /presentation/i);
  assert.deepEqual(source.calls, [Opcode.Present, Opcode.Configure]);
});

test('retirement during readback cannot publish a stale captured frame', async () => {
  let finish;
  const queue = createGPUCommandQueue({
    execute: async packet => {
      if (packet.opcode === Opcode.Readback) { return await new Promise(resolve => { finish = resolve; }); }
      return { status: Status.Ok };
    },
    shutdown: () => finish({ status: Status.Ok, bytes: new Uint8Array(16) }),
  }, () => ({ width: 2, height: 2 }));
  await queue.execute(packet(Opcode.Present));
  const capture = queue.capture();
  await Promise.resolve();
  const rejected = assert.rejects(capture, /retired/i);
  queue.shutdown();
  await rejected;
});
