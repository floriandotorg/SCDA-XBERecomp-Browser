import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import vm from 'node:vm';

const helper = resolve('src/browser/abort-import.js');
const source = existsSync(helper) ? readFileSync(helper, 'utf8') : '';
const directory = resolve('out/m4-current/abort-import-tests');
mkdirSync(directory, { recursive: true });

test('import hook preserves values, receiver, thrown identity and bounded logging', () => {
  const failure = new Error('exact original failure');
  const diagnostics = [];
  const receiver = {};
  let instanceCalls = 0;
  const instantiateWasm = () => { ++instanceCalls; };
  const imports = {
    env: {
      memory: {},
      success(value) { assert.equal(this, receiver); return value; },
      failure() { throw failure; },
      unwind() { throw 'unwind'; },
    },
  };
  const context = vm.createContext({ getWasmImports: () => imports, err: (line) => diagnostics.push(line), Module: { instantiateWasm }, ENVIRONMENT_IS_PTHREAD: true });
  vm.runInContext(source, context);
  const wrapped = vm.runInContext('getWasmImports()', context);
  assert.equal(wrapped, imports);
  assert.equal(wrapped.env.memory, imports.env.memory);
  assert.equal(wrapped.env.success.call(receiver, 17), 17);
  assert.deepEqual(diagnostics, []);
  assert.throws(() => wrapped.env.unwind(), (error) => error === 'unwind');
  assert.deepEqual(diagnostics, []);
  for (let n = 0; n < 30; ++n) {
    assert.throws(() => wrapped.env.failure(), (error) => error === failure);
  }
  assert.match(diagnostics.join('\n'), /browser-import-throw.*env\.failure/);
  assert.match(diagnostics.join('\n'), /exact original failure/);
  assert.match(diagnostics.join('\n'), /Error: exact original failure/);
  assert.ok(diagnostics.length <= 12);
  assert.ok(diagnostics.every((line) => line.length <= 511));
  assert.equal(vm.runInContext('Module.instantiateWasm', context), instantiateWasm);
  assert.equal(instanceCalls, 0);
  const firstWrapper = wrapped.env.failure;
  vm.runInContext('getWasmImports()', context);
  assert.equal(wrapped.env.failure, firstWrapper);
});

test('unwind sentinel logs only in the main runtime and always preserves exact thrown value', () => {
  for (const pthread of [false, true]) {
    const diagnostics = [];
    const imports = { env: { unwind: () => { throw 'unwind'; } } };
    const context = vm.createContext({ getWasmImports: () => imports, err: (line) => diagnostics.push(line), ENVIRONMENT_IS_PTHREAD: pthread });
    vm.runInContext(source, context);
    const wrapped = vm.runInContext('getWasmImports()', context);
    for (let n = 0; n < 20; ++n) {
      assert.throws(() => wrapped.env.unwind(), (error) => error === 'unwind');
    }
    if (pthread) {
      assert.deepEqual(diagnostics, []);
    } else {
      assert.match(diagnostics.join('\n'), /browser-import-throw.*env\.unwind.*runtime=main.*message=unwind.*stack=Error/);
      assert.ok(diagnostics.length <= 8);
      assert.ok(diagnostics.every((line) => line.length <= 511));
    }
  }
  const imports = { env: { unwind: () => { throw 'unwind'; } } };
  const context = vm.createContext({ getWasmImports: () => imports, err: () => { throw new Error('sink failed'); }, ENVIRONMENT_IS_PTHREAD: false });
  vm.runInContext(source, context);
  assert.throws(() => vm.runInContext('getWasmImports().env.unwind()', context), (error) => error === 'unwind');
});

test('pthread entry capture preserves receiver, arguments, returns and thrown identity without proxies or blocking', () => {
  const heap = new Uint8Array(new SharedArrayBuffer(8192));
  const original = new WebAssembly.RuntimeError('public entry trap');
  original.stack = `RuntimeError: public entry trap\n${'public frame\n'.repeat(1000)}`;
  const receiver = {};
  let failure;
  const invoke = function (entry, argument) {
    assert.equal(this, receiver);
    assert.equal(entry, 17);
    assert.equal(argument, 29);
    if (failure !== undefined) { throw failure; }
    return 71;
  };
  const context = vm.createContext({ getWasmImports: () => ({ env: {} }), ENVIRONMENT_IS_PTHREAD: true,
    invokeEntryPoint: invoke, Module: { _browser_abort_thread_failure: () => 4 }, HEAPU8: heap, SharedArrayBuffer, TextEncoder });
  vm.runInContext(source, context);
  vm.runInContext('getWasmImports(); getWasmImports()', context);
  const wrapped = vm.runInContext('invokeEntryPoint', context);
  assert.equal(wrapped.call(receiver, 17, 29), 71);
  failure = 'unwind';
  assert.throws(() => wrapped.call(receiver, 17, 29), (error) => error === 'unwind');
  assert.equal(Atomics.load(new Int32Array(heap.buffer), 1), 0);
  failure = original;
  assert.throws(() => wrapped.call(receiver, 17, 29), (error) => error === original);
  const length = Atomics.load(new Int32Array(heap.buffer), 1);
  assert.ok(length > 0 && length <= 2600);
  const detail = new TextDecoder().decode(heap.subarray(8, 8 + length));
  assert.match(detail, /entry=17 arg=29 error=RuntimeError: public entry trap.*stack=RuntimeError: public entry trap/);
  assert.throws(() => wrapped.call(receiver, 17, 29), (error) => error === original);
  assert.equal(Atomics.load(new Int32Array(heap.buffer), 1), length);
  assert.equal(new TextDecoder().decode(heap.subarray(8, 8 + length)), detail);
  context.Module._browser_abort_thread_failure = () => heap.length - 2;
  assert.throws(() => wrapped.call(receiver, 17, 29), (error) => error === original);
  context.Module._browser_abort_thread_failure = () => { throw new Error('diagnostic unavailable'); };
  assert.throws(() => wrapped.call(receiver, 17, 29), (error) => error === original);
});

