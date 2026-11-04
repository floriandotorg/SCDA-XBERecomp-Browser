import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { runInNewContext } from 'node:vm';

const source = () => readFileSync(new URL('../../src/browser/webgpu-pthreads.js', import.meta.url), 'utf8');
const create = (options = {}) => {
  const sent = [];
  const listeners = new Map();
  const worker = () => ({ postMessage: message => sent.push(message) });
  const Module = { webgpuMailbox: new SharedArrayBuffer(4194368), ...options };
  const PThread = { unusedWorkers: [worker(), worker()], allocateUnusedWorker: worker };
  runInNewContext(source(), { Module, PThread, SharedArrayBuffer, addEventListener: (type, listener) => listeners.set(type, listener) });
  return { Module, PThread, sent, message: data => listeners.get('message')({ data }) };
};
test('GPU bindings reach current and later pthreads before entry while preserving setup hooks', () => {
  let prepared = 0;
  const fixture = create({ preRun: () => { ++prepared; } });
  for (const hook of fixture.Module.preRun) { hook(); }
  fixture.PThread.allocateUnusedWorker();
  assert.equal(prepared, 1);
  assert.equal(fixture.sent.length, 3);
  for (const message of fixture.sent) {
    assert.equal(message.type, 'browser-webgpu-mailbox');
    assert.equal(message.mailbox, fixture.Module.webgpuMailbox);
  }
});
test('worker bindings require a bounded shared mailbox and cannot change owners', () => {
  const fixture = create({ webgpuMailbox: undefined });
  for (const data of [null, {}, { type: 'different' }, { type: 'browser-webgpu-mailbox', mailbox: new ArrayBuffer(4194368) },
    { type: 'browser-webgpu-mailbox', mailbox: new SharedArrayBuffer(64) }]) { fixture.message(data); }
  assert.equal(fixture.Module.webgpuMailbox, undefined);
  const mailbox = new SharedArrayBuffer(4194368);
  fixture.message({ type: 'browser-webgpu-mailbox', mailbox });
  fixture.message({ type: 'browser-webgpu-mailbox', mailbox });
  assert.equal(fixture.Module.webgpuMailbox, mailbox);
  assert.throws(() => fixture.message({ type: 'browser-webgpu-mailbox', mailbox: new SharedArrayBuffer(4194368) }), /ownership/);
  assert.equal(fixture.Module.webgpuMailbox, mailbox);
});
test('explicit fallback mode leaves pthread bindings absent', () => {
  const fixture = create({ webgpuPthreads: false });
  for (const hook of fixture.Module.preRun) { hook(); }
  fixture.PThread.allocateUnusedWorker();
  assert.equal(fixture.sent.length, 0);
});
