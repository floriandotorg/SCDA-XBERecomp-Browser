import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createBootRuntime } from '../../src/browser/boot-runtime.mjs';
import { ISO_MAILBOX, isoMailboxHeader } from '../../src/browser/iso-mailbox.mjs';
import { encodeHostSettings } from '../../src/browser/host-settings.mjs';

const fixture = (acknowledge = true, options = {}) => {
  const ioWorkers = [];
  const guestMessages = [];
  let guestReport;
  let guestError;
  let treeTerminated = false;
  class IoWorker extends EventTarget {
    constructor() { super(); this.messages = []; this.terminated = false; ioWorkers.push(this); }
    postMessage = (data) => {
      this.messages.push(data);
      if (acknowledge) {
        queueMicrotask(() => this.dispatchEvent(new MessageEvent('message', { data: { id: data.id, ok: true } })));
      }
    };
    terminate = () => { this.terminated = true; };
  }
  const runtime = createBootRuntime({
    ...options,
    WorkerClass: IoWorker,
    createBroker: () => ({
      create: (url, options, report, error) => {
        guestReport = report;
        guestError = error;
        return { postMessage: (message) => guestMessages.push(message) };
      },
      terminateAll: async () => { treeTerminated = true; },
    }),
  });
  const abort = new AbortController();
  const context = { source: new Blob(['ISO']), generation: 7, ownership: new Int32Array(new SharedArrayBuffer(4)), signal: abort.signal, reports: [] };
  Atomics.store(context.ownership, 0, context.generation);
  context.report = (data) => context.reports.push(data);
  return { runtime, context, ioWorkers, guestMessages, get guestReport() { return guestReport; }, get guestError() { return guestError; }, get treeTerminated() { return treeTerminated; } };
};

test('development start reaches the owned worker only when explicitly selected', async () => {
  for (const developmentStart of [false, true]) {
    const harness = fixture(true, { developmentStart });
    await harness.runtime.start(harness.context);
    assert.equal(harness.guestMessages[0].developmentStart, developmentStart);
    await harness.runtime.terminateAll();
  }
  assert.throws(() => createBootRuntime({ WorkerClass: EventTarget, developmentStart: 'true' }), /development start/);
});

test('boot time host settings travel in the worker start message beside development start', async () => {
  const hostSettings = encodeHostSettings({ skipIntros: true, pauseOnFocusLoss: true });
  const harness = fixture(true, { hostSettings });
  await harness.runtime.start(harness.context);
  assert.equal(harness.guestMessages[0].hostSettings, hostSettings);
  await harness.runtime.terminateAll();
  const plain = fixture(true);
  await plain.runtime.start(plain.context);
  assert.equal(Object.hasOwn(plain.guestMessages[0], 'hostSettings'), false);
  await plain.runtime.terminateAll();
  assert.throws(() => createBootRuntime({ WorkerClass: EventTarget, hostSettings: { words: [] } }), /host settings/i);
});

test('runtime wires selected source, ownership and completion bridge without player manifest authority', async () => {
  const harness = fixture();
  await harness.runtime.start(harness.context);
  assert.equal(harness.ioWorkers[0].messages[0].file, harness.context.source);
  assert.equal(harness.ioWorkers[0].messages[1].type, 'connect');
  const message = harness.guestMessages[0];
  assert.equal(message.generation, 7);
  assert.equal(message.ownership, harness.context.ownership);
  assert.equal(message.size, harness.context.source.size);
  assert.equal(message.manifest, undefined);
  harness.guestReport({ generation: 7, ok: false, code: 'ENTRY_RETURNED' });
  assert.equal(harness.context.reports[0].code, 'ENTRY_RETURNED');
  harness.runtime.cancelIo();
  await harness.runtime.terminateAll();
  message.port.close();
  harness.ioWorkers[0].messages[1].port.close();
  assert.equal(harness.treeTerminated, true);
  assert.equal(harness.ioWorkers[0].terminated, true);
});

test('cancelling a pending mount rejects startup and prevents creating a guest later', async () => {
  const harness = fixture(false);
  const start = harness.runtime.start(harness.context);
  harness.runtime.cancelIo();
  await assert.rejects(start, { code: 'CANCELLED' });
  await harness.runtime.terminateAll();
  assert.equal(harness.guestMessages.length, 0);
  assert.equal(harness.ioWorkers[0].terminated, true);
});

test('cancellation wakes pending I/O and late guest reports cannot escape retired runtime', async () => {
  const harness = fixture();
  await harness.runtime.start(harness.context);
  const message = harness.guestMessages[0];
  const header = isoMailboxHeader(message.mailbox);
  Atomics.store(header, 1, 4);
  Atomics.store(header, 2, 7);
  Atomics.store(header, 0, ISO_MAILBOX.pending);
  harness.runtime.cancelIo();
  assert.equal(Atomics.load(header, 0), ISO_MAILBOX.failed);
  harness.guestReport({ generation: 7, phase: 'running' });
  assert.equal(harness.context.reports.length, 0);
  await harness.runtime.terminateAll();
  message.port.close();
  harness.ioWorkers[0].messages[1].port.close();
});

