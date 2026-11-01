export const MENU_INPUT = Object.freeze({
  words: 2, bytes: 8,
  up: 1, down: 2, left: 4, right: 8,
  forward: 16, backward: 32, strafeLeft: 64, strafeRight: 128,
  start: 256, confirm: 512, back: 1024,
});

const bindings = new Map([
  ['ArrowUp', MENU_INPUT.up], ['ArrowDown', MENU_INPUT.down],
  ['ArrowLeft', MENU_INPUT.left], ['ArrowRight', MENU_INPUT.right],
  ['KeyW', MENU_INPUT.forward], ['KeyS', MENU_INPUT.backward],
  ['KeyA', MENU_INPUT.strafeLeft], ['KeyD', MENU_INPUT.strafeRight],
  ['Enter', MENU_INPUT.start], ['NumpadEnter', MENU_INPUT.start],
  ['Space', MENU_INPUT.confirm], ['KeyZ', MENU_INPUT.confirm],
  ['Escape', MENU_INPUT.back], ['Backspace', MENU_INPUT.back],
]);
const producers = new WeakMap();

export const createMenuInput = ({ buffer, byteOffset, target = globalThis.window,
  focusTarget = globalThis.window, visibilityTarget = globalThis.document, signal, active = true } = {}) => {
  if (!(buffer instanceof SharedArrayBuffer) || buffer.growable) {
    throw new TypeError('Menu input requires fixed shared memory.');
  }
  if (!Number.isSafeInteger(byteOffset) || byteOffset < 0 || byteOffset % 4 || byteOffset > buffer.byteLength - MENU_INPUT.bytes) {
    throw new RangeError('Menu input span offset must be aligned and in bounds.');
  }
  for (const events of [target, focusTarget, visibilityTarget]) {
    if (!events || typeof events.addEventListener !== 'function' || typeof events.removeEventListener !== 'function') {
      throw new TypeError('Menu input requires keyboard, focus and visibility event targets.');
    }
  }
  signal?.throwIfAborted();
  const spans = producers.get(buffer) ?? new Set();
  for (const start of spans) {
    if (start < byteOffset + MENU_INPUT.bytes && byteOffset < start + MENU_INPUT.bytes) {
      throw new Error('Menu input span already has an overlapping producer owner.');
    }
  }
  spans.add(byteOffset);
  producers.set(buffer, spans);
  const words = new Uint32Array(buffer, byteOffset, MENU_INPUT.words);
  Atomics.store(words, 0, 0);
  Atomics.store(words, 1, 0);
  const held = new Set();
  const suppressed = new Set();
  let enabled = Boolean(active);
  let focused = typeof visibilityTarget.hasFocus === 'function' ? visibilityTarget.hasFocus() : true;
  let disposed = false;
  const available = () => enabled && focused && !visibilityTarget.hidden && !disposed;
  const publish = () => {
    let mask = 0;
    if (available()) {
      for (const code of held) { mask |= bindings.get(code) ?? 0; }
    }
    if (Atomics.exchange(words, 0, mask) !== mask) { Atomics.add(words, 1, 1); }
  };
  const clear = () => {
    for (const code of held) { suppressed.add(code); }
    held.clear();
    publish();
  };
  const keydown = (event) => {
    if (!bindings.has(event.code)) { return; }
    if (!available()) { suppressed.add(event.code); return; }
    const element = event.composedPath?.()[0] ?? event.target;
    if (element?.closest?.('input,textarea,select,button,a,[contenteditable]:not([contenteditable="false"])')) {
      clear();
      return;
    }
    if (event.ctrlKey || event.altKey || event.metaKey) { return; }
    event.preventDefault();
    if (event.repeat || suppressed.has(event.code)) { return; }
    held.add(event.code);
    publish();
  };
  const keyup = (event) => {
    if (!bindings.has(event.code)) { return; }
    const consumed = held.delete(event.code);
    suppressed.delete(event.code);
    if (consumed) { event.preventDefault(); }
    publish();
  };
  const blur = () => { focused = false; clear(); };
  const focus = () => { focused = true; clear(); };
  const visibility = () => { clear(); };
  const dispose = () => {
    if (disposed) { return; }
    disposed = true;
    clear();
    target.removeEventListener('keydown', keydown);
    target.removeEventListener('keyup', keyup);
    focusTarget.removeEventListener('blur', blur);
    focusTarget.removeEventListener('focus', focus);
    visibilityTarget.removeEventListener('visibilitychange', visibility);
    signal?.removeEventListener('abort', dispose);
    spans.delete(byteOffset);
  };
  target.addEventListener('keydown', keydown);
  target.addEventListener('keyup', keyup);
  focusTarget.addEventListener('blur', blur);
  focusTarget.addEventListener('focus', focus);
  visibilityTarget.addEventListener('visibilitychange', visibility);
  signal?.addEventListener('abort', dispose, { once: true });
  return Object.freeze({
    setActive: (value) => {
      if (disposed || enabled === Boolean(value)) { return; }
      enabled = Boolean(value);
      clear();
    },
    dispose,
  });
};
