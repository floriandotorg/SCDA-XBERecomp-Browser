import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { Worker } from 'node:worker_threads';

const directory = resolve('out/menu-input-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/image_symbols.h`, `#pragma once
#define SYM_XNetStartup 00F11000
#define SYM_WSAStartup 00F12000
#define SYM_Direct3D_CreateDevice 00F13000
#define SYM_XInitDevices 004ECDE8
#define SYM_XInputOpen 004EDB24
#define SYM_XInputClose 004EDB7A
#define SYM_XInputGetState 004EDB86
#define SYM_XInputSetState 004EDBF9
#define SYM_XPP_GamepadType 004EC4D4
#define SYM_XPP_MemoryUnitType 004EC458
`);
writeFileSync(`${directory}/run.mjs`, "import create from './input.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} checked owned guest input ABI`, () => {
    const wasm = platform === 'wasm';
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=134217728', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=1048576', '-sPTHREAD_POOL_SIZE=2', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const output = `${directory}/${wasm ? 'input.mjs' : 'native'}`;
    const env = { ...process.env, TMPDIR: directory };
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', `-I${directory}`, '-Isrc', 'tests/browser/menu-input.cpp', 'src/browser/menu-input.cpp', 'src/browser/sdk-provider.cpp', 'src/browser/image.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000, env });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000, env });
    assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
    assert.match(run.stdout, /menu-input-contracts-ok/);
  });
}

const fixture = async (options = {}) => {
  const { createMenuInput, MENU_INPUT } = await import('../../src/browser/menu-input.mjs');
  const buffer = new SharedArrayBuffer(32);
  const words = new Uint32Array(buffer, 8, MENU_INPUT.words);
  const target = new EventTarget();
  const focusTarget = new EventTarget();
  const visibilityTarget = new EventTarget();
  visibilityTarget.hidden = false;
  const input = createMenuInput({ buffer, byteOffset: 8, target, focusTarget, visibilityTarget, ...options });
  const key = (type, code, values = {}) => {
    const event = new Event(type, { cancelable: true });
    Object.assign(event, { code, repeat: false, ...values });
    target.dispatchEvent(event);
    return event;
  };
  return { input, buffer, words, key, target, focusTarget, visibilityTarget, MENU_INPUT };
};

test('press release aliases opposing directions and repeat publish bounded latest state', async () => {
  const { input, words, key, MENU_INPUT } = await fixture();
  assert.equal(key('keydown', 'ArrowUp').defaultPrevented, true);
  assert.equal(words[0], MENU_INPUT.up);
  key('keydown', 'KeyW');
  assert.equal(words[0], MENU_INPUT.up | MENU_INPUT.forward);
  key('keydown', 'Space');
  key('keydown', 'KeyZ');
  assert.equal(words[0] & MENU_INPUT.confirm, MENU_INPUT.confirm);
  key('keyup', 'Space');
  assert.equal(words[0] & MENU_INPUT.confirm, MENU_INPUT.confirm);
  key('keyup', 'KeyZ');
  assert.equal(words[0] & MENU_INPUT.confirm, 0);
  key('keydown', 'Enter');
  assert.equal(words[0] & MENU_INPUT.start, MENU_INPUT.start);
  assert.equal(words[0] & MENU_INPUT.confirm, 0);
  const revision = words[1];
  for (let n = 0; n < 1000; ++n) { key('keydown', 'ArrowUp', { repeat: true }); }
  assert.equal(words[1], revision);
  assert.equal(key('keydown', 'F10').defaultPrevented, false);
  assert.equal(key('keydown', 'KeyS', { ctrlKey: true }).defaultPrevented, false);
  key('keyup', 'ArrowUp');
  assert.equal(words[0] & MENU_INPUT.up, 0);
  assert.equal(words[0] & MENU_INPUT.forward, MENU_INPUT.forward);
  key('keyup', 'KeyW');
  key('keyup', 'Enter');
  assert.equal(words[0], 0);
  input.dispose();
});

