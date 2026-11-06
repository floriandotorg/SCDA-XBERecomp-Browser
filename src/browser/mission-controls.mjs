// Pointer lock and pause for a mission: words 2 to 4 hold the simulation while the page is in the background or the
// host panel is open. Mouse movement goes to the guest's input core through keyboard-input.mjs.
const owners = new WeakMap();
export const createMissionControls = ({ buffer, byteOffset, document = globalThis.document, window = globalThis.window,
  canvas = document?.querySelector('#game'), capture = canvas, pauseOnFocusLoss = () => true, pointerFree = () => false,
  clear = () => {}, now = () => performance.now(), signal } = {}) => {
  if (!(buffer instanceof SharedArrayBuffer) || buffer.growable) { throw new TypeError('Mission controls require fixed shared memory.'); }
  if (!Number.isSafeInteger(byteOffset) || byteOffset < 0 || byteOffset % 4 || byteOffset > buffer.byteLength - 20) { throw new RangeError('Mission input span is out of bounds.'); }
  if (!canvas || !capture?.addEventListener || typeof pauseOnFocusLoss !== 'function' || typeof pointerFree !== 'function' || !document?.addEventListener || !window?.addEventListener) { throw new TypeError('Mission controls require their canvas and settings.'); }
  signal?.throwIfAborted();
  const spans = owners.get(buffer) ?? new Set();
  for (const start of spans) {
    if (start < byteOffset + 20 && byteOffset < start + 20) { throw new Error('Mission input span already has a producer.'); }
  }
  spans.add(byteOffset);
  owners.set(buffer, spans);
  const words = new Int32Array(buffer, byteOffset, 5);
  Atomics.store(words, 0, 0);
  Atomics.store(words, 1, 0);
  let focused = document.hasFocus?.() ?? true;
  let disposed = false;
  let suspended = false;
  let pausedAt = Atomics.load(words, 2) ? Atomics.load(words, 3) >>> 0 : undefined;
  const reset = () => { Atomics.store(words, 0, 0); Atomics.store(words, 1, 0); clear(); };
  const locked = () => document.pointerLockElement === canvas;
  const update = () => {
    reset();
    const paused = (!focused && pauseOnFocusLoss()) || document.hidden || suspended;
    const time = Math.floor(now());
    if (paused && pausedAt === undefined) {
      pausedAt = time;
      Atomics.store(words, 3, time);
      Atomics.store(words, 2, 1);
      if (locked()) { document.exitPointerLock(); }
    } else if (!paused && pausedAt !== undefined) {
      Atomics.add(words, 4, Math.max(0, time - pausedAt));
      Atomics.store(words, 2, 0);
      pausedAt = undefined;
      Atomics.notify(words, 2);
    }
  };
  // raw counts as native raw input reads them where the browser offers that, else the plain lock; a game menu keeps
  // the cursor free, a click there is the menu's
  const request = async () => {
    if (disposed || !focused || document.hidden || suspended || locked() || pointerFree()) { return; }
    canvas.focus({ preventScroll: true });
    if (typeof canvas.requestPointerLock !== 'function') { return; }
    for (const options of [{ unadjustedMovement: true }, undefined]) {
      try {
        await canvas.requestPointerLock(options);
        break;
      } catch {}
    }
    if (disposed && locked()) { document.exitPointerLock(); }
  };
  const release = () => { if (locked()) { document.exitPointerLock(); } reset(); };
  const blur = () => { focused = false; update(); };
  const focus = () => { focused = true; update(); };
  const failure = () => { reset(); };
  const dispose = () => {
    if (disposed) { return; }
    disposed = true;
    if (pausedAt !== undefined) {
      Atomics.add(words, 4, Math.max(0, Math.floor(now()) - pausedAt));
      Atomics.store(words, 2, 0);
      Atomics.notify(words, 2);
      pausedAt = undefined;
    }
    release();
    capture.removeEventListener('click', request);
    document.removeEventListener('pointerlockchange', update);
    document.removeEventListener('pointerlockerror', failure);
    document.removeEventListener('visibilitychange', update);
    window.removeEventListener('blur', blur);
    window.removeEventListener('focus', focus);
    signal?.removeEventListener('abort', dispose);
    spans.delete(byteOffset);
  };
  capture.addEventListener('click', request);
  document.addEventListener('pointerlockchange', update);
  document.addEventListener('pointerlockerror', failure);
  document.addEventListener('visibilitychange', update);
  window.addEventListener('blur', blur);
  window.addEventListener('focus', focus);
  signal?.addEventListener('abort', dispose, { once: true });
  const setSuspended = value => {
    if (disposed || suspended === Boolean(value)) { return; }
    suspended = Boolean(value);
    if (suspended) { release(); }
    update();
  };
  update();
  const refresh = () => { if (!disposed) { update(); } };
  return Object.freeze({ dispose, setSuspended, release, refresh });
};
