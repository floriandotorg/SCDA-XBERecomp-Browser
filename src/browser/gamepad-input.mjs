import { MENU_INPUT } from './menu-input.mjs';

export const GAMEPAD_INPUT = Object.freeze({ words: 8, bytes: 32 });

const owners = new WeakMap();
const buttons = [MENU_INPUT.confirm, MENU_INPUT.back, MENU_INPUT.x, MENU_INPUT.y, MENU_INPUT.white, MENU_INPUT.black,
  MENU_INPUT.leftTrigger, MENU_INPUT.rightTrigger, MENU_INPUT.select, MENU_INPUT.start, MENU_INPUT.leftThumb, MENU_INPUT.rightThumb,
  MENU_INPUT.up, MENU_INPUT.down, MENU_INPUT.left, MENU_INPUT.right];
const axis = value => Number.isFinite(value) ? Math.round(Math.max(-1, Math.min(1, value)) * 32767) : 0;

export const createGamepadInput = ({ buffer, byteOffset, target = globalThis.window, visibilityTarget = globalThis.document,
  navigator = globalThis.navigator, requestFrame = globalThis.requestAnimationFrame?.bind(globalThis),
  cancelFrame = globalThis.cancelAnimationFrame?.bind(globalThis), signal } = {}) => {
  if (!(buffer instanceof SharedArrayBuffer) || buffer.growable) { throw new TypeError('Gamepad input requires fixed shared memory.'); }
  if (!Number.isSafeInteger(byteOffset) || byteOffset < 0 || byteOffset % 4 || byteOffset > buffer.byteLength - GAMEPAD_INPUT.bytes) {
    throw new RangeError('Gamepad input span must be aligned and in bounds.');
  }
  for (const events of [target, visibilityTarget]) {
    if (!events || typeof events.addEventListener !== 'function' || typeof events.removeEventListener !== 'function') {
      throw new TypeError('Gamepad input requires focus and visibility event targets.');
    }
  }
  if (typeof requestFrame !== 'function' || typeof cancelFrame !== 'function') { throw new TypeError('Gamepad input requires animation polling.'); }
  signal?.throwIfAborted();
  const spans = owners.get(buffer) ?? new Set();
  for (const start of spans) {
    if (start < byteOffset + GAMEPAD_INPUT.bytes && byteOffset < start + GAMEPAD_INPUT.bytes) {
      throw new Error('Gamepad input span already has an overlapping owner.');
    }
  }
  spans.add(byteOffset);
  owners.set(buffer, spans);
  const words = new Int32Array(buffer, byteOffset, GAMEPAD_INPUT.words);
  let disposed = false;
  let active = true;
  let focused = visibilityTarget.hasFocus?.() ?? true;
  let frame;
  const publish = (sample, force = false) => {
    if (!force && sample.every((value, n) => Atomics.load(words, n + 1) === value)) { return; }
    const sequence = Atomics.load(words, 0) & ~1;
    Atomics.store(words, 0, sequence + 1);
    for (let n = 0; n < sample.length; ++n) { Atomics.store(words, n + 1, sample[n]); }
    Atomics.store(words, 0, sequence + 2);
  };
  const neutral = () => publish([0, 0, 0, 0, 0, 0, 0]);
  const poll = () => {
    if (disposed) { return; }
    frame = undefined;
    let pad;
    if (active && focused && !visibilityTarget.hidden) {
      try { pad = Array.from(navigator?.getGamepads?.() ?? []).find(value => value?.connected && value.mapping === 'standard'); }
      catch {}
    }
    if (disposed) { return; }
    if (pad) {
      let mask = 0;
      for (let n = 0; n < buttons.length; ++n) {
        if (pad.buttons?.[n]?.pressed) { mask |= buttons[n]; }
      }
      publish([1, mask, axis(pad.axes?.[0]), axis(-pad.axes?.[1]), axis(pad.axes?.[2]), axis(-pad.axes?.[3]), 0]);
    } else { neutral(); }
    if (!disposed) { frame = requestFrame(poll); }
  };
  const blur = () => { focused = false; neutral(); };
  const focus = () => { focused = true; neutral(); };
  const visibility = () => { neutral(); };
  const dispose = () => {
    if (disposed) { return; }
    disposed = true;
    if (frame !== undefined) { cancelFrame(frame); }
    neutral();
    target.removeEventListener('blur', blur);
    target.removeEventListener('focus', focus);
    visibilityTarget.removeEventListener('visibilitychange', visibility);
    signal?.removeEventListener('abort', dispose);
    spans.delete(byteOffset);
  };
  publish([0, 0, 0, 0, 0, 0, 0], true);
  target.addEventListener('blur', blur);
  target.addEventListener('focus', focus);
  visibilityTarget.addEventListener('visibilitychange', visibility);
  signal?.addEventListener('abort', dispose, { once: true });
  frame = requestFrame(poll);
  return Object.freeze({ setActive: value => {
    if (disposed || active === Boolean(value)) { return; }
    active = Boolean(value);
    neutral();
  }, dispose });
};
