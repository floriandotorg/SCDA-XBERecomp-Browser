// Binding rules of src/core/input.cpp (Bind, Unbind, BindPad, LoadBindings) over the sparse override maps settings.mjs
// keeps: keys maps an action id to its key names, comma separated ('' unbound); padButtons maps a pad action id to a
// button name ('' none). Absent is the default. Every function returns a new map, or the same one when it refuses.
import { ACTIONS, PAD_ACTIONS, PAD_BUTTONS, actionNamed, isMouseKey, keyNamed, padActionNamed, padButtonNamed } from './input-actions.mjs';

const names = list => list.split(',').map(keyNamed).filter(Boolean).map(entry => entry.name);

export const keyBinding = (keys, id) => {
  const entry = actionNamed(id);
  if (!entry) { return []; }
  return names(Object.hasOwn(keys ?? {}, id) ? String(keys[id]) : entry.defaults);
};

const lockedOut = id => id === 'settings_panel';

export const bind = (keys, id, name, slot = 0) => {
  const entry = actionNamed(id);
  const key = name ? keyNamed(name) : undefined;
  if (!entry || (slot !== 0 && slot !== 1) || (name && !key)) { return keys; }
  if (entry.kind === 'host' && id !== 'push_to_talk' && isMouseKey(key)) { return keys; }
  const current = keyBinding(keys, id);
  const slots = [current[0] ?? '', current[1] ?? ''];
  if (key && slots[1 - slot] === key.name) { slots[1 - slot] = ''; }
  slots[slot] = key?.name ?? '';
  if (!slots[0] && !slots[1] && lockedOut(id)) { return keys; }
  return { ...keys, [id]: slots.filter(Boolean).join(',') };
};

export const unbind = (keys, id) => !actionNamed(id) || lockedOut(id) ? keys : { ...keys, [id]: '' };

const savedButton = (pads, entry) => {
  if (!Object.hasOwn(pads ?? {}, entry.id)) { return entry.button; }
  const value = String(pads[entry.id]);
  return value ? padButtonNamed(value)?.button ?? -1 : -1;
};

// LoadBindings: a hand edited map may give two actions one button; the later takes its stock button if free, else none
const resolvePads = pads => {
  const binds = new Map(), taken = new Set(), later = [];
  for (const entry of PAD_ACTIONS) {
    const button = savedButton(pads, entry);
    if (button >= 0 && taken.has(button)) { later.push(entry); continue; }
    binds.set(entry.id, button);
    if (button >= 0) { taken.add(button); }
  }
  for (const entry of later) {
    const free = entry.button >= 0 && !taken.has(entry.button);
    binds.set(entry.id, free ? entry.button : -1);
    if (free) { taken.add(entry.button); }
  }
  return binds;
};

export const padBinding = (pads, id) => resolvePads(pads).get(id) ?? -1;

export const resolve = (keys, pads) => ({
  keys: Object.fromEntries(ACTIONS.map(entry => [entry.id, keyBinding(keys, entry.id)])),
  pads: Object.fromEntries(resolvePads(pads)),
});

export const bindPad = (pads, id, button) => {
  const entry = padActionNamed(id);
  if (!entry || !Number.isInteger(button) || button < -1 || button >= PAD_BUTTONS.length || (button < 0 && !entry.event)) { return pads; }
  const binds = resolvePads(pads);
  const old = binds.get(id);
  const off = button < 0 || (old === button && entry.event);
  for (const [other, value] of binds) {
    if (button >= 0 && value === button) { binds.set(other, old); }
  }
  binds.set(id, off ? -1 : button);
  const saved = {};
  for (const action of PAD_ACTIONS) {
    const value = binds.get(action.id);
    if (value !== action.button) { saved[action.id] = value >= 0 ? PAD_BUTTONS[value].name : ''; }
  }
  return saved;
};

export const reset = () => ({});
export const resetPad = () => ({});

// the key as the player's keyboard labels it (navigator.keyboard.getLayoutMap()), the US name without a layout
export const label = (name, layout) => {
  const entry = keyNamed(name);
  if (!entry) { return ''; }
  const code = entry.codes[0];
  const positional = /^(Key|Digit)|^(Minus|Equal|BracketLeft|BracketRight|Backslash|Semicolon|Quote|Backquote|Comma|Period|Slash)$/.test(code ?? '');
  const live = positional ? layout?.get?.(code) : undefined;
  if (typeof live !== 'string' || live.length !== 1) { return entry.name; }
  if (live === ',') { return 'Comma'; }
  const upper = live.toUpperCase();
  return upper > ' ' && upper.codePointAt(0) < 0x7f || /\p{L}/u.test(upper) ? upper : entry.name;
};
