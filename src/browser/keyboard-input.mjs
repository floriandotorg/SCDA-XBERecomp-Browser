// The page's raw keyboard and mouse for the guest's input core (src/browser/keyboard-input.h mirrors the layout):
// held keys by Windows virtual key code, wheel notches, counted clicks, the cursor over the frame, mouse movement while
// the pointer is locked, typed characters and an Escape tap. The guest writes back what takes the cursor (flags).
import { keyOfButton, vkOfCode } from './input-actions.mjs';

export const KEYBOARD_INPUT = Object.freeze({
  words: 64, change: 0, keys: 1, wheelUp: 9, wheelDown: 10, pressing: 11, clicks: 12, cursorX: 13, cursorY: 14,
  cursorW: 15, cursorH: 16, active: 17, mouseX: 18, mouseY: 19, startTaps: 20, charHead: 21, chars: 22, charUnits: 64,
  flags: 54, hostPanel: 55, hostConsole: 56,
  menuFlag: 1, pointerFlag: 2, textFlag: 4, mapDragFlag: 8, movieFlag: 16, captureFlag: 32,
});
const K = KEYBOARD_INPUT;
const PIXELS_PER_NOTCH = 100, LINES_PER_NOTCH = 3;

// wheel deltas into whole notches, up positive: pixels 100 a notch, lines 3, a page one
export const createWheelAccumulator = () => {
  let pending = 0;
  return (deltaY, deltaMode = 0) => {
    if (!Number.isFinite(deltaY) || deltaY === 0) { return 0; }
    const units = deltaMode === 1 ? LINES_PER_NOTCH : deltaMode === 2 ? 1 : PIXELS_PER_NOTCH;
    if (Math.sign(pending) && Math.sign(pending) !== Math.sign(-deltaY)) { pending = 0; }
    pending += -deltaY / units;
    const notches = Math.trunc(pending) || 0;
    pending -= notches;
    return notches;
  };
};

const owners = new WeakMap();
const hostControl = element => element?.closest?.('input,textarea,select,button,a,summary,[contenteditable]:not([contenteditable="false"])');

