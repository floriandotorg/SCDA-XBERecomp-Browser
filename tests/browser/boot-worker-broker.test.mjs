import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createWorkerBroker, messageTransfers } from '../../src/browser/boot-worker-broker.mjs';

test('transfer discovery retains shared Wasm backing and deduplicates transferred buffers and ports', () => {
  const bytes = new Uint8Array(8);
  const shared = new Uint8Array(new SharedArrayBuffer(8));
  const channel = new MessageChannel();
  const data = { bytes, alias: bytes.buffer, shared, port: channel.port1, nested: new Map([[bytes.buffer, new Set([bytes])]]) };
  data.cycle = data;
  assert.deepEqual(messageTransfers(data, [channel.port1]), [channel.port1, bytes.buffer]);
  channel.port1.close();
  channel.port2.close();
});

const tick = () => new Promise((resolve) => setTimeout(resolve, 10));
const fixture = () => {
  const workers = [];
  class FakeWorker extends EventTarget {
    constructor(url, options) {
      super();
      this.url = url;
      this.options = options;
      this.messages = [];
      this.terminated = false;
      workers.push(this);
    }
    postMessage = (data, transfer) => { this.messages.push({ data, transfer }); };
    terminate = () => { this.terminated = true; };
  }
  const broker = createWorkerBroker({ WorkerClass: FakeWorker, hostUrl: new URL('https://local.test/boot-worker-host.mjs') });
  return { broker, workers };
};

test('parent owns root, pthreads and descendants, and forwards transfer lists', async () => {
  const { broker, workers } = fixture();
  const root = broker.create(new URL('https://local.test/boot-worker.mjs'));
  const rootControl = workers[0].messages[0].data.control;
  const childChannel = new MessageChannel();
  rootControl.postMessage({ type: 'create', entry: 'https://local.test/story_boot.mjs', options: { type: 'module', name: 'em-pthread' }, port: childChannel.port1 }, [childChannel.port1]);
  await tick();
  assert.equal(workers.length, 2);
  assert.equal(workers[1].options.name, 'em-pthread');
  const bytes = new ArrayBuffer(4);
  childChannel.port2.postMessage({ type: 'post', data: { bytes }, transfer: [bytes] }, [bytes]);
  await tick();
  assert.equal(bytes.byteLength, 0);
  assert.equal(workers[1].messages[1].data.bytes.byteLength, 4);
  assert.equal(workers[1].messages[1].transfer[0], workers[1].messages[1].data.bytes);
  const grandchildChannel = new MessageChannel();
  workers[1].messages[0].data.control.postMessage({ type: 'create', entry: 'https://local.test/story_boot.mjs', options: { name: 'em-pthread' }, port: grandchildChannel.port1 }, [grandchildChannel.port1]);
  await tick();
  assert.equal(workers.length, 3);
  await broker.terminateAll();
  assert.ok(workers.every((worker) => worker.terminated));
  assert.throws(() => root.postMessage({}), /retired/);
  assert.throws(() => broker.create(new URL('https://local.test/boot-worker.mjs')), /retired/);
  childChannel.port2.close();
  grandchildChannel.port2.close();
  for (const worker of workers) { worker.messages[0].data.control.close(); }
});

test('queued descendant creation is discarded after retirement', async () => {
  const { broker, workers } = fixture();
  broker.create(new URL('https://local.test/boot-worker.mjs'));
  const channel = new MessageChannel();
  workers[0].messages[0].data.control.postMessage({ type: 'create', entry: 'https://local.test/story_boot.mjs', options: {}, port: channel.port1 }, [channel.port1]);
  await broker.terminateAll();
  await tick();
  assert.equal(workers.length, 1);
  assert.equal(workers[0].terminated, true);
  workers[0].messages[0].data.control.close();
  channel.port2.close();
});

test('parent forwards child messages and errors while active, never after retirement', async () => {
  const { broker, workers } = fixture();
  const messages = [];
  const errors = [];
  broker.create(new URL('https://local.test/boot-worker.mjs'), {}, (data) => messages.push(data), (error) => errors.push(error));
  workers[0].dispatchEvent(new MessageEvent('message', { data: { ok: false, code: 'GUEST_FAULT' } }));
  const error = new Event('error');
  Object.assign(error, { message: 'Worker crashed.' });
  workers[0].dispatchEvent(error);
  await broker.terminateAll();
  workers[0].dispatchEvent(new MessageEvent('message', { data: { phase: 'running' } }));
  assert.deepEqual(messages, [{ ok: false, code: 'GUEST_FAULT' }]);
  assert.equal(errors[0].message, 'Worker crashed.');
  workers[0].messages[0].data.control.close();
});

test('termination attempts every owned worker even if one handle fails', async () => {
  const { broker, workers } = fixture();
  broker.create(new URL('https://local.test/boot-worker.mjs'));
  broker.create(new URL('https://local.test/story_boot.mjs'));
  const original = workers[0].terminate;
  workers[0].terminate = () => { throw new Error('Termination unavailable.'); };
  await assert.rejects(broker.terminateAll(), /Worker termination failed/);
  assert.equal(workers[1].terminated, true);
  workers[0].terminate = original;
  await broker.terminateAll();
  for (const worker of workers) { worker.messages[0].data.control.close(); }
});

test('failed bootstrap releases its local ports and terminates its allocated worker', async () => {
  const workers = [];
  class BrokenWorker extends EventTarget {
    constructor() { super(); workers.push(this); }
    postMessage = () => { throw new Error('Bootstrap transfer failed.'); };
    terminate = () => { this.terminated = true; };
  }
  const broker = createWorkerBroker({ WorkerClass: BrokenWorker, hostUrl: new URL('https://local.test/boot-worker-host.mjs') });
  assert.throws(() => broker.create(new URL('https://local.test/boot-worker.mjs')), /Bootstrap transfer failed/);
  assert.equal(workers[0].terminated, true);
  await broker.terminateAll();
});
