import { test } from 'node:test';
import assert from 'node:assert/strict';
import { KEYBOARD_INPUT as K } from '../../src/browser/keyboard-input.mjs';
import { PAD_INDEX, buttonCaption, createVirtualPad, stickVector, touchMode } from '../../src/browser/touch-controls.mjs';

test('a stick drag is a direction scaled by its reach, clamped to the rim, y down as a standard pad', () => {
  assert.deepEqual(stickVector(0, 0, 50), [0, 0]);
  assert.deepEqual(stickVector(25, 0, 50), [0.5, 0]);
  assert.deepEqual(stickVector(0, -200, 50), [0, -1]);
  const [x, y] = stickVector(30, 40, 50);
  assert.ok(Math.abs(x - 0.6) < 1e-9 && Math.abs(y - 0.8) < 1e-9);
  assert.deepEqual(stickVector(2, 0, 50), [0, 0], 'a resting thumb inside the dead zone is still');
});

test('the guest flags choose what the overlay shows', () => {
  assert.equal(touchMode(0), 'play');
  assert.equal(touchMode(K.menuFlag), 'menu');
  assert.equal(touchMode(K.pointerFlag), 'menu');
  assert.equal(touchMode(K.menuFlag | K.mapDragFlag), 'play');
  assert.equal(touchMode(K.menuFlag | K.textFlag), 'text');
  assert.equal(touchMode(K.movieFlag), 'movie');
  assert.equal(touchMode(K.movieFlag | K.menuFlag), 'menu');
});

test('captions follow the pad bindings', () => {
  assert.equal(buttonCaption({}, PAD_INDEX.a), 'Action');
  assert.equal(buttonCaption({}, PAD_INDEX.y), 'Jump');
  assert.equal(buttonCaption({}, PAD_INDEX.rt), 'Fire');
  assert.equal(buttonCaption({}, PAD_INDEX.start), 'Pause');
  assert.equal(buttonCaption({ jump: 'A' }, PAD_INDEX.a), 'Jump');
  assert.equal(buttonCaption({ jump: 'A' }, PAD_INDEX.y), '', 'a button nothing is bound to has no caption');
});

test('the virtual pad is a standard pad with held buttons and both sticks', () => {
  const pad = createVirtualPad();
  assert.equal(pad.sample(), undefined, 'nothing until it is connected');
  pad.setConnected(true);
  assert.deepEqual(pad.sample().axes, [0, 0, 0, 0]);
  pad.press(PAD_INDEX.a, true);
  pad.press(PAD_INDEX.rt, true);
  pad.press(PAD_INDEX.rt, true);
  pad.setStick(0, 0.5, -1);
  const sample = pad.sample();
  assert.equal(sample.mapping, 'standard');
  assert.equal(sample.connected, true);
  assert.equal(sample.buttons.length, 17);
  assert.deepEqual(sample.buttons.map((button, n) => button.pressed ? n : -1).filter(n => n >= 0), [PAD_INDEX.a, PAD_INDEX.rt]);
  assert.equal(sample.buttons[PAD_INDEX.rt].value, 1);
  assert.deepEqual(sample.axes, [0.5, -1, 0, 0]);
  pad.press(PAD_INDEX.rt, false);
  assert.equal(pad.sample().buttons[PAD_INDEX.rt].pressed, true, 'two fingers on one button hold it until both lift');
  pad.press(PAD_INDEX.rt, false);
  assert.equal(pad.sample().buttons[PAD_INDEX.rt].pressed, false);
  pad.release();
  assert.ok(pad.sample().buttons.every(button => !button.pressed));
  assert.deepEqual(pad.sample().axes, [0, 0, 0, 0]);
  pad.setConnected(false);
  assert.equal(pad.sample(), undefined);
});