export const createKeyboardInput = ({ buffer, byteOffset, target = globalThis.window, document = globalThis.document,
  canvas = () => document?.querySelector?.('#game'), requestFrame = globalThis.requestAnimationFrame?.bind(globalThis),
  cancelFrame = globalThis.cancelAnimationFrame?.bind(globalThis), signal, active = true } = {}) => {
  if (!(buffer instanceof SharedArrayBuffer) || buffer.growable) { throw new TypeError('Keyboard input requires fixed shared memory.'); }
  if (!Number.isSafeInteger(byteOffset) || byteOffset <= 0 || byteOffset % 4 || byteOffset > buffer.byteLength - K.words * 4) {
    throw new RangeError('Keyboard input span must be aligned and in bounds.');
  }
  if (!target?.addEventListener || !document?.addEventListener || typeof canvas !== 'function') {
    throw new TypeError('Keyboard input requires its event targets and canvas.');
  }
  signal?.throwIfAborted();
  const spans = owners.get(buffer) ?? new Set();
  for (const start of spans) {
    if (start < byteOffset + K.words * 4 && byteOffset < start + K.words * 4) { throw new Error('Keyboard input span already has a producer.'); }
  }
  spans.add(byteOffset);
  owners.set(buffer, spans);
  const words = new Int32Array(buffer, byteOffset, K.words);
  const wheel = createWheelAccumulator();
  let enabled = Boolean(active), focused = document.hasFocus?.() ?? true, disposed = false, released = false, frame;
  let lastFlags = 0;
  const flags = () => Atomics.load(words, K.flags) >>> 0;
  const pointing = () => (flags() & (K.menuFlag | K.pointerFlag)) !== 0;
  const available = () => enabled && focused && !document.hidden && !disposed;
  const locked = () => document.pointerLockElement && document.pointerLockElement === canvas();
  const bump = () => { Atomics.add(words, K.change, 1); };
  const setKey = (vk, down) => {
    if (!(vk > 0 && vk < 256)) { return false; }
    const word = K.keys + (vk >> 5), bit = 1 << (vk & 31);
    const before = Atomics.load(words, word);
    if (Boolean(before & bit) === down) { return false; }
    if (down) { Atomics.or(words, word, bit); } else { Atomics.and(words, word, ~bit); }
    bump();
    return true;
  };
  const clear = () => {
    for (let n = 0; n < 8; ++n) { Atomics.store(words, K.keys + n, 0); }
    Atomics.store(words, K.pressing, 0);
    Atomics.store(words, K.mouseX, 0);
    Atomics.store(words, K.mouseY, 0);
    bump();
  };
  const publishActive = () => {
    const now = available() ? 1 : 0;
    if (Atomics.exchange(words, K.active, now) !== now) { if (!now) { clear(); } else { bump(); } }
  };
  const accumulate = (field, value) => {
    const delta = Math.round(Math.max(-1e6, Math.min(1e6, value)) * 1000);
    let previous = Atomics.load(words, field);
    for (;;) {
      const next = Math.max(-1e9, Math.min(1e9, previous + delta));
      const observed = Atomics.compareExchange(words, field, previous, next);
      if (observed === previous) { return; }
      previous = observed;
    }
  };
  const typed = character => {
    for (const unit of character.split('').map(c => c.charCodeAt(0))) {
      const head = Atomics.load(words, K.charHead) >>> 0;
      const slot = head % K.charUnits, word = K.chars + (slot >> 1), shift = (slot & 1) * 16;
      Atomics.store(words, word, (Atomics.load(words, word) & ~(0xffff << shift)) | (unit << shift));
      Atomics.store(words, K.charHead, (head + 1) | 0);
    }
  };
  const cursor = event => {
    const element = canvas();
    const rect = element?.getBoundingClientRect?.();
    if (!rect || rect.width <= 0 || rect.height <= 0 || !Number.isFinite(event.clientX)) { return; }
    Atomics.store(words, K.cursorX, Math.round(event.clientX - rect.left));
    Atomics.store(words, K.cursorY, Math.round(event.clientY - rect.top));
    Atomics.store(words, K.cursorH, Math.round(rect.height));
    Atomics.store(words, K.cursorW, Math.round(rect.width));
  };
  const keydown = event => {
    focused = document.hasFocus?.() ?? focused;
    if (!available()) { return; }
    if (hostControl(event.composedPath?.()[0] ?? event.target)) { return; }
    const vk = vkOfCode(event.code);
    if (!vk || event.metaKey) { return; }
    if (event.code === 'Escape' && locked()) { return; }
    event.preventDefault();
    if (flags() & K.textFlag) {
      if (event.key === 'Backspace') { typed('\b'); } else if (event.key?.length === 1 && !event.ctrlKey && !event.altKey) { typed(event.key); }
    }
    if (!event.repeat) { setKey(vk, true); }
  };
  const keyup = event => {
    const vk = vkOfCode(event.code);
    if (vk && setKey(vk, false)) { event.preventDefault(); }
  };
  const mousedown = event => {
    if (!available() || event.target !== canvas()) { return; }
    const key = keyOfButton(event.button);
    if (!key) { return; }
    event.preventDefault();
    cursor(event);
    if (event.button === 0) { Atomics.store(words, K.pressing, pointing() ? 1 : 0); }
    setKey(key.vk, true);
  };
  const mouseup = event => {
    const key = keyOfButton(event.button);
    if (!key) { return; }
    if (event.button === 0) {
      if (Atomics.exchange(words, K.pressing, 0) && available() && pointing()) { Atomics.add(words, K.clicks, 1); }
    }
    setKey(key.vk, false);
  };
  const mousemove = event => {
    if (!available()) { return; }
    cursor(event);
    if ((locked() || (flags() & K.mapDragFlag)) && Number.isFinite(event.movementX) && Number.isFinite(event.movementY)) {
      accumulate(K.mouseX, event.movementX);
      accumulate(K.mouseY, event.movementY);
    }
    bump();
  };
  const wheelEvent = event => {
    if (!available() || (event.target !== canvas() && !locked())) { return; }
    event.preventDefault();
    const notches = wheel(event.deltaY, event.deltaMode);
    if (notches > 0) { Atomics.add(words, K.wheelUp, notches); }
    if (notches < 0) { Atomics.add(words, K.wheelDown, -notches); }
    if (notches) { bump(); }
  };
  const suppress = event => { if (event.target === canvas()) { event.preventDefault(); } };
  const lockChange = () => {
    if (locked()) { released = false; return; }
    // the browser lets go of the lock on Escape itself: in play that is the pause key
    if (!released && available() && !pointing()) { Atomics.add(words, K.startTaps, 1); }
  };
  const lock = async () => {
    const element = canvas();
    if (!element?.requestPointerLock || locked()) { return; }
    try { await element.requestPointerLock({ unadjustedMovement: true }); }
    catch {
      try { await element.requestPointerLock(); } catch {}
    }
  };
  // the guest's flags: a menu or pointer frees the cursor, play takes it again where the browser allows that
  const watch = () => {
    frame = undefined;
    if (disposed) { return; }
    const now = flags();
    const free = (now & (K.menuFlag | K.pointerFlag)) && !(now & K.mapDragFlag);
    if (free && locked()) {
      released = true;
      document.exitPointerLock?.();
    } else if (!free && (lastFlags & (K.menuFlag | K.pointerFlag)) && released && available()) {
      void lock();
    }
    const element = canvas();
    if (element?.style) { element.style.cursor = free && !(now & K.movieFlag) ? 'default' : (locked() ? 'none' : ''); }
    lastFlags = now;
    frame = requestFrame?.(watch);
  };
  const blur = () => { focused = false; publishActive(); };
  const focus = () => { focused = true; publishActive(); };
  const visibility = () => { publishActive(); };
  const listeners = [[target, 'keydown', keydown], [target, 'keyup', keyup], [target, 'mousedown', mousedown], [target, 'mouseup', mouseup],
    [target, 'mousemove', mousemove], [target, 'blur', blur], [target, 'focus', focus], [document, 'visibilitychange', visibility],
    [document, 'pointerlockchange', lockChange], [target, 'contextmenu', suppress], [target, 'auxclick', suppress]];
  for (const [events, name, listener] of listeners) { events.addEventListener(name, listener); }
  target.addEventListener('wheel', wheelEvent, { passive: false });
  const dispose = () => {
    if (disposed) { return; }
    disposed = true;
    publishActive();
    clear();
    if (frame !== undefined) { cancelFrame?.(frame); }
    for (const [events, name, listener] of listeners) { events.removeEventListener(name, listener); }
    target.removeEventListener('wheel', wheelEvent, { passive: false });
    signal?.removeEventListener('abort', dispose);
    spans.delete(byteOffset);
  };
  signal?.addEventListener('abort', dispose, { once: true });
  publishActive();
  frame = requestFrame?.(watch);
  return Object.freeze({
    setActive: value => {
      if (disposed || enabled === Boolean(value)) { return; }
      enabled = Boolean(value);
      publishActive();
    },
    // the host's panel or console is open over the game
    setHost: ({ panel, console: open } = {}) => {
      if (panel !== undefined) { Atomics.store(words, K.hostPanel, panel ? 1 : 0); }
      if (open !== undefined) { Atomics.store(words, K.hostConsole, open ? 1 : 0); }
    },
    pointerFree: () => pointing() && !(flags() & K.mapDragFlag),
    snapshot: () => ({ active: Atomics.load(words, K.active), flags: flags(), changes: Atomics.load(words, K.change),
      clicks: Atomics.load(words, K.clicks), focused, enabled }),
    capturing: () => (flags() & K.captureFlag) !== 0,
    dispose,
  });
};
