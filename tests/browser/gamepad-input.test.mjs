import test from 'node:test';
import assert from 'node:assert/strict';
import { createGamepadInput, GAMEPAD_INPUT } from '../../src/browser/gamepad-input.mjs';
import { MENU_INPUT } from '../../src/browser/menu-input.mjs';

const fixture = () => {
  const buffer = new SharedArrayBuffer(64);
  const whole = new Int32Array(buffer);
  whole.fill(0x12345678);
  const words = new Int32Array(buffer, 8, 8);
  const target = new EventTarget();
  const document = new EventTarget();
  document.hidden = false;
  document.hasFocus = () => true;
  const frames = new Map();
  let sequence = 0;
  let pads = [];
  const options = { buffer, byteOffset: 8, target, visibilityTarget: document,
    navigator: { getGamepads: () => pads },
    requestFrame: callback => { frames.set(++sequence, callback); return sequence; },
    cancelFrame: id => { frames.delete(id); } };
  const tick = () => {
    const pending = [...frames];
    frames.clear();
    for (const [, callback] of pending) { callback(); }
  };
  const pad = (axes = [0, 0, 0, 0], pressed = []) => ({ connected: true, mapping: 'standard', axes,
    buttons: Array.from({ length: 17 }, (_, n) => ({ pressed: pressed.includes(n), value: pressed.includes(n) ? 1 : 0 })) });
  return { buffer, whole, words, target, document, frames, options, tick, pad, setPads: value => { pads = value; } };
};

test('standard gamepad preserves signed analog sticks and original controller button meanings', () => {
  const value = fixture();
  const producer = createGamepadInput(value.options);
  assert.equal(GAMEPAD_INPUT.words, 8);
  assert.equal(GAMEPAD_INPUT.bytes, 32);
  value.setPads([null, value.pad([0.5, -0.25, -1, 1], [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15])]);
  value.tick();
  assert.equal(value.words[0] % 2, 0);
  assert.equal(value.words[1], 1);
  assert.equal(value.words[2], MENU_INPUT.confirm | MENU_INPUT.back | MENU_INPUT.x | MENU_INPUT.y | MENU_INPUT.white | MENU_INPUT.black |
    MENU_INPUT.leftTrigger | MENU_INPUT.rightTrigger | MENU_INPUT.select | MENU_INPUT.start | MENU_INPUT.leftThumb | MENU_INPUT.rightThumb | MENU_INPUT.up | MENU_INPUT.right);
  assert.deepEqual([...value.words.slice(3, 7)], [16384, 8192, -32767, -32767]);
  assert.equal(value.words[7], 0);
  assert.equal(value.whole[0], 0x12345678);
  assert.equal(value.whole[10], 0x12345678);
  const sequence = value.words[0];
  value.tick();
  assert.equal(value.words[0], sequence);
  producer.dispose();
});

test('disconnect, blur, hidden pages and explicit inactivity release held buttons and axes', () => {
  for (const reason of ['disconnect', 'blur', 'hidden', 'inactive']) {
    const value = fixture();
    const producer = createGamepadInput(value.options);
    value.setPads([value.pad([1, -1, 1, -1], [0, 9])]);
    value.tick();
    assert.notEqual(value.words[2], 0);
    if (reason === 'disconnect') { value.setPads([]); value.tick(); }
    if (reason === 'blur') { value.target.dispatchEvent(new Event('blur')); }
    if (reason === 'hidden') { value.document.hidden = true; value.document.dispatchEvent(new Event('visibilitychange')); }
    if (reason === 'inactive') { producer.setActive(false); }
    assert.deepEqual([...value.words.slice(1)], [0, 0, 0, 0, 0, 0, 0]);
    value.tick();
    assert.deepEqual([...value.words.slice(1)], [0, 0, 0, 0, 0, 0, 0]);
    producer.dispose();
  }
});