test('profile keyboard controls publish real analog buttons without synthesizing text', async () => {
  const { input, words, key, focusTarget, MENU_INPUT } = await fixture();
  const controls = [
    ['KeyX', 'x', 2048], ['KeyY', 'y', 4096],
    ['KeyQ', 'black', 8192], ['KeyE', 'white', 16384],
    ['BracketLeft', 'leftTrigger', 32768], ['BracketRight', 'rightTrigger', 65536],
  ];
  for (const [code, name, mask] of controls) {
    assert.equal(MENU_INPUT[name], mask);
    assert.equal(key('keydown', code).defaultPrevented, true);
    assert.equal(words[0], mask);
    key('keyup', code);
    assert.equal(words[0], 0);
  }
  key('keydown', 'KeyX');
  focusTarget.dispatchEvent(new Event('blur'));
  assert.equal(words[0], 0);
  focusTarget.dispatchEvent(new Event('focus'));
  key('keydown', 'KeyX', { repeat: true });
  assert.equal(words[0], 0);
  key('keyup', 'KeyX');
  key('keydown', 'KeyX');
  assert.equal(words[0], MENU_INPUT.x);
  input.dispose();
});

test('blur hide reactivation and disposal clear without replaying held repeat keys', async () => {
  const { input, words, key, focusTarget, visibilityTarget, MENU_INPUT } = await fixture();
  key('keydown', 'Escape');
  key('keydown', 'Backspace');
  key('keyup', 'Escape');
  assert.equal(words[0], MENU_INPUT.back);
  focusTarget.dispatchEvent(new Event('blur'));
  assert.equal(words[0], 0);
  key('keydown', 'ArrowDown');
  assert.equal(words[0], 0);
  focusTarget.dispatchEvent(new Event('focus'));
  key('keydown', 'ArrowDown', { repeat: true });
  assert.equal(words[0], 0);
  key('keyup', 'ArrowDown');
  key('keydown', 'ArrowDown');
  assert.equal(words[0], MENU_INPUT.down);
  visibilityTarget.hidden = true;
  visibilityTarget.dispatchEvent(new Event('visibilitychange'));
  assert.equal(words[0], 0);
  visibilityTarget.hidden = false;
  visibilityTarget.dispatchEvent(new Event('visibilitychange'));
  key('keydown', 'Space', { repeat: true });
  assert.equal(words[0], 0);
  input.setActive(false);
  key('keydown', 'Enter');
  input.setActive(true);
  key('keydown', 'Enter', { repeat: true });
  assert.equal(words[0], 0);
  key('keyup', 'Enter');
  key('keydown', 'Enter');
  assert.equal(words[0], MENU_INPUT.start);
  input.dispose();
  assert.equal(words[0], 0);
  key('keydown', 'ArrowUp');
  assert.equal(words[0], 0);
  input.dispose();
});

test('host focus transfer clears held input and suppresses repeats until release', async () => {
  const { input, words, key, target, MENU_INPUT } = await fixture();
  const canvas = { closest: () => null };
  const host = { closest: () => host };
  const on = (element) => ({ composedPath: () => [element, target] });
  key('keydown', 'ArrowDown', on(canvas));
  assert.equal(words[0], MENU_INPUT.down);
  const focusout = new Event('focusout');
  Object.assign(focusout, { relatedTarget: host, composedPath: () => [canvas, target] });
  target.dispatchEvent(focusout);
  assert.equal(words[0], 0);
  const focusin = new Event('focusin');
  Object.assign(focusin, { composedPath: () => [host, target] });
  target.dispatchEvent(focusin);
  assert.equal(key('keydown', 'ArrowDown', { ...on(host), repeat: true }).defaultPrevented, false);
  assert.equal(words[0], 0);
  assert.equal(key('keydown', 'KeyW', on(host)).defaultPrevented, false);
  key('keydown', 'ArrowDown', { ...on(canvas), repeat: true });
  assert.equal(words[0], 0);
  key('keyup', 'ArrowDown', on(host));
  key('keydown', 'ArrowDown', on(canvas));
  assert.equal(words[0], MENU_INPUT.down);
  input.dispose();
});

