import { test } from 'node:test';
import assert from 'node:assert/strict';
import { Worker, MessageChannel } from 'node:worker_threads';
import { createIsoMailbox, ISO_MAILBOX, finishIsoRead, readIsoRange } from '../../src/browser/iso-mailbox.mjs';
import { installIsoIoService } from '../../src/browser/iso-io-service.mjs';

const waitMessage = (port) => new Promise((resolve, reject) => {
  const timer = setTimeout(() => reject(new Error('Worker response timed out')), 5000);
  port.once('message', (message) => { clearTimeout(timer); resolve(message); });
});

const ioWorker = async (context) => {
  const worker = new Worker(new URL('./iso-io-node.mjs', import.meta.url));
  context.after(() => worker.terminate());
  const request = async (message, transfer = []) => {
    const response = waitMessage(worker);
    worker.postMessage(message, transfer);
    const reply = await response;
    assert.equal(reply.ok, true, JSON.stringify(reply));
    return reply;
  };
  await request({ type: 'mount', file: new File([Uint8Array.from({ length: 256 }, (_, n) => n)], 'synthetic.iso'), generation: 1 });
  return { worker, request };
};

test('shared completion refuses stale tags and never writes cancelled bytes', () => {
  const mailbox = createIsoMailbox(32);
  const header = new Int32Array(mailbox, 0, ISO_MAILBOX.headerWords);
  header.set([ISO_MAILBOX.pending, 7, 2, 0, 0]);
  assert.equal(finishIsoRead(mailbox, 7, 1, new Uint8Array([8])), false);
  assert.equal(header[0], ISO_MAILBOX.pending);
  assert.equal(new Uint8Array(mailbox, ISO_MAILBOX.headerBytes)[0], 0);
  Atomics.store(header, 0, ISO_MAILBOX.cancelled);
  assert.equal(finishIsoRead(mailbox, 7, 2, new Uint8Array([8])), false);
  assert.equal(new Uint8Array(mailbox, ISO_MAILBOX.headerBytes)[0], 0);
});

test('synchronous bridge times out, invalidates the slot and rejects unsafe reuse', () => {
  const mailbox = createIsoMailbox(16);
  const messages = [];
  const port = { postMessage: (message) => messages.push(message) };
  assert.throws(() => readIsoRange(port, mailbox, 0, 4, 1, 1, 5), (error) => error.code === 'TIMEOUT');
  assert.deepEqual(messages.map((message) => message.type), ['read', 'cancel']);
  assert.equal(finishIsoRead(mailbox, 1, 1, new Uint8Array([1, 2, 3, 4])), false);
  const header = new Int32Array(mailbox, 0, ISO_MAILBOX.headerWords);
  Atomics.store(header, 0, ISO_MAILBOX.writing);
  assert.throws(() => readIsoRange(port, mailbox, 0, 4, 2, 1, 5), (error) => error.code === 'BUSY');
});

test('mailbox request tags cannot be reused after a timeout or successful read', () => {
  const mailbox = createIsoMailbox(16);
  const blockedPort = { postMessage: () => {} };
  assert.throws(() => readIsoRange(blockedPort, mailbox, 0, 4, 1, 1, 1), (error) => error.code === 'TIMEOUT');
  assert.throws(() => readIsoRange(blockedPort, mailbox, 0, 4, 1, 1, 1), (error) => error.code === 'REQUEST');
  const completingPort = { postMessage: (message) => {
    assert.equal(finishIsoRead(mailbox, 1, 1, new Uint8Array([99])), false);
    finishIsoRead(mailbox, message.requestId, message.generation, new Uint8Array([7, 8]));
  } };
  assert.deepEqual([...readIsoRange(completingPort, mailbox, 0, 2, 2, 1)], [7, 8]);
  assert.throws(() => readIsoRange(blockedPort, mailbox, 0, 4, 2, 1, 1), (error) => error.code === 'REQUEST');
});

test('dedicated I/O completes while a separate guest worker blocks in Atomics.wait', async (context) => {
  const { request } = await ioWorker(context);
  const guest = new Worker(new URL('./iso-guest-node.mjs', import.meta.url));
  context.after(() => guest.terminate());
  const channel = new MessageChannel();
  const mailbox = createIsoMailbox(64);
  await request({ type: 'connect', port: channel.port1, mailbox, generation: 1 }, [channel.port1]);
  let heartbeats = 0;
  const timer = setInterval(() => { ++heartbeats; }, 1);
  context.after(() => clearInterval(timer));
  const response = waitMessage(guest);
  guest.postMessage({ port: channel.port2, mailbox, generation: 1 }, [channel.port2]);
  const result = await response;
  assert.equal(result.ok, true, JSON.stringify(result));
  assert.deepEqual(result.bytes, [13, 14, 15, 16, 17, 18, 19]);
  assert.equal(result.rangeError, 'RANGE');
  assert.ok(heartbeats > 0);
  const stats = (await request({ type: 'stats' })).stats;
  assert.equal(stats.bytesRead, 7);
  assert.equal(stats.cacheBytes, 0);
});

