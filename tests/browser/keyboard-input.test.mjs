import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { KEYBOARD_INPUT, createKeyboardInput, createWheelAccumulator } from '../../src/browser/keyboard-input.mjs';

const K = KEYBOARD_INPUT;

test('the layout matches keyboard-input.h and stays inside its words', () => {
  const header = readFileSync('src/browser/keyboard-input.h', 'utf8');
  const constant = name => Number(new RegExp(`\\b${name} = (\\d+)`).exec(header)?.[1]);
  const names = { words: 'kWords', change: 'kChange', keys: 'kKeys', wheelUp: 'kWheelUp', wheelDown: 'kWheelDown', pressing: 'kPressing',
    clicks: 'kClicks', cursorX: 'kCursorX', cursorY: 'kCursorY', cursorW: 'kCursorW', cursorH: 'kCursorH', active: 'kActive',
    mouseX: 'kMouseX', mouseY: 'kMouseY', startTaps: 'kStartTaps', charHead: 'kCharHead', chars: 'kChars', charUnits: 'kCharUnits',
    flags: 'kFlags', hostPanel: 'kHostPanel', hostConsole: 'kHostConsole', menuFlag: 'kMenuFlag', pointerFlag: 'kPointerFlag',
    textFlag: 'kTextFlag', mapDragFlag: 'kMapDragFlag', movieFlag: 'kMovieFlag', captureFlag: 'kCaptureFlag' };
  for (const [key, name] of Object.entries(names)) { assert.equal(K[key], constant(name), key); }
  assert.ok(K.keys + 8 <= K.wheelUp);
  assert.ok(K.chars + K.charUnits / 2 <= K.flags);
  assert.ok(K.hostConsole < K.words);
});

test('wheel deltas become whole notches, up positive, per delta mode', () => {
  const wheel = createWheelAccumulator();
  assert.equal(wheel(-100, 0), 1);
  assert.equal(wheel(-40, 0), 0);
  assert.equal(wheel(-60, 0), 1);
  assert.equal(wheel(250, 0), -2);
  assert.equal(wheel(50, 0), -1);
  assert.equal(wheel(30, 0), 0);
  assert.equal(wheel(-70, 0), 0, 'a turn the other way drops the part notch');
  assert.equal(wheel(-30, 0), 1);
  assert.equal(createWheelAccumulator()(3, 1), -1);
  assert.equal(createWheelAccumulator()(-1, 2), 1);
  assert.equal(wheel(0, 0), 0);
  assert.equal(wheel(Number.NaN, 0), 0);
});

const fixture = () => {
  const buffer = new SharedArrayBuffer(16 + K.words * 4 + 16);
  const words = new Int32Array(buffer, 16, K.words);
  const target = new EventTarget();
  const document = new EventTarget();
  Object.assign(document, { hidden: false, hasFocus: () => true, pointerLockElement: null, exitPointerLock() { document.pointerLockElement = null; } });
  const canvas = new EventTarget();
  canvas.style = {};
  canvas.getBoundingClientRect = () => ({ left: 10, top: 20, width: 640, height: 480 });
  const frames = [];
  const input = createKeyboardInput({ buffer, byteOffset: 16, target, document, canvas: () => canvas,
    requestFrame: callback => frames.push(callback), cancelFrame: () => {} });
  const event = (type, fields = {}) => {
    const value = new Event(type, { cancelable: true });
    for (const [key, item] of Object.entries(fields)) { Object.defineProperty(value, key, { value: item }); }
    return value;
  };
  const dispatch = (type, fields) => { const value = event(type, { target: canvas, ...fields }); target.dispatchEvent(value); return value; };
  const tick = () => frames.splice(0).forEach(callback => callback());
  return { words, target, document, canvas, input, dispatch, tick };
};

test('keys go into the bitmap by virtual key code and come out on blur', () => {
  const f = fixture();
  assert.equal(f.words[K.active], 1);
  const press = f.dispatch('keydown', { code: 'KeyW', key: 'w' });
  assert.ok(press.defaultPrevented);
  assert.equal(f.words[K.keys + (0x57 >> 5)] >>> 0 & (1 << (0x57 & 31)), 1 << (0x57 & 31));
  f.dispatch('keydown', { code: 'F1', key: 'F1' });
  assert.ok(f.words[K.keys + (0x70 >> 5)] & (1 << (0x70 & 31)));
  f.dispatch('keyup', { code: 'KeyW', key: 'w' });
  assert.equal(f.words[K.keys + (0x57 >> 5)] & (1 << (0x57 & 31)), 0);
  f.target.dispatchEvent(new Event('blur'));
  assert.equal(f.words[K.active], 0);
  assert.equal(f.words[K.keys + (0x70 >> 5)], 0);
  f.input.dispose();
});

