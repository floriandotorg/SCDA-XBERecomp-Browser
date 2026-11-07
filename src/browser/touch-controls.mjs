// On-screen controls for touch screens. The overlay is a virtual standard pad that gamepad-input.mjs merges with a
// physical one, so the guest reads it as the console's controller and shows its prompts. A drag on the right of the
// screen looks as the mouse does (keyboard-input.mjs look), and while a game menu takes the cursor a finger on the
// picture is the mouse.
import { KEYBOARD_INPUT as K } from './keyboard-input.mjs';
import { PAD_ACTIONS, PAD_BUTTONS } from './input-actions.mjs';
import { padBinding } from './input-bindings.mjs';

// standard gamepad button indices
export const PAD_INDEX = Object.freeze({ a: 0, b: 1, x: 2, y: 3, lb: 4, rb: 5, lt: 6, rt: 7, back: 8, start: 9, ls: 10, rs: 11,
  up: 12, down: 13, left: 14, right: 15 });
const DEAD_ZONE = 0.08;

export const stickVector = (dx, dy, radius) => {
  const length = Math.hypot(dx, dy);
  if (!length || length / radius < DEAD_ZONE) { return [0, 0]; }
  const scale = Math.min(1, length / radius) / length;
  return [dx * scale, dy * scale];
};

export const touchMode = flags => {
  if (flags & K.textFlag) { return 'text'; }
  if ((flags & (K.menuFlag | K.pointerFlag)) && !(flags & K.mapDragFlag)) { return 'menu'; }
  if (flags & K.movieFlag) { return 'movie'; }
  return 'play';
};

const CAPTIONS = Object.freeze({ jump: 'Jump', crouch: 'Crouch', back_to_wall: 'Wall', fire: 'Fire', alt_fire: 'Alt fire',
  interact: 'Action', scope: 'Equip', whistle: 'Whistle', quick_inventory: 'Items', opsat: 'OPSAT', zoom: 'Zoom',
  night_vision: 'Night', thermal_vision: 'Thermal', emf_vision: 'EMF', sticky_cam_vision: 'Cam', pause: 'Pause' });

export const buttonCaption = (pads, index) => {
  const physical = PAD_BUTTONS.find(entry => entry.index === index)?.button;
  const action = PAD_ACTIONS.find(entry => !entry.event && padBinding(pads, entry.id) === physical);
  return action ? CAPTIONS[action.id] ?? '' : '';
};

export const createVirtualPad = () => {
  const holds = new Array(17).fill(0);
  const axes = [0, 0, 0, 0];
  let connected = false;
  return Object.freeze({
    setConnected: value => { connected = Boolean(value); },
    press: (index, down) => { holds[index] = Math.max(0, holds[index] + (down ? 1 : -1)); },
    setStick: (stick, x, y) => { axes[stick * 2] = x; axes[stick * 2 + 1] = y; },
    release: () => { holds.fill(0); axes.fill(0); },
    held: index => holds[index] > 0,
    sample: () => connected ? { connected: true, mapping: 'standard', axes: [...axes],
      buttons: holds.map(count => ({ pressed: count > 0, value: count > 0 ? 1 : 0 })) } : undefined,
  });
};

