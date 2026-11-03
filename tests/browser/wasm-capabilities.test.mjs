import { test } from 'node:test';
import assert from 'node:assert/strict';
import { assertWasmCapabilities } from '../../src/browser/wasm-capabilities.mjs';

const required = { wasmRequirements: ['tail-call'] };

test('default builds do not probe or require tail-call support', () => {
  const validate = () => { throw new Error('must not probe'); };
  assertWasmCapabilities({}, validate);
  assertWasmCapabilities({ wasmRequirements: [] }, validate);
});

test('tail builds validate an explicit minimal return_call module before execution', () => {
  let calls = 0;
  assertWasmCapabilities(required, (bytes) => {
    ++calls;
    assert.deepEqual([...bytes], [0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 96, 0, 0, 3, 2, 1, 0, 10, 6, 1, 4, 0, 18, 0, 11]);
    assert.equal(WebAssembly.validate(bytes), true);
    return true;
  });
  assert.equal(calls, 1);
});

test('false validation reports CAPABILITY without compiling or instantiating the app', () => {
  assert.throws(() => assertWasmCapabilities(required, () => false), (error) => error.code === 'CAPABILITY' && /tail.call/i.test(error.message));
});

test('unrelated validator failures are not mislabeled as capabilities', () => {
  const error = new WebAssembly.CompileError('unrelated validation problem');
  assert.throws(() => assertWasmCapabilities(required, () => { throw error; }), (actual) => actual === error && actual.code !== 'CAPABILITY');
});
