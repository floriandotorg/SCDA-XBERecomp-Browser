import { test } from 'node:test';
import assert from 'node:assert/strict';
import { bind, bindPad, keyBinding, label, padBinding, resolve, unbind } from '../../src/browser/input-bindings.mjs';

test('keys default to the table and overrides replace them, an empty override is unbound', () => {
  assert.deepEqual(keyBinding({}, 'interact'), ['Space', 'Enter']);
  assert.deepEqual(keyBinding({ interact: 'F' }, 'interact'), ['F']);
  assert.deepEqual(keyBinding({ interact: '' }, 'interact'), []);
  assert.deepEqual(keyBinding({ interact: 'Nonsense,G' }, 'interact'), ['G']);
  assert.deepEqual(resolve({}, {}).keys.scope, ['E', 'MMB']);
});

test('bind has two slots and a key the other slot has moves', () => {
  assert.deepEqual(bind({}, 'fire', 'G', 0), { fire: 'G' });
  assert.deepEqual(bind({}, 'fire', 'G', 1), { fire: 'LMB,G' });
  assert.deepEqual(bind({}, 'interact', 'Enter', 0), { interact: 'Enter' });
  assert.deepEqual(bind({ interact: 'Space,Enter' }, 'interact', 'Space', 1), { interact: 'Space' });
  assert.deepEqual(bind({}, 'interact', '', 1), { interact: 'Space' });
});

test('host keys refuse mouse buttons and the wheel, push to talk takes them, unknown keys are refused', () => {
  const keys = {};
  assert.equal(bind(keys, 'console', 'LMB', 0), keys);
  assert.equal(bind(keys, 'settings_panel', 'Wheel Up', 0), keys);
  assert.deepEqual(bind({}, 'push_to_talk', 'Mouse4', 0), { push_to_talk: 'Mouse4' });
  assert.equal(bind(keys, 'fire', 'Nonsense', 0), keys);
  assert.equal(bind(keys, 'nonsense', 'G', 0), keys);
});

test('the settings panel can never be left without a key', () => {
  const keys = {};
  assert.equal(unbind(keys, 'settings_panel'), keys);
  assert.equal(bind(keys, 'settings_panel', '', 0), keys);
  assert.deepEqual(bind(keys, 'settings_panel', 'F9', 1), { settings_panel: 'F10,F9' });
  assert.deepEqual(bind({ settings_panel: 'F10,F9' }, 'settings_panel', '', 0), { settings_panel: 'F9' });
  assert.deepEqual(unbind({}, 'fire'), { fire: '' });
});

test('pad buttons change places, events come off their button', () => {
  assert.equal(padBinding({}, 'jump'), 3);
  assert.equal(padBinding({}, 'quick_save'), -1);
  const swapped = bindPad({}, 'jump', 1);
  assert.deepEqual(swapped, { jump: 'B', crouch: 'Y' });
  assert.equal(padBinding(swapped, 'crouch'), 3);
  assert.deepEqual(bindPad(swapped, 'jump', 3), {});
  const saved = bindPad({}, 'quick_save', 13);
  assert.deepEqual(saved, { quick_save: 'BACK', opsat: '' });
  assert.deepEqual(bindPad(saved, 'quick_save', 13), { opsat: '' });
  assert.deepEqual(bindPad(saved, 'quick_save', -1), { opsat: '' });
  const pads = {};
  assert.equal(bindPad(pads, 'jump', -1), pads);
  assert.equal(bindPad(pads, 'jump', 99), pads);
});

test('resolve settles two actions saved on one button: the later takes its stock button if free, else none', () => {
  const { pads } = resolve({}, { jump: 'A', crouch: 'A', interact: 'Y' });
  assert.equal(pads.jump, 0);
  assert.equal(pads.crouch, 1);
  assert.equal(pads.interact, 3);
  const second = resolve({}, { jump: 'B', crouch: 'B' }).pads;
  assert.equal(second.jump, 1);
  assert.equal(second.crouch, -1);
  assert.equal(resolve({}, { fire: '' }).pads.fire, -1);
});

test('labels follow the keyboard layout where one is known', () => {
  const layout = new Map([['KeyQ', 'a'], ['KeyW', 'z'], ['Digit1', '&'], ['Comma', ';'], ['Semicolon', ',']]);
  assert.equal(label('Q', layout), 'A');
  assert.equal(label('W', layout), 'Z');
  assert.equal(label('1', layout), '&');
  assert.equal(label('Semicolon', layout), 'Comma');
  assert.equal(label('LMB', layout), 'LMB');
  assert.equal(label('Q'), 'Q');
});