// the overlay's buttons: label, the modes that show them and where they sit (a CSS class)
const BUTTONS = Object.freeze([
  { index: PAD_INDEX.a, label: 'A', modes: 'play menu text movie', place: 'face a' },
  { index: PAD_INDEX.b, label: 'B', modes: 'play menu text movie', place: 'face b' },
  { index: PAD_INDEX.x, label: 'X', modes: 'play menu text', place: 'face x' },
  { index: PAD_INDEX.y, label: 'Y', modes: 'play menu text', place: 'face y' },
  { index: PAD_INDEX.rt, label: 'RT', modes: 'play', place: 'trigger rt' },
  { index: PAD_INDEX.lt, label: 'LT', modes: 'play', place: 'trigger lt' },
  { index: PAD_INDEX.rb, label: 'RB', modes: 'play menu', place: 'shoulder rb' },
  { index: PAD_INDEX.lb, label: 'LB', modes: 'play menu', place: 'shoulder lb' },
  { index: PAD_INDEX.ls, label: 'LS', modes: 'play', place: 'thumb ls' },
  { index: PAD_INDEX.rs, label: 'RS', modes: 'play', place: 'thumb rs' },
  { index: PAD_INDEX.up, label: '', modes: 'play menu text', place: 'dpad up' },
  { index: PAD_INDEX.down, label: '', modes: 'play menu text', place: 'dpad down' },
  { index: PAD_INDEX.left, label: '', modes: 'play menu text', place: 'dpad left' },
  { index: PAD_INDEX.right, label: '', modes: 'play menu text', place: 'dpad right' },
  { index: PAD_INDEX.back, label: 'Back', modes: 'play menu text', place: 'system back' },
  { index: PAD_INDEX.start, label: 'Start', modes: 'play menu text movie', place: 'system start' },
]);
const STICK_RADIUS = 56;
const LOOK_SCALE = 1.1;

