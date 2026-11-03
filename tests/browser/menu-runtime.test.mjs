import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createMenuRuntime } from '../../src/browser/menu-runtime.mjs';

const fixture = (ready = Promise.resolve()) => {
  const events = [];
  let factory;
  let callbacks;
  let owned = true;
  const abort = new AbortController();
  const runtime = createMenuRuntime({
    acquireCanvas: () => ({ offscreen: true }),
    createRuntime: (options) => { factory = options.createServices; return { marker: true }; },
    createGPU: (options) => {
      callbacks = options;
      return { ready, webgpuMailbox: options.mailbox, cancel: () => events.push('cancel'), dispose: async () => events.push('dispose') };
    },
    createInput: (options) => { events.push(['input', options]); return { dispose: () => events.push('input-dispose') }; },
    onPresented: (details) => events.push(['presented', details]),
    onAdapter: (details) => events.push(['adapter', details]),
    onDiagnostic: (details) => events.push(['diagnostic', details]),
  });
  const context = { generation: 3, signal: abort.signal, owned: () => owned, report: (data) => events.push(['report', data]) };
  return { runtime, context, events, abort, retire: () => { owned = false; },
    start: () => factory(context), get callbacks() { return callbacks; } };
};

test('menu services use only completed genuine Present events to activate host rendering state', async () => {
  const harness = fixture();
  assert.equal(harness.runtime.marker, true);
  const services = await harness.start();
  assert.ok(services.webgpuMailbox instanceof SharedArrayBuffer);
  harness.callbacks.onAdapter({ source: 'service-adapter' });
  assert.equal(harness.events.some((event) => event[0] === 'report'), false);
  harness.callbacks.onPresented({ command: 4, resource: 1, resources: 3 });
  harness.callbacks.onPresented({ command: 5, resource: 1, resources: 3 });
  assert.equal(harness.events.filter((event) => event[0] === 'report' && event[1].phase === 'running').length, 1);
  const buffer = new SharedArrayBuffer(32);
  assert.equal(services.handle({ generation: 3, phase: 'input', buffer, byteOffset: 8, words: 2 }), true);
  assert.equal(harness.events.find((event) => event[0] === 'input')[1].byteOffset, 8);
  services.cancel();
  await services.dispose();
  harness.retire();
  harness.callbacks.onPresented({ command: 6, resource: 1, resources: 3 });
  assert.equal(harness.events.filter((event) => event === 'input-dispose').length, 1);
  assert.equal(harness.events.filter((event) => event === 'cancel').length, 1);
  assert.equal(harness.events.filter((event) => event === 'dispose').length, 1);
  assert.equal(harness.events.filter((event) => event[0] === 'presented').length, 2);
});

test('guest render diagnostics reach the host without becoming validation errors or consuming the outcome', async () => {
  const harness = fixture();
  const services = await harness.start();
  const guest = { generation: 3, ok: false, code: 'GUEST_FAULT', graphics: { renderedFrames: 0, state: { vertex: { program: [1, 2, 3, 4] } } } };
  assert.equal(services.handle(guest), false);
  assert.deepEqual(harness.events.find((event) => event[0] === 'diagnostic'), ['diagnostic', { guest }]);
  assert.equal(harness.events.some((event) => event[0] === 'report'), false);
  const invalidGraphics = { generation: 3, code: 'GRAPHICS_PROTOCOL', fileOpens: [{ status: 0xc0000033 }] };
  assert.equal(services.handle(invalidGraphics), false);
  assert.deepEqual(harness.events.filter((event) => event[0] === 'diagnostic')[1], ['diagnostic', { guest: invalidGraphics }]);
  harness.retire();
  services.handle(guest);
  assert.equal(harness.events.filter((event) => event[0] === 'diagnostic').length, 2);
  await services.dispose();
});

test('owned Wasm abort diagnostics survive without graphics or a successful Present', async () => {
  const harness = fixture();
  const services = await harness.start();
  const guest = { generation: 3, ok: false, code: 'WASM_ABORT', message: 'synthetic abort',
    wasmDiagnostics: { abortReason: 'synthetic abort', caughtMessage: 'unreachable', stderr: ['synthetic stderr'], stderrTruncated: false } };
  assert.equal(services.handle(guest), false);
  assert.deepEqual(harness.events.filter((event) => event[0] === 'diagnostic'), [['diagnostic', { guest }]]);
  assert.equal(harness.events.some((event) => event[0] === 'report' || event[0] === 'presented'), false);
  harness.retire();
  services.handle(guest);
  assert.equal(harness.events.filter((event) => event[0] === 'diagnostic').length, 1);
  await services.dispose();
});

test('abort during service initialization wakes and disposes its owned service', async () => {
  const harness = fixture(new Promise(() => {}));
  const starting = harness.start();
  await new Promise((resolve) => setImmediate(resolve));
  harness.abort.abort();
  await assert.rejects(starting, { name: 'AbortError' });
  assert.deepEqual(harness.events, ['cancel', 'dispose']);
});

test('validation errors identify commands and cannot report running', async () => {
  const harness = fixture();
  const services = await harness.start();
  harness.callbacks.onDiagnostic({ command: 17, resource: 9, status: 1, message: 'invalid binding' });
  const report = harness.events.find((event) => event[0] === 'report')[1];
  assert.equal(report.code, 'WEBGPU_VALIDATION');
  assert.match(report.message, /17.*9.*invalid binding/);
  assert.equal(harness.events.some((event) => event[0] === 'presented'), false);
  await services.dispose();
});
