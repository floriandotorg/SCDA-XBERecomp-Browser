import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createBootSession } from '../../src/browser/boot-session.mjs';
import { createIsoMailbox } from '../../src/browser/iso-mailbox.mjs';

const directory = resolve('out/m4-current/abort-diagnostic-tests');
mkdirSync(`${directory}/tmp`, { recursive: true });
const environment = { ...process.env, TMPDIR: `${directory}/tmp` };
const build = (name, traced, threads = false) => {
  const output = `${directory}/${name}.mjs`;
  const result = spawnSync('em++', ['-std=c++20', '-O3', '-fwasm-exceptions', '-pthread',
    'tests/browser/abort-diagnostic.cpp', ...(traced ? ['src/browser/abort-diagnostic.cpp', '--profiling-funcs', '-Wl,--wrap=abort'] : []),
    '--no-entry', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0',
    '-sINITIAL_MEMORY=67108864', '-sMAXIMUM_MEMORY=268435456', '-sALLOW_MEMORY_GROWTH=1',
    '-sEXPORTED_RUNTIME_METHODS=HEAPU8', ...(threads ? ['-DBROWSER_ABORT_THREAD_TEST', '-sPTHREAD_POOL_SIZE=1'] : []), '-o', output], { encoding: 'utf8', timeout: 120000, env: environment });
  writeFileSync(`${directory}/${name}-build.log`, `${result.stdout ?? ''}${result.stderr ?? ''}`);
  assert.equal(result.status, 0, result.stderr);
  return output;
};
const reconstructStack = (stderr) => stderr.filter((text) => text.startsWith('[browser-abort-stack]')).map((text, n) => {
  const prefix = `[browser-abort-stack] ${n}:`;
  assert.ok(text.startsWith(prefix), text);
  assert.ok(text.length <= 512);
  return text.slice(prefix.length);
}).join('');
const exercise = async (path, depth = 0, broken = '', exceptionKind, caught = true, requestedLimit) => {
  const { default: create } = await import(pathToFileURL(path).href);
  const stderr = [];
  const aborts = [];
  const module = await create({ printErr: (text) => {
    if (broken === 'stderr' && text.startsWith('[browser-abort-')) {
      throw new Error('synthetic diagnostic sink failure');
    }
    stderr.push(text);
  }, onAbort: (reason) => aborts.push(reason) });
  const pointer = module._abort_sentinel();
  const before = module.HEAPU8.slice(pointer, pointer + 4);
  const heapBytes = module.HEAPU8.byteLength;
  const stackLimit = Error.stackTraceLimit;
  const NativeError = globalThis.Error;
  try {
    const limit = requestedLimit ?? (depth ? 1000 : 30);
    Error.stackTraceLimit = limit;
    if (broken === 'capture') {
      globalThis.Error = class extends NativeError {
        constructor(...arguments_) {
          super(...arguments_);
          Object.defineProperty(this, 'stack', { get: () => { throw new NativeError('synthetic stack failure'); } });
        }
      };
    }
    assert.throws(() => exceptionKind === undefined ? module._abort_fixture(depth) : module._abort_exception_fixture(exceptionKind, caught ? 1 : 0), WebAssembly.RuntimeError);
    assert.equal(Error.stackTraceLimit, limit);
  } finally {
    globalThis.Error = NativeError;
    Error.stackTraceLimit = stackLimit;
  }
  assert.deepEqual(aborts, ['']);
  assert.deepEqual(module.HEAPU8.slice(pointer, pointer + 4), before);
  assert.equal(module.HEAPU8.byteLength, heapBytes);
  assert.ok(stderr.includes('Aborted()'));
  if (broken === 'stderr') {
    assert.deepEqual(stderr, ['Aborted()']);
  }
  if (broken === 'capture') {
    assert.ok(!stderr.some((text) => text.startsWith('[browser-abort-stack]')));
  }
  return stderr;
};

test('public Wasm control demonstrates the opaque abort without diagnostics', async () => {
  const stderr = await exercise(build('opaque', false));
  assert.deepEqual(stderr, ['Aborted()']);
});

