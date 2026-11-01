import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createBootRuntime } from '../../src/browser/boot-runtime.mjs';

const fixture = (createServices) => {
  const reports = [];
  const messages = [];
  const order = [];
  let publish;
  class Io extends EventTarget {
    postMessage = (data) => queueMicrotask(() => this.dispatchEvent(new MessageEvent('message', { data: { id: data.id, ok: true } })));
    terminate = () => order.push('io');
  }
  const runtime = createBootRuntime({
    WorkerClass: Io,
    createServices,
    createBroker: () => ({
      create: (url, options, report) => {
        publish = report;
        return { postMessage: (message) => messages.push(message) };
      },
      terminateAll: async () => order.push('guest'),
    }),
  });
  const abort = new AbortController();
  const ownership = new Int32Array(new SharedArrayBuffer(4));
  Atomics.store(ownership, 0, 1);
  const context = { source: new Blob(['ISO']), generation: 1, ownership, signal: abort.signal, report: (data) => reports.push(data) };
  return { runtime, context, reports, messages, order, get publish() { return publish; } };
};

test('trusted services attach before execution, consume descriptors and retire after guest workers', { timeout: 1000 }, async (t) => {
  const mailbox = new SharedArrayBuffer(64);
  const input = [];
  let serviceContext;
  const harness = fixture(async (context) => {
    serviceContext = context;
    return {
      webgpuMailbox: mailbox,
      cancel: () => harness.order.push('cancel'),
      dispose: async () => harness.order.push('dispose'),
      handle: (data) => {
        if (data.phase !== 'input') { return false; }
        input.push(data);
        return true;
      },
    };
  });
  t.after(() => harness.runtime.terminateAll());
  await harness.runtime.start(harness.context);
  assert.equal(harness.messages[0].webgpuMailbox, mailbox);
  assert.equal(serviceContext.generation, 1);
  assert.equal(serviceContext.owned(), true);
  harness.publish({ generation: 1, phase: 'input', address: 16 });
  assert.equal(input.length, 1);
  assert.equal(harness.reports.length, 0);
  serviceContext.report({ generation: 1, phase: 'diagnostic', command: 8 });
  assert.equal(harness.reports.length, 1);
  await harness.runtime.terminateAll();
  assert.deepEqual(harness.order, ['cancel', 'io', 'guest', 'dispose']);
  assert.equal(serviceContext.owned(), false);
  serviceContext.report({ generation: 1, phase: 'diagnostic', command: 9 });
  harness.publish({ generation: 1, phase: 'input', address: 20 });
  assert.equal(input.length, 1);
  assert.equal(harness.reports.length, 1);
});

test('retirement during asynchronous service initialization cancels the late service without creating guest work', { timeout: 1000 }, async (t) => {
  let resolve;
  let entered;
  const ready = new Promise((done) => { entered = done; });
  const harness = fixture(async () => {
    entered();
    return new Promise((done) => { resolve = done; });
  });
  t.after(() => harness.runtime.terminateAll());
  const starting = harness.runtime.start(harness.context);
  await ready;
  harness.runtime.cancelIo();
  resolve({ cancel: () => harness.order.push('late-cancel'), dispose: async () => harness.order.push('late-dispose') });
  await assert.rejects(starting, { code: 'CANCELLED' });
  await harness.runtime.terminateAll();
  assert.equal(harness.messages.length, 0);
  assert.equal(harness.order.filter((item) => item === 'late-cancel').length, 1);
  assert.equal(harness.order.filter((item) => item === 'late-dispose').length, 1);
});

test('initialization failure is not misreported as a teardown failure after the initializer has cleaned up', async () => {
  const harness = fixture(async () => { throw new Error('GPU unavailable'); });
  await assert.rejects(harness.runtime.start(harness.context), /GPU unavailable/);
  await harness.runtime.terminateAll();
  assert.deepEqual(harness.order, ['io', 'guest']);
  assert.equal(harness.messages.length, 0);
});

test('service cancellation failure does not prevent retiring I/O and guest workers', { timeout: 1000 }, async () => {
  const harness = fixture(async () => ({
    cancel: () => { throw new Error('cancel failure'); },
    dispose: async () => harness.order.push('dispose'),
  }));
  await harness.runtime.start(harness.context);
  await assert.rejects(harness.runtime.terminateAll(), /cancel failure/);
  assert.deepEqual(harness.order, ['io', 'guest', 'dispose']);
});