test('generation replacement wakes old guest slots and retains the mounted File', async (context) => {
  const { request } = await ioWorker(context);
  const oldChannel = new MessageChannel();
  context.after(() => oldChannel.port2.close());
  const oldMailbox = createIsoMailbox(32);
  await request({ type: 'connect', port: oldChannel.port1, mailbox: oldMailbox, generation: 1 }, [oldChannel.port1]);
  const header = new Int32Array(oldMailbox, 0, ISO_MAILBOX.headerWords);
  header.set([ISO_MAILBOX.pending, 1, 1, 0, 0]);
  await request({ type: 'generation', generation: 2 });
  assert.equal(Atomics.load(header, 0), ISO_MAILBOX.failed);
  const guest = new Worker(new URL('./iso-guest-node.mjs', import.meta.url));
  context.after(() => guest.terminate());
  const nextChannel = new MessageChannel();
  const mailbox = createIsoMailbox(64);
  await request({ type: 'connect', port: nextChannel.port1, mailbox, generation: 2 }, [nextChannel.port1]);
  const response = waitMessage(guest);
  guest.postMessage({ port: nextChannel.port2, mailbox, generation: 2 }, [nextChannel.port2]);
  assert.equal((await response).ok, true);
});

for (const outcome of ['READ_FAILED', 'SHORT_READ', 'CANCELLED']) {
  test(`I/O ${outcome} wakes a blocked guest without touching a replacement slot`, async (context) => {
    let complete;
    let fail;
    let started;
    const readStarted = new Promise((resolve) => { started = resolve; });
    const file = { size: 256, slice: () => ({ arrayBuffer: () => {
      started();
      return new Promise((resolve, reject) => { complete = resolve; fail = reject; });
    } }) };
    const control = new EventTarget();
    let reply;
    control.postMessage = (message) => { reply = message; };
    installIsoIoService(control);
    const request = (message) => {
      control.dispatchEvent(new MessageEvent('message', { data: message }));
      assert.equal(reply.ok, true);
      return reply;
    };
    context.after(() => request({ type: 'close' }));
    request({ type: 'mount', file, generation: 1 });
    const guest = new Worker(new URL('./iso-guest-node.mjs', import.meta.url));
    context.after(() => guest.terminate());
    const channel = new MessageChannel();
    const mailbox = createIsoMailbox(64);
    request({ type: 'connect', port: channel.port1, mailbox, generation: 1 });
    const response = waitMessage(guest);
    guest.postMessage({ port: channel.port2, mailbox, generation: 1 }, [channel.port2]);
    await readStarted;
    if (outcome === 'READ_FAILED') {
      fail(new Error('private file details'));
    } else if (outcome === 'SHORT_READ') {
      complete(new ArrayBuffer(1));
    } else {
      request({ type: 'generation', generation: 2 });
      assert.equal(request({ type: 'stats' }).stats.stagingBytes, 7);
    }
    const result = await response;
    assert.equal(result.ok, false);
    assert.equal(result.code, outcome);
    assert.ok(!result.message.includes('private'));
    if (outcome === 'CANCELLED') {
      const header = new Int32Array(mailbox, 0, ISO_MAILBOX.headerWords);
      header.set([ISO_MAILBOX.pending, 2, 2, 0, 0]);
      complete(new Uint8Array(7).fill(99).buffer);
      await new Promise((resolve) => setImmediate(resolve));
      assert.equal(header[0], ISO_MAILBOX.pending);
      assert.equal(new Uint8Array(mailbox, ISO_MAILBOX.headerBytes)[0], 0);
    }
    assert.equal(request({ type: 'stats' }).stats.stagingBytes, 0);
  });
}

test('I/O service limits attached guest mailboxes and close wakes pending reads', async (context) => {
  const { worker, request } = await ioWorker(context);
  const mailboxes = [];
  for (let n = 0; n < 2; ++n) {
    const channel = new MessageChannel();
    context.after(() => channel.port2.close());
    const mailbox = createIsoMailbox(32);
    mailboxes.push(mailbox);
    await request({ type: 'connect', port: channel.port1, mailbox, generation: 1 }, [channel.port1]);
  }
  const excess = new MessageChannel();
  context.after(() => excess.port2.close());
  const rejected = waitMessage(worker);
  worker.postMessage({ type: 'connect', port: excess.port1, mailbox: createIsoMailbox(32), generation: 1 }, [excess.port1]);
  assert.equal((await rejected).error.code, 'BUSY');
  const header = new Int32Array(mailboxes[0], 0, ISO_MAILBOX.headerWords);
  header.set([ISO_MAILBOX.pending, 1, 1, 0, 0]);
  await request({ type: 'close' });
  assert.equal(header[0], ISO_MAILBOX.failed);
});
