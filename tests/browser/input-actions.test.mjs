import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { ACTIONS, KEY_NAMES, PAD_ACTIONS, PAD_BUTTONS, keyNamed, vkOfCode } from '../../src/browser/input-actions.mjs';

const native = readFileSync('src/core/input.cpp', 'utf8');
const windowsKeys = { VK_LBUTTON: 0x01, VK_RBUTTON: 0x02, VK_MBUTTON: 0x04, VK_XBUTTON1: 0x05, VK_XBUTTON2: 0x06, kWheelUp: 0x0e,
  kWheelDown: 0x0f, VK_SPACE: 0x20, VK_RETURN: 0x0d, VK_ESCAPE: 0x1b, VK_BACK: 0x08, VK_TAB: 0x09, VK_SHIFT: 0x10, VK_CONTROL: 0x11,
  VK_MENU: 0x12, VK_CAPITAL: 0x14, VK_UP: 0x26, VK_DOWN: 0x28, VK_LEFT: 0x25, VK_RIGHT: 0x27, VK_INSERT: 0x2d, VK_DELETE: 0x2e,
  VK_HOME: 0x24, VK_END: 0x23, VK_PRIOR: 0x21, VK_NEXT: 0x22, VK_F1: 0x70, VK_F2: 0x71, VK_F4: 0x73, VK_F5: 0x74, VK_F6: 0x75,
  VK_F7: 0x76, VK_F8: 0x77, VK_F9: 0x78, VK_F10: 0x79, VK_F11: 0x7a, VK_F12: 0x7b, VK_ADD: 0x6b, VK_SUBTRACT: 0x6d,
  VK_MULTIPLY: 0x6a, VK_DIVIDE: 0x6f, VK_DECIMAL: 0x6e, VK_OEM_3: 0xc0, VK_OEM_MINUS: 0xbd, VK_OEM_PLUS: 0xbb, VK_OEM_4: 0xdb,
  VK_OEM_6: 0xdd, VK_OEM_5: 0xdc, VK_OEM_1: 0xba, VK_OEM_7: 0xde, VK_OEM_COMMA: 0xbc, VK_OEM_PERIOD: 0xbe, VK_OEM_2: 0xbf,
  ...Object.fromEntries(Array.from({ length: 10 }, (_, n) => [`VK_NUMPAD${n}`, 0x60 + n])) };
const vk = token => token.startsWith("'") ? token.charCodeAt(1) : windowsKeys[token];
const table = (name) => native.split(`${name}[] = {`)[1].split('\n};')[0];

test('every native key name is present with its virtual key code and no code maps to two keys', () => {
  const names = [...table('constexpr Name kNames').matchAll(/\{([\w']+), "([^"]+)"\}/g)].map(([, token, name]) => [name, vk(token)]);
  assert.equal(names.length, KEY_NAMES.length);
  for (const [name, code] of names) {
    assert.ok(code !== undefined, name);
    assert.equal(keyNamed(name)?.vk, code, name);
  }
  const codes = KEY_NAMES.flatMap(entry => entry.codes);
  assert.equal(new Set(codes).size, codes.length);
  assert.equal(vkOfCode('NumpadEnter'), 0x0d);
  assert.equal(vkOfCode('KeyQ'), 0x51);
  assert.equal(vkOfCode('F3'), 0x72);
  assert.equal(vkOfCode('Nonsense'), 0);
});

test('the story actions follow the native table, defaults resolve, and only browser reserved keys differ', () => {
  const rows = [...table('constexpr Target kStoryActions').matchAll(/\{\{"(\w+)", "([^"]+)", "([^"]*)"\}, ([^,}]+)(?:, (true|false))?/g)];
  assert.deepEqual(ACTIONS.map(entry => entry.id), rows.map(row => row[1]));
  for (const [, id, label, defaults, , playOnly] of rows) {
    const entry = ACTIONS.find(item => item.id === id);
    assert.equal(entry.label, label);
    assert.equal(entry.defaults, id === 'quick_inventory' ? 'Tab' : defaults, id);
    assert.equal(entry.playOnly, playOnly === 'true', id);
    for (const name of entry.defaults.split(',').filter(Boolean)) { assert.ok(keyNamed(name), `${id} ${name}`); }
  }
});

test('pad actions and buttons follow kStoryPadActions and kPadNames', () => {
  const rows = [...table('constexpr PadAction kStoryPadActions').matchAll(/\{"(\w+)", "([^"]+)", (-?\d+)/g)];
  assert.deepEqual(PAD_ACTIONS.map(entry => [entry.id, entry.label, entry.button]), rows.map(([, id, label, button]) => [id, label, Number(button)]));
  const names = [...native.split('kPadNames[kPadButtons] = {')[1].split('};')[0].matchAll(/"([^"]+)"/g)].map(match => match[1]);
  assert.deepEqual(PAD_BUTTONS.map(entry => entry.name), names.slice(0, 16));
  assert.deepEqual(new Set(PAD_BUTTONS.map(entry => entry.index)).size, 16);
});
