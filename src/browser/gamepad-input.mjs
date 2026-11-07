import { MENU_INPUT } from './menu-input.mjs';
import { PAD_BUTTONS } from './input-actions.mjs';

// input-core.h: the pad (words 0 to 7), the guest's rumble, then the device's motion readings for gyro aiming
export const GAMEPAD_INPUT = Object.freeze({ words: 144, bytes: 576, rumble: 8, motionHead: 9, motionFlags: 10, motionRing: 16, motionEntries: 16,
  motionEntryWords: 8 });
const RUMBLE_RENEW_MS = 100, RUMBLE_EFFECT_MS = 200;
const finite = values => values.every(Number.isFinite);
const screenAngle = () => Number(globalThis.screen?.orientation?.angle ?? globalThis.orientation ?? 0) || 0;

const owners = new WeakMap();
const buttons = [MENU_INPUT.confirm, MENU_INPUT.back, MENU_INPUT.x, MENU_INPUT.y, MENU_INPUT.white, MENU_INPUT.black,
  MENU_INPUT.leftTrigger, MENU_INPUT.rightTrigger, MENU_INPUT.select, MENU_INPUT.start, MENU_INPUT.leftThumb, MENU_INPUT.rightThumb,
  MENU_INPUT.up, MENU_INPUT.down, MENU_INPUT.left, MENU_INPUT.right];
const axis = value => Number.isFinite(value) ? Math.round(Math.max(-1, Math.min(1, value)) * 32767) : 0;
const strongest = values => values.reduce((best, value) => Math.abs(value) > Math.abs(best) ? value : best, 0);
// a physical pad and the on-screen one (touch-controls.mjs) as one standard pad: any press, the deepest trigger, the
// larger deflection of each axis
const mergePads = pads => pads.length === 1 ? pads[0] : {
  buttons: Array.from({ length: 17 }, (_, n) => ({ pressed: pads.some(pad => pad.buttons?.[n]?.pressed),
    value: Math.max(...pads.map(pad => Number(pad.buttons?.[n]?.value) || 0)) })),
  axes: [0, 1, 2, 3].map(n => strongest(pads.map(pad => Number.isFinite(pad.axes?.[n]) ? pad.axes[n] : 0))),
};

