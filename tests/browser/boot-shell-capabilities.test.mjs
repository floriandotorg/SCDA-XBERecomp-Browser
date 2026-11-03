import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mountBootShell } from '../../src/browser/boot-shell.mjs';

const root = () => {
  const controls = new Map(['iso', 'boot', 'stop', 'status', 'detail', 'selected'].map((name) => [name, {
    dataset: {}, addEventListener() {}, removeEventListener() {}, setAttribute() {},
  }]));
  return { querySelector: (selector) => controls.get(selector.slice(1)) };
};

const isolated = (context) => {
  for (const [name, value] of [['crossOriginIsolated', true], ['Worker', class {}]]) {
    const original = Object.getOwnPropertyDescriptor(globalThis, name);
    Object.defineProperty(globalThis, name, { value, configurable: true });
    context.after(() => {
      if (original) { Object.defineProperty(globalThis, name, original); }
      else { delete globalThis[name]; }
    });
  }
};

const source = { name: 'public.iso', size: 100, slice: () => { throw new Error('ISO must not be read by capability preflight'); } };

test('unsupported build-owned tail requirement refuses before runtime creation or ISO reads', async (context) => {
  isolated(context);
  let runtimes = 0;
  const shell = mountBootShell({ root: root(), loadManifest: async () => ({ wasmRequirements: ['tail-call'] }), validateWasm: () => false,
    createRuntime: () => { ++runtimes; throw new Error('must not create runtime'); } });
  await shell.controller.select(source);
  assert.equal((await shell.controller.start()).code, 'CAPABILITY');
  assert.equal(runtimes, 0);
  assert.equal(shell.controller.state.stopping, false);
  await shell.dispose();
});

test('supported and default builds preserve distinct compile/runtime failures', async (context) => {
  isolated(context);
  for (const wasmRequirements of [[], ['tail-call']]) {
    let probes = 0;
    let runtimes = 0;
    const shell = mountBootShell({ root: root(), loadManifest: async () => ({ wasmRequirements }), validateWasm: () => { ++probes; return true; },
      createRuntime: () => {
        ++runtimes;
        return { start: async () => { throw new WebAssembly.CompileError('unrelated application compilation failure'); },
          cancelIo() {}, terminateAll: async () => {}, sampleProgress() {} };
      } });
    await shell.controller.select(source);
    const result = await shell.controller.start();
    assert.equal(result.code, 'BOOT');
    assert.match(result.message, /unrelated application compilation failure/);
    assert.equal(runtimes, 1);
    assert.equal(probes, wasmRequirements.length);
    await shell.dispose();
  }
});

test('manifest loading errors are not rewritten to unsupported engine errors', async (context) => {
  isolated(context);
  const shell = mountBootShell({ root: root(), loadManifest: async () => { throw Object.assign(new Error('deployment manifest missing'), { code: 'DEPLOYMENT' }); },
    createRuntime: () => { throw new Error('must not execute'); } });
  await shell.controller.select(source);
  assert.equal((await shell.controller.start()).code, 'DEPLOYMENT');
  await shell.dispose();
});
