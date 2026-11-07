import test from 'node:test';
import assert from 'node:assert/strict';
import { createGamepadInput, GAMEPAD_INPUT } from '../../src/browser/gamepad-input.mjs';
import { MENU_INPUT } from '../../src/browser/menu-input.mjs';

const fixture = () => {
  const buffer = new SharedArrayBuffer(8 + GAMEPAD_INPUT.bytes + 8);
  const whole = new Int32Array(buffer);
  whole.fill(0x12345678);
  const words = new Int32Array(buffer, 8, GAMEPAD_INPUT.words);
  words.fill(0);
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
  return { buffer, whole, words, floats: new Float32Array(buffer, 8, GAMEPAD_INPUT.words), target, document, frames, options, tick, pad,
    setPads: value => { pads = value; } };
};

test('standard gamepad preserves signed analog sticks and original controller button meanings', () => {
  const value = fixture();
  const producer = createGamepadInput(value.options);
  assert.equal(GAMEPAD_INPUT.words, 144);
  assert.equal(GAMEPAD_INPUT.bytes, 576);
  value.setPads([null, value.pad([0.5, -0.25, -1, 1], [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 15])]);
  value.tick();
  assert.equal(value.words[0] % 2, 0);
  assert.equal(value.words[1], 1);
  assert.equal(value.words[2], MENU_INPUT.confirm | MENU_INPUT.back | MENU_INPUT.x | MENU_INPUT.y | MENU_INPUT.white | MENU_INPUT.black |
    MENU_INPUT.leftTrigger | MENU_INPUT.rightTrigger | MENU_INPUT.select | MENU_INPUT.start | MENU_INPUT.leftThumb | MENU_INPUT.rightThumb | MENU_INPUT.up | MENU_INPUT.right);
  assert.deepEqual([...value.words.slice(3, 7)], [16384, 8192, -32767, -32767]);
  assert.equal(value.words[7], (0xffff & ~(1 << 9) & ~(1 << 10)) | (255 << 16) | (255 << 24), 'physical buttons in kPadNames order');
  assert.equal(value.whole[0], 0x12345678);
  assert.equal(value.whole[2 + GAMEPAD_INPUT.words], 0x12345678);
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
    assert.deepEqual([...value.words.slice(1, 8)], [0, 0, 0, 0, 0, 0, 0]);
    value.tick();
    assert.deepEqual([...value.words.slice(1, 8)], [0, 0, 0, 0, 0, 0, 0]);
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
  assert.deepEqual([...value.words.slice(2, 8)], [0, 0, 0, 0, 0, 0]);
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
  assert.deepEqual([...value.words.slice(1, 8)], [0, 0, 0, 0, 0, 0, 0]);
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
  assert.deepEqual([...value.words.slice(1, 8)], [0, 0, 0, 0, 0, 0, 0]);
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
  assert.deepEqual([...value.words.slice(1, 8)], [0, 0, 0, 0, 0, 0, 0]);
  assert.equal(value.frames.size, 0);
});

test('an on-screen pad publishes alone and merges with a physical pad', () => {
  const value = fixture();
  let virtual = [];
  const producer = createGamepadInput({ ...value.options, virtualPads: () => virtual });
  virtual = [value.pad([0.5, -1, 0, 0], [0])];
  value.tick();
  assert.equal(value.words[1], 2, 'a touch screen\'s pad alone is connected as 2: the guest names its buttons');
  assert.equal(value.words[2], MENU_INPUT.confirm);
  assert.deepEqual([...value.words.slice(3, 5)], [16384, 32767]);
  value.setPads([value.pad([-0.25, 0, 0, 0.75], [1, 7])]);
  value.tick();
  assert.equal(value.words[1], 1);
  assert.equal(value.words[2], MENU_INPUT.confirm | MENU_INPUT.back | MENU_INPUT.rightTrigger);
  assert.deepEqual([...value.words.slice(3, 7)], [16384, 32767, 0, -24575], 'the larger deflection of each axis wins');
  assert.equal(value.words[7], 1 | 2 | (1 << 7) | (255 << 24));
  virtual = [];
  value.setPads([]);
  value.tick();
  assert.deepEqual([...value.words.slice(1, 8)], [0, 0, 0, 0, 0, 0, 0]);
  producer.setActive(false);
  virtual = [value.pad([1, 0, 0, 0], [0])];
  value.tick();
  assert.deepEqual([...value.words.slice(1, 8)], [0, 0, 0, 0, 0, 0, 0], 'an inactive producer ignores the on-screen pad too');
  producer.dispose();
});