test('clicks count only for a press and release both made while the guest takes the cursor', () => {
  const f = fixture();
  f.dispatch('mousedown', { button: 0, clientX: 330, clientY: 260 });
  f.dispatch('mouseup', { button: 0 });
  assert.equal(f.words[K.clicks], 0, 'a click in play is fire, not a menu click');
  Atomics.store(f.words, K.flags, K.menuFlag);
  f.dispatch('mousedown', { button: 0, clientX: 330, clientY: 260 });
  assert.equal(f.words[K.pressing], 1);
  assert.deepEqual([f.words[K.cursorX], f.words[K.cursorY], f.words[K.cursorW], f.words[K.cursorH]], [320, 240, 640, 480]);
  f.dispatch('mouseup', { button: 0 });
  assert.equal(f.words[K.clicks], 1);
  f.dispatch('mousedown', { button: 0 });
  Atomics.store(f.words, K.flags, 0);
  f.dispatch('mouseup', { button: 0 });
  assert.equal(f.words[K.clicks], 1, 'the menu closed under the press');
  const right = f.dispatch('mousedown', { button: 2 });
  assert.ok(right.defaultPrevented);
  assert.ok(f.words[K.keys] & (1 << 2));
  f.input.dispose();
});

test('characters go into the ring only while a name is typed, Backspace as a backspace', () => {
  const f = fixture();
  f.dispatch('keydown', { code: 'KeyA', key: 'a' });
  assert.equal(f.words[K.charHead], 0);
  Atomics.store(f.words, K.flags, K.textFlag);
  for (let n = 0; n < 70; ++n) { f.dispatch('keydown', { code: 'KeyB', key: 'b', repeat: true }); }
  f.dispatch('keydown', { code: 'Backspace', key: 'Backspace' });
  assert.equal(f.words[K.charHead], 71);
  const unit = n => (f.words[K.chars + ((n % 64) >> 1)] >>> ((n & 1) * 16)) & 0xffff;
  assert.equal(unit(69), 'b'.charCodeAt(0));
  assert.equal(unit(70), 8);
  f.input.dispose();
});

test('mouse movement accumulates in thousandths while the pointer is locked, and losing the lock in play taps Start', () => {
  const f = fixture();
  f.dispatch('mousemove', { movementX: 3, movementY: -2, clientX: 0, clientY: 0 });
  assert.equal(f.words[K.mouseX], 0);
  f.document.pointerLockElement = f.canvas;
  f.dispatch('mousemove', { movementX: 3.5, movementY: -2, clientX: 0, clientY: 0 });
  assert.deepEqual([f.words[K.mouseX], f.words[K.mouseY]], [3500, -2000]);
  f.document.pointerLockElement = null;
  f.document.dispatchEvent(new Event('pointerlockchange'));
  assert.equal(f.words[K.startTaps], 1);
  f.document.pointerLockElement = f.canvas;
  f.document.dispatchEvent(new Event('pointerlockchange'));
  Atomics.store(f.words, K.flags, K.menuFlag);
  f.tick();
  assert.equal(f.document.pointerLockElement, null, 'a menu frees the cursor');
  f.document.dispatchEvent(new Event('pointerlockchange'));
  assert.equal(f.words[K.startTaps], 1, 'our own release is no pause');
  assert.equal(f.canvas.style.cursor, 'default');
  f.input.dispose();
});

test('the wheel over the frame counts notches and is kept from the page', () => {
  const f = fixture();
  const event = f.dispatch('wheel', { deltaY: -200, deltaMode: 0 });
  assert.ok(event.defaultPrevented);
  assert.equal(f.words[K.wheelUp], 2);
  f.dispatch('wheel', { deltaY: 3, deltaMode: 1 });
  assert.equal(f.words[K.wheelDown], 1);
  f.input.setActive(false);
  f.dispatch('wheel', { deltaY: -100, deltaMode: 0 });
  assert.equal(f.words[K.wheelUp], 2);
  f.input.dispose();
});