test('controller samples shared execution independently and only independent I/O stats count as completion', async () => {
  const harness = fixture();
  await harness.runtime.start(harness.context);
  const words = new Uint32Array(new SharedArrayBuffer(16));
  harness.guestReport({ generation: 7, phase: 'progress', buffer: words.buffer, address: 4 });
  assert.equal(harness.context.reports.length, 0);
  Atomics.store(words, 1, 43);
  assert.deepEqual(harness.runtime.sampleProgress(), { execution: 43, ioBytes: 0 });
  for (let n = 0; n < 20; ++n) { harness.runtime.sampleProgress(); }
  const requests = harness.ioWorkers[0].messages.filter((message) => message.type === 'stats');
  assert.equal(requests.length, 1, 'only one independent stats request may be pending');
  harness.ioWorkers[0].dispatchEvent(new MessageEvent('message', {
    data: { id: requests[0].id, ok: true, stats: { generation: 7, bytesRead: 4096 } },
  }));
  await Promise.resolve();
  assert.deepEqual(harness.runtime.sampleProgress(), { execution: 43, ioBytes: 4096 });
  const mailbox = isoMailboxHeader(harness.guestMessages[0].mailbox);
  Atomics.store(mailbox, 1, 999);
  Atomics.store(mailbox, 0, ISO_MAILBOX.complete);
  harness.guestReport({ generation: 7, phase: 'booting', ioBytes: 99999 });
  assert.equal(harness.runtime.sampleProgress().ioBytes, 4096);
  Atomics.store(harness.context.ownership, 0, 0);
  Atomics.store(words, 1, 99);
  assert.equal(harness.runtime.sampleProgress(), undefined);
  harness.runtime.cancelIo();
  await harness.runtime.terminateAll();
});

test('stale and duplicate progress descriptors cannot replace the observed runtime word', async () => {
  const harness = fixture();
  await harness.runtime.start(harness.context);
  const original = new Uint32Array(new SharedArrayBuffer(16));
  const stale = new Uint32Array(new SharedArrayBuffer(16));
  Atomics.store(stale, 1, 999);
  harness.guestReport({ generation: 6, phase: 'progress', buffer: stale.buffer, address: 4 });
  assert.equal(harness.runtime.sampleProgress(), undefined);
  harness.guestReport({ generation: 7, phase: 'progress', buffer: original.buffer, address: 4 });
  harness.guestReport({ generation: 7, phase: 'progress', buffer: stale.buffer, address: 4 });
  Atomics.store(original, 1, 17);
  assert.equal(harness.runtime.sampleProgress().execution, 17);
  harness.runtime.cancelIo();
  await harness.runtime.terminateAll();
  Atomics.store(original, 1, 18);
  assert.equal(harness.runtime.sampleProgress(), undefined);
  harness.guestReport({ generation: 7, phase: 'progress', buffer: stale.buffer, address: 4 });
  assert.equal(harness.context.reports.length, 0);
});

test('malformed progress descriptors fail closed instead of observing arbitrary or unbounded memory', async () => {
  for (const descriptor of [
    { buffer: new ArrayBuffer(16), address: 4 },
    { buffer: new SharedArrayBuffer(16), address: 0 },
    { buffer: new SharedArrayBuffer(16), address: 3 },
    { buffer: new SharedArrayBuffer(16), address: 16 },
    { buffer: new SharedArrayBuffer(16), address: 2 ** 32 + 4 },
    { buffer: new SharedArrayBuffer(16), address: 4.5 },
    { buffer: new SharedArrayBuffer(16, { maxByteLength: 64 }), address: 4 },
  ]) {
    const harness = fixture();
    await harness.runtime.start(harness.context);
    harness.guestReport({ generation: 7, phase: 'progress', ...descriptor });
    assert.equal(harness.context.reports[0].code, 'PROGRESS_PROTOCOL');
    harness.runtime.cancelIo();
    await harness.runtime.terminateAll();
  }
});

test('a guest running callback is not a diagnostic startup completion contract', async () => {
  const harness = fixture();
  await harness.runtime.start(harness.context);
  harness.guestReport({ generation: 7, phase: 'running' });
  assert.deepEqual(harness.context.reports[0], {
    generation: 7, ok: false, code: 'BOOT_PROTOCOL', message: 'Guest callbacks cannot establish a running game.',
  });
  harness.runtime.cancelIo();
  await harness.runtime.terminateAll();
});

test('worker failure becomes an explicit generation tagged boot failure', async () => {
  const harness = fixture();
  await harness.runtime.start(harness.context);
  harness.guestError({ message: 'Guest crashed.' });
  assert.deepEqual(harness.context.reports[0], { generation: 7, ok: false, code: 'WORKER', message: 'Guest crashed.' });
  harness.runtime.cancelIo();
  await harness.runtime.terminateAll();
  harness.guestMessages[0].port.close();
  harness.ioWorkers[0].messages[1].port.close();
});