export const createTouchControls = ({ root = document, window = globalThis.window, container, settings, onMenu = () => {} }) => {
  if (!container) { throw new Error('Touch controls need their container.'); }
  const pad = createVirtualPad();
  const coarse = window.matchMedia?.('(any-pointer: coarse)');
  let keyboard, inGame = false, touched = Boolean(coarse?.matches), otherInput = false, mode = 'play', frame, disposed = false;
  const fingers = new Map();

  const element = (tag, className, parent = container) => {
    const node = root.createElement(tag);
    node.className = className;
    parent.append(node);
    return node;
  };
  const moveZone = element('div', 'zone move');
  const lookZone = element('div', 'zone look');
  const stickBase = element('div', 'stick', moveZone);
  const stickKnob = element('div', 'knob', stickBase);
  const buttons = BUTTONS.map(entry => {
    const button = element('button', `pad ${entry.place}`);
    button.type = 'button';
    button.dataset.modes = entry.modes;
    button.dataset.index = String(entry.index);
    button.setAttribute('aria-label', entry.label || entry.place.split(' ')[1]);
    const label = element('b', '', button);
    label.textContent = entry.label;
    element('small', '', button);
    return button;
  });
  const menuButton = element('button', 'pad host menu');
  menuButton.type = 'button';
  menuButton.dataset.modes = 'play menu text movie';
  menuButton.setAttribute('aria-label', 'Pause and settings');
  menuButton.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M8 5v14M16 5v14"/></svg>';
  const typeButton = element('button', 'pad host keyboard');
  typeButton.type = 'button';
  typeButton.dataset.modes = 'text';
  typeButton.setAttribute('aria-label', 'Type a name');
  typeButton.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><rect x="2.5" y="6" width="19" height="12" rx="1.5"/><path d="M6 10h1M9.5 10h1M13 10h1M16.5 10h1M7 14h10"/></svg>';
  const typing = element('input', 'typing');
  Object.assign(typing, { type: 'text', autocomplete: 'off', autocapitalize: 'characters', spellcheck: false, enterKeyHint: 'done' });
  typing.setAttribute('aria-label', 'Name');

  const refreshCaptions = () => {
    const pads = settings.get('padButtons');
    for (const button of buttons) { button.querySelector('small').textContent = buttonCaption(pads, Number(button.dataset.index)); }
  };
  const applyLook = () => {
    container.style.setProperty('--touch-opacity', String(settings.get('touchOpacity') / 100));
    container.style.setProperty('--touch-scale', String(settings.get('touchSize') / 100));
  };
  const enabled = () => {
    const choice = settings.get('touchControls');
    return choice === 'on' || (choice === 'auto' && touched && !otherInput);
  };
  const shown = () => inGame && enabled();
  // the stick rests where a thumb first finds it, and goes there again on release
  const restStick = () => {
    stickBase.classList.remove('active');
    stickBase.style.left = stickBase.style.top = stickKnob.style.transform = '';
  };
  const releaseAll = () => {
    for (const [id, finger] of fingers) {
      if (finger.kind === 'pointer') { keyboard?.touch('cancel', finger.x, finger.y); }
      fingers.delete(id);
    }
    for (const button of buttons) { button.classList.remove('held'); }
    restStick();
    pad.release();
  };
  const update = () => {
    const visible = shown();
    container.hidden = !visible;
    pad.setConnected(visible);
    if (!visible) { releaseAll(); }
    if (typing !== root.activeElement && mode !== 'text') { typing.blur(); }
  };

  const haptic = () => { if (settings.get('touchHaptics')) { window.navigator?.vibrate?.(8); } };
  // the first touch in play takes the whole screen where the browser allows it, sideways; on its release, as a touch
  // grants user activation only then
  const immerse = () => {
    if (!settings.get('touchFullscreen') || root.fullscreenElement || !root.fullscreenEnabled) { return; }
    void root.documentElement.requestFullscreen?.({ navigationUI: 'hide' })
      ?.then(() => window.screen?.orientation?.lock?.('landscape'))?.catch(() => {});
  };
  const pressButton = (event, button) => {
    const index = Number(button.dataset.index);
    fingers.set(event.pointerId, { kind: 'button', index, button });
    pad.press(index, true);
    button.classList.add('held');
    haptic();
  };
  const down = event => {
    if (disposed || event.pointerType === 'mouse' && settings.get('touchControls') !== 'on') { return; }
    event.preventDefault();
    const button = event.target.closest?.('button.pad');
    if (button === menuButton) { releaseAll(); onMenu(); return; }
    if (button === typeButton) { typing.value = ''; typing.focus({ preventScroll: true }); return; }
    container.setPointerCapture?.(event.pointerId);
    if (button) { pressButton(event, button); return; }
    const zone = event.target.closest?.('.zone');
    if (!zone) { return; }
    if (mode !== 'play') {
      if ([...fingers.values()].some(finger => finger.kind === 'pointer')) { return; }
      fingers.set(event.pointerId, { kind: 'pointer', x: event.clientX, y: event.clientY });
      keyboard?.touch('down', event.clientX, event.clientY);
      return;
    }
    if (zone === moveZone && ![...fingers.values()].some(finger => finger.kind === 'stick')) {
      const bounds = moveZone.getBoundingClientRect();
      fingers.set(event.pointerId, { kind: 'stick', x: event.clientX, y: event.clientY });
      stickBase.style.left = `${event.clientX - bounds.left}px`;
      stickBase.style.top = `${event.clientY - bounds.top}px`;
      stickKnob.style.transform = '';
      stickBase.classList.add('active');
      return;
    }
    fingers.set(event.pointerId, { kind: 'look', x: event.clientX, y: event.clientY });
  };
  const move = event => {
    const finger = fingers.get(event.pointerId);
    if (!finger) { return; }
    event.preventDefault();
    if (finger.kind === 'stick') {
      const radius = STICK_RADIUS * settings.get('touchSize') / 100;
      const [x, y] = stickVector(event.clientX - finger.x, event.clientY - finger.y, radius);
      pad.setStick(0, x, y);
      stickKnob.style.transform = `translate(${x * radius}px, ${y * radius}px)`;
    } else if (finger.kind === 'look') {
      const scale = LOOK_SCALE * settings.get('touchLook');
      keyboard?.look((event.clientX - finger.x) * scale, (event.clientY - finger.y) * scale);
      finger.x = event.clientX;
      finger.y = event.clientY;
    } else if (finger.kind === 'pointer') {
      finger.x = event.clientX;
      finger.y = event.clientY;
      keyboard?.touch('move', event.clientX, event.clientY);
    } else if (finger.kind === 'button') {
      // a thumb rolling from one button onto its neighbour presses that one instead
      const over = root.elementFromPoint?.(event.clientX, event.clientY)?.closest?.('button.pad');
      if (over && over !== finger.button && over.dataset.index !== undefined && !over.closest('[hidden]')) {
        pad.press(finger.index, false);
        finger.button.classList.toggle('held', pad.held(finger.index));
        pressButton(event, over);
      }
    }
  };
  const up = event => {
    if (event.type === 'pointerup' && event.pointerType === 'touch') { immerse(); }
    const finger = fingers.get(event.pointerId);
    if (!finger) { return; }
    fingers.delete(event.pointerId);
    if (finger.kind === 'stick') {
      pad.setStick(0, 0, 0);
      restStick();
    } else if (finger.kind === 'pointer') {
      keyboard?.touch(event.type === 'pointerup' ? 'up' : 'cancel', event.clientX, event.clientY);
    } else if (finger.kind === 'button') {
      pad.press(finger.index, false);
      finger.button.classList.toggle('held', pad.held(finger.index));
    }
  };
  const anyTouch = event => {
    if (event.pointerType !== 'touch' || touched && !otherInput) { return; }
    touched = true;
    otherInput = false;
    update();
  };
  // the keyboard's Done accepts the name as the pad's A does
  const accept = () => {
    typing.blur();
    pad.press(PAD_INDEX.a, true);
    setTimeout(() => { pad.press(PAD_INDEX.a, false); }, 120);
  };
  const typed = event => {
    event.preventDefault();
    if (event.inputType === 'deleteContentBackward') { keyboard?.type('\b'); }
    else if (event.inputType === 'insertLineBreak' || event.inputType === 'insertParagraph') { accept(); }
    else if (event.data) { keyboard?.type(event.data); }
  };
  const typedKey = event => {
    if (event.key === 'Enter') { event.preventDefault(); accept(); }
    event.stopPropagation();
  };
  // a physical keyboard's key hides the overlay in Automatic until the next touch, as a physical pad's press does
  const keyed = event => {
    if (event.target === typing || otherInput || settings.get('touchControls') !== 'auto' || !inGame) { return; }
    otherInput = true;
    update();
  };
  // the guest's flags pick the layout
  const watch = () => {
    frame = window.requestAnimationFrame(watch);
    if (!inGame) { return; }
    const next = touchMode(keyboard?.flags() ?? 0);
    if (next !== mode) {
      releaseAll();
      mode = next;
      container.dataset.mode = mode;
      if (mode !== 'text') { typing.blur(); }
    }
    if (!otherInput && settings.get('touchControls') === 'auto') {
      let physical;
      try { physical = Array.from(window.navigator.getGamepads?.() ?? []).find(value => value?.connected && value.buttons?.some(button => button.pressed)); }
      catch {}
      if (physical) { otherInput = true; update(); }
    }
  };
  const blur = () => { releaseAll(); };
  const stopSettings = settings.subscribe(name => {
    if (name === 'padButtons') { refreshCaptions(); }
    if (name === 'touchOpacity' || name === 'touchSize') { applyLook(); }
    if (name === 'touchControls') { otherInput = false; update(); }
  });
  container.addEventListener('pointerdown', down);
  container.addEventListener('pointermove', move);
  container.addEventListener('pointerup', up);
  container.addEventListener('pointercancel', up);
  container.addEventListener('contextmenu', event => { event.preventDefault(); });
  typing.addEventListener('beforeinput', typed);
  typing.addEventListener('keydown', typedKey);
  root.addEventListener('pointerdown', anyTouch, true);
  window.addEventListener('keydown', keyed);
  window.addEventListener('blur', blur);
  container.dataset.mode = mode;
  refreshCaptions();
  applyLook();
  update();
  frame = window.requestAnimationFrame(watch);

  return Object.freeze({
    pad: () => pad.sample(),
    setKeyboard: value => { keyboard = value; },
    setGame: value => {
      if (inGame === Boolean(value)) { return; }
      inGame = Boolean(value);
      update();
    },
    // touch is how this device is used: the page shows touch layouts
    active: () => enabled(),
    shown,
    dispose: () => {
      disposed = true;
      window.cancelAnimationFrame(frame);
      stopSettings();
      releaseAll();
      root.removeEventListener('pointerdown', anyTouch, true);
      window.removeEventListener('keydown', keyed);
      window.removeEventListener('blur', blur);
    },
  });
};