test('every standard face, shoulder, menu, stick and direction button maps independently', () => {
  const value = fixture();
  const producer = createGamepadInput(value.options);
  const masks = [MENU_INPUT.confirm, MENU_INPUT.back, MENU_INPUT.x, MENU_INPUT.y, MENU_INPUT.white, MENU_INPUT.black,
    MENU_INPUT.leftTrigger, MENU_INPUT.rightTrigger, MENU_INPUT.select, MENU_INPUT.start, MENU_INPUT.leftThumb, MENU_INPUT.rightThumb,
    MENU_INPUT.up, MENU_INPUT.down, MENU_INPUT.left, MENU_INPUT.right, 0];
  for (let n = 0; n < masks.length; ++n) {
    value.setPads([value.pad([0, 0, 0, 0], [n])]);
    value.tick();
    assert.equal(value.words[2], masks[n]);
  }
  producer.dispose();
});

test('focus and reconnect use the current physical state without stale input', () => {
  const value = fixture();
  const producer = createGamepadInput(value.options);
  value.setPads([value.pad([1, 0, 0, 0], [0])]);
  value.tick();
  value.target.dispatchEvent(new Event('blur'));
  value.setPads([value.pad([-0.5, 0, 0, 0], [1])]);
  value.target.dispatchEvent(new Event('focus'));
  value.tick();
  assert.equal(value.words[2], MENU_INPUT.back);
  assert.equal(value.words[3], -16383);
  producer.setActive(false);
  producer.setActive(true);
  value.setPads([value.pad()]);
  value.tick();
  assert.deepEqual([...value.words.slice(2)], [0, 0, 0, 0, 0, 0]);
  producer.dispose();
});

test('invalid axes and unsupported mappings cannot publish malformed guest input', () => {
  const value = fixture();
  const producer = createGamepadInput(value.options);
  value.setPads([{ ...value.pad(), mapping: '' }]);
  value.tick();
  assert.equal(value.words[1], 0);
  value.setPads([value.pad([Infinity, NaN, 3, -3])]);
  value.tick();
  assert.deepEqual([...value.words.slice(3, 7)], [0, 0, 32767, 32767]);
  value.setPads([{ ...value.pad(), connected: false }]);
  value.tick();
  assert.deepEqual([...value.words.slice(1)], [0, 0, 0, 0, 0, 0, 0]);
  producer.dispose();
});

test('retirement cancels polling, releases input and cannot write a replaced runtime', () => {
  const value = fixture();
  const controller = new AbortController();
  const producer = createGamepadInput({ ...value.options, signal: controller.signal });
  value.setPads([value.pad([1, 1, 1, 1], [0])]);
  value.tick();
  const late = [...value.frames.values()][0];
  controller.abort();
  assert.equal(value.frames.size, 0);
  assert.deepEqual([...value.words.slice(1)], [0, 0, 0, 0, 0, 0, 0]);
  const replacement = createGamepadInput(value.options);
  value.setPads([value.pad([0.5, 0, 0, 0], [2])]);
  value.tick();
  const snapshot = [...value.words];
  late();
  producer.dispose();
  assert.deepEqual([...value.words], snapshot);
  replacement.dispose();
});

test('shared spans reject overlap, invalid bounds and already retired owners', () => {
  const value = fixture();
  for (const byteOffset of [-4, 1, 36, 64]) {
    assert.throws(() => createGamepadInput({ ...value.options, byteOffset }));
  }
  const controller = new AbortController();
  controller.abort();
  assert.throws(() => createGamepadInput({ ...value.options, signal: controller.signal }), /abort/i);
  const producer = createGamepadInput(value.options);
  assert.throws(() => createGamepadInput(value.options), /owner|overlap/i);
  assert.throws(() => createGamepadInput({ ...value.options, byteOffset: 12 }), /owner|overlap/i);
  producer.dispose();
  createGamepadInput(value.options).dispose();
});

test('retirement inside the browser poll cannot publish into the retired owner', () => {
  const value = fixture();
  const controller = new AbortController();
  value.options.navigator.getGamepads = () => {
    controller.abort();
    return [value.pad([1, 1, 1, 1], [0])];
  };
  createGamepadInput({ ...value.options, signal: controller.signal });
  value.tick();
  assert.deepEqual([...value.words.slice(1)], [0, 0, 0, 0, 0, 0, 0]);
  assert.equal(value.frames.size, 0);
});
