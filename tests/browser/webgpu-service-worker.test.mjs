import test from 'node:test';
import assert from 'node:assert/strict';
import { startGPUWorker } from '../../src/browser/webgpu-service.mjs';
import { createMailbox } from '../../src/browser/webgpu-transport.mjs';

const installWorker = ({ ready = true, stopped = true, resources = 0 } = {}) => {
  globalThis.location = { href: 'http://localhost/' };
  globalThis.Worker = class {
    postMessage(message) {
      if (message.type === 'start' && ready) { queueMicrotask(() => this.onmessage?.({ data: { type: 'ready' } })); }
      if (message.type === 'shutdown' && stopped) { queueMicrotask(() => this.onmessage?.({ data: { type: 'stopped', resources } })); }
    }
    terminate() { this.terminated = true; }
  };
};

test('disposal awaits zero-resource handshake and then terminates once', async () => {
  installWorker();
  const service = startGPUWorker({ canvas: {}, mailbox: createMailbox(), moduleURL: '/gpu.mjs' });
  await service.ready;
  const result = await service.dispose();
  assert.equal(result.resources, 0);
  assert.equal(result.forced, false);
  assert.equal(service.worker.terminated, true);
  assert.equal(await service.dispose(), result);
});

test('missing stopped handshake forces termination and reports failure', async () => {
  installWorker({ stopped: false });
  const diagnostics = [];
  const service = startGPUWorker({ canvas: {}, mailbox: createMailbox(), moduleURL: '/gpu.mjs', shutdownTimeoutMs: 5, onDiagnostic: value => diagnostics.push(value) });
  await service.ready;
  await assert.rejects(service.dispose(), /timed out/);
  assert.equal(service.worker.terminated, true);
  assert.equal(diagnostics.at(-1).forcedTermination, true);
});

test('initialization deadline rejects readiness and disposes worker', async () => {
  installWorker({ ready: false });
  const service = startGPUWorker({ canvas: {}, mailbox: createMailbox(), moduleURL: '/gpu.mjs', initTimeoutMs: 5 });
  await assert.rejects(service.ready, /initialization timed out/);
  await service.dispose();
  assert.equal(service.worker.terminated, true);
});

test('abort wakes mailbox and rejects readiness before initialization', async () => {
  installWorker({ ready: false });
  const signal = new AbortController();
  const mailbox = createMailbox();
  const service = startGPUWorker({ canvas: {}, mailbox, moduleURL: '/gpu.mjs', signal: signal.signal });
  signal.abort();
  await assert.rejects(service.ready, /cancelled/);
  await service.dispose();
  assert.equal(new Int32Array(mailbox)[0], 4);
});

test('nonzero resource handshake cannot claim successful cleanup', async () => {
  installWorker({ resources: 1 });
  const service = startGPUWorker({ canvas: {}, mailbox: createMailbox(), moduleURL: '/gpu.mjs' });
  await service.ready;
  await assert.rejects(service.dispose(), /resources/);
  assert.equal(service.worker.terminated, true);
});