test('fresh presses recover when key releases occurred outside the active document', async () => {
  const { input, words, key, focusTarget, visibilityTarget, MENU_INPUT } = await fixture();
  const suspend = [
    [() => focusTarget.dispatchEvent(new Event('blur')), () => focusTarget.dispatchEvent(new Event('focus'))],
    [() => { visibilityTarget.hidden = true; visibilityTarget.dispatchEvent(new Event('visibilitychange')); },
      () => { visibilityTarget.hidden = false; visibilityTarget.dispatchEvent(new Event('visibilitychange')); }],
    [() => input.setActive(false), () => input.setActive(true)],
  ];
  for (const [pause, resume] of suspend) {
    key('keydown', 'ArrowDown');
    assert.equal(words[0], MENU_INPUT.down);
    pause();
    assert.equal(words[0], 0);
    resume();
    for (let n = 0; n < 100; ++n) { key('keydown', 'ArrowDown', { repeat: true }); }
    assert.equal(words[0], 0);
    key('keydown', 'ArrowDown');
    assert.equal(words[0], MENU_INPUT.down, 'fresh physical press must not require a stale keyup');
    key('keyup', 'ArrowDown');
    assert.equal(words[0], 0);
  }
  input.setActive(false);
  key('keydown', 'Space');
  input.setActive(true);
  key('keydown', 'Space', { repeat: true });
  assert.equal(words[0], 0);
  key('keydown', 'Space');
  assert.equal(words[0], MENU_INPUT.confirm);
  input.dispose();
});

test('diagnostic summary focus clears game input and preserves native disclosure keys', async () => {
  const { input, words, key, target, MENU_INPUT } = await fixture();
  const canvas = { closest: () => null };
  const summary = { closest: (selector) => selector.split(',').includes('summary') ? summary : null };
  key('keydown', 'ArrowDown', { composedPath: () => [canvas, target] });
  assert.equal(words[0], MENU_INPUT.down);
  const focusout = new Event('focusout');
  Object.assign(focusout, { relatedTarget: summary });
  target.dispatchEvent(focusout);
  assert.equal(words[0], 0);
  for (const code of ['Enter', 'Space', 'ArrowLeft', 'ArrowRight']) {
    assert.equal(key('keydown', code, { composedPath: () => [summary, target] }).defaultPrevented, false);
    assert.equal(words[0], 0);
    key('keyup', code, { composedPath: () => [summary, target] });
  }
  key('keyup', 'ArrowDown');
  key('keydown', 'ArrowDown', { composedPath: () => [canvas, target] });
  assert.equal(words[0], MENU_INPUT.down);
  input.dispose();
});

test('reconnecting to a retired producer starts neutral and ignores stale repeats', async () => {
  const { createMenuInput } = await import('../../src/browser/menu-input.mjs');
  const { input, words, key, target, focusTarget, visibilityTarget, MENU_INPUT } = await fixture();
  key('keydown', 'ArrowDown');
  assert.equal(words[0], MENU_INPUT.down);
  input.dispose();
  assert.equal(words[0], 0);
  const replacement = createMenuInput({ target, focusTarget, visibilityTarget, buffer: words.buffer, byteOffset: words.byteOffset });
  key('keydown', 'ArrowDown', { repeat: true });
  assert.equal(words[0], 0);
  key('keydown', 'Space');
  assert.equal(words[0], MENU_INPUT.confirm);
  key('keyup', 'Space');
  assert.equal(words[0], 0);
  replacement.dispose();
  key('keydown', 'Space');
  assert.equal(words[0], 0);
});

test('shared input span validates bounds and exclusive producer ownership', async () => {
  const { createMenuInput } = await import('../../src/browser/menu-input.mjs');
  const buffer = new SharedArrayBuffer(32);
  const target = new EventTarget();
  const options = { buffer, byteOffset: 8, target, focusTarget: target, visibilityTarget: target };
  new Uint32Array(buffer).fill(0x12345678);
  const input = createMenuInput(options);
  assert.equal(new Uint32Array(buffer)[0], 0x12345678);
  assert.equal(new Uint32Array(buffer)[4], 0x12345678);
  assert.throws(() => createMenuInput(options), /owned|producer/i);
  assert.throws(() => createMenuInput({ ...options, byteOffset: 12 }), /owned|producer|overlap/i);
  assert.throws(() => createMenuInput({ ...options, byteOffset: 3 }), /span|offset|align/i);
  assert.throws(() => createMenuInput({ ...options, byteOffset: 28 }), /span|range|bound/i);
  assert.throws(() => createMenuInput({ ...options, buffer: new ArrayBuffer(32) }), /shared/i);
  assert.throws(() => createMenuInput({ ...options, byteOffset: -4 }), /span|offset|range/i);
  assert.throws(() => createMenuInput({ ...options, byteOffset: NaN }), /span|offset|range/i);
  input.dispose();
  const replacement = createMenuInput(options);
  replacement.dispose();
});