export const createGamepadInput = ({ buffer, byteOffset, target = globalThis.window, visibilityTarget = globalThis.document,
  navigator = globalThis.navigator, requestFrame = globalThis.requestAnimationFrame?.bind(globalThis),
  cancelFrame = globalThis.cancelAnimationFrame?.bind(globalThis), virtualPads = () => [], motion = () => false,
  motionSensor = typeof globalThis.DeviceMotionEvent === 'function' && navigator?.maxTouchPoints > 0, orientation = screenAngle,
  now = () => globalThis.performance.now(), signal } = {}) => {
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
  const floats = new Float32Array(buffer, byteOffset, GAMEPAD_INPUT.words);
  let disposed = false;
  let actuator, rumble = 0, rumbleAt = 0;
  // the motors the guest set, as the dual rumble a standard pad plays; an effect has a length, so a held one is renewed
  const playRumble = hardware => {
    const value = Atomics.load(words, GAMEPAD_INPUT.rumble) >>> 0;
    const target = hardware?.vibrationActuator;
    if (target !== actuator) { stopRumble(); actuator = target; }
    if (!actuator || (value === rumble && (!value || now() - rumbleAt < RUMBLE_RENEW_MS))) { return; }
    rumble = value;
    rumbleAt = now();
    if (!value) { void actuator.reset?.()?.catch?.(() => {}); return; }
    void actuator.playEffect?.('dual-rumble', { startDelay: 0, duration: RUMBLE_EFFECT_MS, strongMagnitude: (value & 0xffff) / 0xffff,
      weakMagnitude: (value >>> 16) / 0xffff })?.catch?.(() => {});
  };
  const stopRumble = () => {
    if (actuator && rumble) { void actuator.reset?.()?.catch?.(() => {}); }
    rumble = 0;
  };
  // a reading about the device's axes (x right, y up and z out of its screen held upright) turned by the screen's
  // rotation into the picture's, which are the controller's: the phone in the hands is the controller
  const readMotion = event => {
    const rate = event.rotationRate, gravity = event.accelerationIncludingGravity;
    if (!rate || !finite([rate.alpha, rate.beta, rate.gamma])) { return; }
    const radians = orientation() * Math.PI / 180, cos = Math.cos(radians), sin = Math.sin(radians);
    const turn = (x, y, z) => [x * cos - y * sin, x * sin + y * cos, z];
    const accelerated = Boolean(gravity) && finite([gravity.x, gravity.y, gravity.z]);
    const head = Atomics.load(words, GAMEPAD_INPUT.motionHead) >>> 0;
    const entry = GAMEPAD_INPUT.motionRing + head % GAMEPAD_INPUT.motionEntries * GAMEPAD_INPUT.motionEntryWords;
    Atomics.store(words, entry, Math.round(event.timeStamp * 1000) | 0);
    floats.set([...turn(rate.beta, rate.gamma, rate.alpha), ...(accelerated ? turn(gravity.x, gravity.y, gravity.z) : [0, 0, 0])], entry + 1);
    Atomics.store(words, entry + 7, accelerated ? 1 : 0);
    Atomics.store(words, GAMEPAD_INPUT.motionHead, (head + 1) | 0);
  };
  let listening = false;
  const listen = on => {
    if (on === listening) { return; }
    listening = on;
    if (on) { target.addEventListener('devicemotion', readMotion); }
    else { target.removeEventListener('devicemotion', readMotion); }
  };
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
    let pad, hardware;
    if (active && focused && !visibilityTarget.hidden) {
      try { hardware = Array.from(navigator?.getGamepads?.() ?? []).find(value => value?.connected && value.mapping === 'standard'); }
      catch {}
      const present = [hardware, ...virtualPads()].filter(Boolean);
      if (present.length) { pad = mergePads(present); }
    }
    if (disposed) { return; }
    listen(!disposed && Boolean(motion()));
    if (pad) {
      playRumble(hardware);
      let mask = 0;
      for (let n = 0; n < buttons.length; ++n) {
        if (pad.buttons?.[n]?.pressed) { mask |= buttons[n]; }
      }
      // the physical buttons in kPadNames order for the input core, with the triggers' travel
      let physical = 0;
      for (const { button, index } of PAD_BUTTONS) {
        if (pad.buttons?.[index]?.pressed) { physical |= 1 << button; }
      }
      const trigger = n => Math.round(Math.max(0, Math.min(1, Number(pad.buttons?.[n]?.value) || (pad.buttons?.[n]?.pressed ? 1 : 0))) * 255);
      physical |= (trigger(6) << 16) | (trigger(7) << 24);
      // connected: 1 a physical pad, 2 only a touch screen's on-screen one (input-core.cpp shows the pad's prompts)
      publish([hardware ? 1 : 2, mask, axis(pad.axes?.[0]), axis(-pad.axes?.[1]), axis(pad.axes?.[2]), axis(-pad.axes?.[3]), physical]);
    } else {
      stopRumble();
      neutral();
    }
    if (!disposed) { frame = requestFrame(poll); }
  };
  const blur = () => { focused = false; stopRumble(); neutral(); };
  const focus = () => { focused = true; neutral(); };
  const visibility = () => { neutral(); };
  const dispose = () => {
    if (disposed) { return; }
    disposed = true;
    if (frame !== undefined) { cancelFrame(frame); }
    listen(false);
    stopRumble();
    neutral();
    target.removeEventListener('blur', blur);
    target.removeEventListener('focus', focus);
    visibilityTarget.removeEventListener('visibilitychange', visibility);
    signal?.removeEventListener('abort', dispose);
    spans.delete(byteOffset);
  };
  publish([0, 0, 0, 0, 0, 0, 0], true);
  Atomics.store(words, GAMEPAD_INPUT.motionFlags, motionSensor ? 1 : 0);
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