test('wrapped real Wasm abort includes original named C++ caller without changing failure or memory', async () => {
  const output = build('traced', true);
  const stderr = await exercise(output);
  const trace = reconstructStack(stderr);
  assert.ok(trace, JSON.stringify(stderr));
  assert.match(trace, /AbortRoot\(\).*wasm-function\[/);
  assert.match(trace, /__wrap_abort/);
  assert.ok(Buffer.byteLength(trace) <= 4140);
  writeFileSync(`${directory}/traced-stderr.log`, stderr.join('\n'));

  const long = await exercise(output, 200);
  const longTrace = reconstructStack(long);
  assert.ok(Buffer.byteLength(longTrace) <= 4140);
  assert.match(longTrace, /AbortRoot/);
  assert.match(longTrace, /LongAbortStack/);
  assert.match(longTrace, /\[truncated\]$/);
  await exercise(output, 0, 'stderr');
  await exercise(output, 0, 'capture');
});

test('abort capture expands default stack depth only temporarily and restores on sink or capture failure', async () => {
  const output = build('depth-restoration', true);
  const stderr = await exercise(output, 20, '', undefined, true, 10);
  const trace = reconstructStack(stderr);
  assert.ok((trace.match(/LongAbortStack/g) ?? []).length > 10, trace);
  assert.match(trace, /abort_fixture/);
  assert.ok(Buffer.byteLength(trace) <= 4140);
  assert.ok(stderr.length <= 11);
  await exercise(output, 0, 'stderr', undefined, true, 3);
  await exercise(output, 0, 'capture', undefined, true, 3);
});

test('active caught exceptions report actual bounded what while noexcept reports SDK current_exception availability', async () => {
  const output = `${directory}/traced.mjs`;
  const active = await exercise(output, 0, '', 0);
  assert.ok(active.some((text) => text.includes('public active exception payload')), JSON.stringify(active));
  assert.ok(active.some((text) => text.startsWith('[browser-abort-exception] 0:public active exception payload')));
  assert.match(reconstructStack(active), /CaughtTerminateRoot/);
  const noexcept = await exercise(output, 0, '', 0, false);
  assert.match(reconstructStack(noexcept), /NoexceptAbortRoot/);
  assert.ok(noexcept.includes('[browser-abort-exception] 0:[std::current_exception empty]'));
  const inactive = await exercise(output);
  assert.ok(inactive.includes('[browser-abort-exception] 0:[std::current_exception empty]'));
  const nonstandard = await exercise(output, 0, '', 2);
  assert.ok(nonstandard.includes('[browser-abort-exception] 0:[non-std C++ exception]'));
  const long = await exercise(output, 0, '', 1);
  const messages = long.filter((text) => text.startsWith('[browser-abort-exception]'));
  assert.ok(messages.length <= 3);
  assert.ok(messages.every((text) => text.length <= 512));
  assert.equal(messages.map((text, n) => text.slice(`[browser-abort-exception] ${n}:`.length)).join(''), 'x'.repeat(1024));
  await exercise(output, 0, 'stderr', 0);
  await exercise(output, 0, 'capture', 0);
  writeFileSync(`${directory}/active-exception-stderr.log`, active.join('\n'));
});

test('actual long Wasm abort survives unchanged boot-session stderr admission budgets', async () => {
  const { default: create } = await import(pathToFileURL(`${directory}/traced.mjs`).href);
  const ownership = new Int32Array(new SharedArrayBuffer(4));
  Atomics.store(ownership, 0, 1);
  const original = [];
  const results = [];
  const session = createBootSession({
    create: async (hooks) => {
      const module = await create({ ...hooks, printErr: (text) => { original.push(text); hooks.printErr(text); } });
      module._abort_fixture(200);
      return module;
    },
    manifest: {}, postMessage: (result) => results.push(result),
  });
  const stackLimit = Error.stackTraceLimit;
  try {
    Error.stackTraceLimit = 1000;
    await session({ data: { port: { close: () => {} }, mailbox: createIsoMailbox(), generation: 1, ownership, size: 4096 } });
  } finally {
    Error.stackTraceLimit = stackLimit;
  }
  assert.equal(results.length, 1);
  const result = results[0];
  assert.equal(result.ok, false);
  assert.equal(result.code, 'WASM_ABORT');
  assert.equal(result.wasmDiagnostics.abortReason, '');
  assert.equal(result.wasmDiagnostics.caughtMessage, 'unreachable');
  assert.equal(result.wasmDiagnostics.stderrTruncated, false);
  assert.deepEqual(result.wasmDiagnostics.stderr, original);
  assert.ok(original.length <= 11);
  const stack = reconstructStack(result.wasmDiagnostics.stderr);
  assert.match(stack, /AbortRoot\(\).*wasm-function\[/);
  assert.match(stack, /LongAbortStack/);
  assert.match(stack, /\[truncated\]$/);
  assert.ok(Buffer.byteLength(stack) >= 4000 && Buffer.byteLength(stack) <= 4140);
  writeFileSync(`${directory}/boot-session-chunks.json`, JSON.stringify(result, null, 2));
});

test('wrapped pthread abort captures the original failing thread, not its parent', () => {
  build('threaded', true, true);
  const runner = `${directory}/thread-run.mjs`;
  writeFileSync(runner, `import create from './threaded.mjs';
const module = await create();
module._abort_thread_fixture();
setTimeout(() => process.exit(74), 10000);
`);
  const run = spawnSync(process.execPath, [runner], { encoding: 'utf8', timeout: 20000, env: environment });
  writeFileSync(`${directory}/thread-stderr.log`, `${run.stdout ?? ''}${run.stderr ?? ''}`);
  assert.notEqual(run.status, 0, run.stderr);
  assert.notEqual(run.status, 74, run.stderr);
  assert.match(run.stderr, /\[browser-abort-stack\]/);
  assert.match(run.stderr, /AbortThreadRoot\(\).*wasm-function\[/);
  assert.match(run.stderr, /AbortRoot\(\).*wasm-function\[/);
  assert.match(run.stderr, /Aborted\(\)/);
  assert.match(run.stderr, /RuntimeError/);
});