test('activation is idempotent and abort releases the producer span', async () => {
  const controller = new AbortController();
  const { input, buffer, words, key, target, focusTarget, visibilityTarget, MENU_INPUT } = await fixture({ signal: controller.signal });
  key('keydown', 'Space');
  input.setActive(true);
  assert.equal(words[0], MENU_INPUT.confirm);
  controller.abort();
  assert.equal(words[0], 0);
  key('keydown', 'Enter');
  assert.equal(words[0], 0);
  const { createMenuInput } = await import('../../src/browser/menu-input.mjs');
  assert.throws(() => createMenuInput({ buffer, byteOffset: 8, target, focusTarget, visibilityTarget, signal: controller.signal }), { name: 'AbortError' });
  const replacement = createMenuInput({ buffer, byteOffset: 8, target, focusTarget, visibilityTarget });
  replacement.dispose();
});

test('main event producer stays responsive while a guest-like worker is blocked', async () => {
  const { input, buffer, words, key, MENU_INPUT } = await fixture();
  const gate = new SharedArrayBuffer(4);
  const worker = new Worker(`const { parentPort, workerData } = require('node:worker_threads');
const gate = new Int32Array(workerData.gate);
parentPort.postMessage('waiting');
Atomics.wait(gate, 0, 0);
parentPort.postMessage(Atomics.load(new Uint32Array(workerData.buffer, 8, 2), 0));`, { eval: true, workerData: { buffer, gate } });
  try {
    await new Promise((resolve, reject) => { worker.once('message', resolve); worker.once('error', reject); });
    key('keydown', 'Space');
    assert.equal(words[0], MENU_INPUT.confirm);
    const observed = new Promise((resolve, reject) => { worker.once('message', resolve); worker.once('error', reject); });
    Atomics.store(new Int32Array(gate), 0, 1);
    Atomics.notify(new Int32Array(gate), 0);
    assert.equal(await observed, MENU_INPUT.confirm);
  } finally {
    input.dispose();
    await worker.terminate();
  }
});
test('canvas focus refreshes a stale initial document focus sample', async () => {
  const { createMenuInput } = await import('../../src/browser/menu-input.mjs');
  const buffer = new SharedArrayBuffer(8);
  const target = new EventTarget();
  let focused = false;
  target.hasFocus = () => focused;
  const input = createMenuInput({ buffer, byteOffset: 0, target, focusTarget: target, visibilityTarget: target });
  focused = true;
  target.dispatchEvent(new Event('focusin'));
  const event = new Event('keydown', { cancelable: true });
  Object.assign(event, { code: 'KeyW', repeat: false });
  target.dispatchEvent(event);
  assert.equal(new Uint32Array(buffer)[0], 16);
  input.dispose();
});
test('a fresh physical key uses current document focus when the browser omits a window focus event', async () => {
  const { createMenuInput } = await import('../../src/browser/menu-input.mjs');
  const buffer = new SharedArrayBuffer(8);
  const target = new EventTarget();
  let focused = false;
  target.hasFocus = () => focused;
  const input = createMenuInput({ buffer, byteOffset: 0, target, focusTarget: target, visibilityTarget: target });
  const key = () => {
    const event = new Event('keydown', { cancelable: true });
    Object.assign(event, { code: 'KeyW', repeat: false });
    target.dispatchEvent(event);
  };
  focused = true;
  key();
  assert.equal(new Uint32Array(buffer)[0], 16);
  focused = false;
  target.dispatchEvent(new Event('blur'));
  key();
  assert.equal(new Uint32Array(buffer)[0], 0);
  input.dispose();
});
test('Escape unlocks capture without also pressing the game Back button', async () => {
  const { input, words, key, visibilityTarget } = await fixture();
  visibilityTarget.pointerLockElement = {};
  key('keydown', 'Escape');
  assert.equal(words[0], 0);
  visibilityTarget.pointerLockElement = null;
  key('keydown', 'Escape');
  assert.equal(words[0], 1024);
  input.dispose();
});