test('pthread entry capture expands only diagnostic stack depth and restores the original limit', () => {
  let seen;
  const original = Error.stackTraceLimit;
  const failure = new Error('exact failure');
  const context = vm.createContext({ getWasmImports: () => ({ env: {} }), ENVIRONMENT_IS_PTHREAD: true, Error,
    invokeEntryPoint: () => { seen = Error.stackTraceLimit; throw failure; }, Module: {} });
  vm.runInContext(source, context);
  vm.runInContext('getWasmImports()', context);
  assert.throws(() => vm.runInContext('invokeEntryPoint(1, 2)', context), (error) => error === failure);
  assert.ok(seen >= 4096);
  assert.equal(Error.stackTraceLimit, original);
});

test('stack-limit access and restoration failures never replace pthread entry results or exceptions', () => {
  for (const phase of ['read', 'write', 'restore']) {
    const failure = { message: 'unchanged pthread failure' };
    let writes = 0;
    const BrokenError = class extends Error {};
    Object.defineProperty(BrokenError, 'stackTraceLimit', {
      get: () => { if (phase === 'read') { throw new Error('broken read'); } return 10; },
      set: () => { ++writes; if (phase === 'write' || writes > 1) { throw new Error('broken write'); } },
    });
    const context = vm.createContext({ getWasmImports: () => ({ env: {} }), ENVIRONMENT_IS_PTHREAD: true, Error: BrokenError,
      invokeEntryPoint: (entry) => { if (entry === 1) { throw failure; } return 17; }, Module: {} });
    vm.runInContext(source, context);
    vm.runInContext('getWasmImports()', context);
    assert.equal(vm.runInContext('invokeEntryPoint(0, 2)', context), 17);
    assert.throws(() => vm.runInContext('invokeEntryPoint(1, 2)', context), (error) => error === failure);
  }
});

test('diagnostic sink failure does not replace the original imported JS exception', () => {
  const failure = Object.freeze({ primary: true });
  const imports = { env: { failure: () => { throw failure; } } };
  const context = vm.createContext({ getWasmImports: () => imports, err: () => { throw new Error('sink failed'); } });
  vm.runInContext(source, context);
  const wrapped = vm.runInContext('getWasmImports()', context);
  assert.throws(() => wrapped.env.failure(), (error) => error === failure);
});

test('real Wasm captures the primary JS import throw before joinable Worker termination', () => {
  const output = `${directory}/foreign.mjs`;
  const compile = spawnSync('em++', ['-std=c++20', '-O3', '-pthread', '-fwasm-exceptions', '-Isrc', '-DX86_PORTABLE_RUNTIME', '-DBROWSER_MENU_TRACE_ABORTS=1', 'tests/browser/abort-import.cpp', 'src/browser/cpu_runtime.cpp', '--no-entry', '-Wl,--wrap=pthread_join', '-sPTHREAD_POOL_SIZE=1', '-sENVIRONMENT=node', '-sINITIAL_MEMORY=134217728', '--profiling-funcs', ...(existsSync(helper) ? ['--pre-js', helper] : []), '-o', output], { encoding: 'utf8', timeout: 120000 });
  assert.equal(compile.status, 0, compile.stderr);
  for (const phase of ['direct', 'unwind', 'join']) {
    const runner = `${directory}/${phase}-run.mjs`;
    const direct = phase === 'unwind' ? '_unwind_direct' : '_throw_direct';
    const expected = phase === 'unwind' ? "'unwind'" : 'globalThis.browserPrimaryError';
    writeFileSync(runner, `import assert from 'node:assert/strict';\nimport create from './foreign.mjs';\nconst lines = []; const module = await create({ printErr: (line) => { lines.push(line); process.stderr.write(line + '\\n'); } });\n${phase !== 'join' ? `try { module.${direct}(); } catch (error) { assert.equal(error, ${expected}); assert.ok(lines.some((line) => line.includes('[browser-import-throw]'))); process.exit(0); } process.exit(76);` : 'module._throw_during_join(); process.exit(77);'}\n`);
    const run = spawnSync(process.execPath, [runner], { encoding: 'utf8', timeout: 20000 });
    const log = `${run.stdout ?? ''}${run.stderr ?? ''}`;
    writeFileSync(`${directory}/${phase}.log`, log);
    if (phase !== 'join') {
      assert.equal(run.status, 0, log);
      if (phase === 'unwind') {
        assert.match(log, /runtime=main message=unwind/);
      }
    } else {
      assert.notEqual(run.status, 0, log);
      const primary = log.indexOf('[browser-import-throw]');
      const destructor = log.indexOf('phase=destroy-joinable');
      assert.ok(primary >= 0 && destructor > primary, log);
      assert.match(log, /retirement primary JS error/);
      assert.match(log, /phase=destroy-joinable .*done=0 uncaught=0 caught=0/);
    }
  }
});