const motion = (fields, timeStamp) => {
  const event = Object.assign(new Event('devicemotion'), fields);
  Object.defineProperty(event, 'timeStamp', { value: timeStamp });
  return event;
};

test('the guest\'s rumble plays on the physical pad at the motors\' strength and stops with it', () => {
  const value = fixture();
  const effects = [];
  let clock = 0;
  const hardware = { ...value.pad(), vibrationActuator: { playEffect: (type, options) => { effects.push([type, options]); return Promise.resolve(); },
    reset: () => { effects.push(['reset']); return Promise.resolve(); } } };
  const producer = createGamepadInput({ ...value.options, now: () => clock });
  value.setPads([hardware]);
  Atomics.store(value.words, 8, 65535 | 32768 << 16);
  value.tick();
  assert.equal(effects.length, 1);
  assert.equal(effects[0][0], 'dual-rumble');
  assert.equal(effects[0][1].strongMagnitude, 1);
  assert.ok(Math.abs(effects[0][1].weakMagnitude - 0.5) < 0.001);
  clock = 50;
  value.tick();
  assert.equal(effects.length, 1, 'a running effect is not restarted every frame');
  clock = 150;
  value.tick();
  assert.equal(effects.length, 2, 'a held rumble is renewed before its effect ends');
  Atomics.store(value.words, 8, 0);
  value.tick();
  assert.deepEqual(effects.at(-1), ['reset']);
  Atomics.store(value.words, 8, 1000);
  value.tick();
  value.target.dispatchEvent(new Event('blur'));
  assert.deepEqual(effects.at(-1), ['reset'], 'a page in the background stops the motors');
  producer.dispose();
});

test('motion readings go into the ring in the picture\'s axes while gyro aiming is on', () => {
  const value = fixture();
  let aiming = false;
  let angle = 0;
  const producer = createGamepadInput({ ...value.options, motion: () => aiming, motionSensor: true, orientation: () => angle });
  assert.equal(value.words[10], 1, 'the page says it can read a motion sensor');
  const reading = motion({ rotationRate: { alpha: 30, beta: 10, gamma: 20 }, accelerationIncludingGravity: { x: 1, y: 9, z: 2 } }, 1500.5);
  value.target.dispatchEvent(reading);
  value.tick();
  assert.equal(value.words[9], 0, 'readings wait for gyro aiming');
  aiming = true;
  value.tick();
  value.target.dispatchEvent(reading);
  assert.equal(value.words[9], 1);
  assert.equal(value.words[16] >>> 0, 1500500);
  assert.deepEqual([...value.floats.slice(17, 23)], [10, 20, 30, 1, 9, 2]);
  assert.equal(value.words[23], 1);
  angle = 90;
  value.target.dispatchEvent(motion({ rotationRate: { alpha: 30, beta: 10, gamma: 20 }, accelerationIncludingGravity: null }, 1510));
  assert.equal(value.words[9], 2);
  const turned = [...value.floats.slice(25, 28)].map(n => Math.round(n * 1000) / 1000);
  assert.deepEqual(turned, [-20, 10, 30], 'landscape: the picture\'s right is the device\'s down, its up the device\'s right');
  assert.equal(value.words[31], 0, 'no accelerometer');
  value.target.dispatchEvent(motion({ rotationRate: { alpha: NaN, beta: 0, gamma: 0 } }, 1520));
  assert.equal(value.words[9], 2, 'an unreadable reading is dropped');
  aiming = false;
  value.tick();
  value.target.dispatchEvent(reading);
  assert.equal(value.words[9], 2);
  producer.dispose();
});
