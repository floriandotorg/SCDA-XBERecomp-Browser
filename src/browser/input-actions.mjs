// The story game's keyboard and controller actions, ported from src/core/input.cpp (kStoryActions, kStoryPadActions,
// kNames, kPadNames). Keys are named and identified as native names them: by Windows virtual key code, letters, digits
// and symbols by their US position, which KeyboardEvent.code also gives.
const freeze = value => Object.freeze(value);
const action = (id, label, defaults, kind, options = {}) => freeze({ id, label, defaults, kind, playOnly: false, hidden: false, ...options });

// kind: pad (presses a pad target), event (the game side acts once per press), host (the host acts on it)
export const ACTIONS = freeze([
  action('move_forward', 'MOVE FORWARD', 'W', 'pad'),
  action('move_back', 'MOVE BACK', 'S', 'pad'),
  action('strafe_left', 'MOVE LEFT', 'A', 'pad'),
  action('strafe_right', 'MOVE RIGHT', 'D', 'pad'),
  action('jump', 'JUMP / SPLIT JUMP', 'Shift', 'pad'),
  action('crouch', 'CROUCH', 'C', 'pad'),
  action('zoom_in', 'ZOOM IN / ACCELERATE', 'Wheel Up', 'event'),
  action('zoom_out', 'ZOOM OUT / DECELERATE', 'Wheel Down', 'event'),
  action('back_to_wall', 'BACK TO WALL / SWITCH HAND', 'Q', 'pad'),
  action('fire', 'FIRE', 'LMB', 'pad'),
  action('alt_fire', 'ALTERNATE FIRE', 'RMB', 'pad'),
  action('interact', 'INTERACTION', 'Space,Enter', 'pad'),
  action('scope', 'USE / EQUIP', 'E,MMB', 'pad'),
  action('reload', 'RELOAD', 'R', 'event', { playOnly: true }),
  action('whistle', 'WHISTLE', 'V', 'event', { playOnly: true }),
  // native Ctrl: Ctrl with W closes the tab, which a page cannot prevent outside fullscreen keyboard lock
  action('quick_inventory', 'QUICK INVENTORY', 'Tab', 'pad'),
  action('opsat', 'OPSAT', 'F1', 'pad'),
  action('zoom', 'EEV VISION / ZOOM / SNIPER', '1', 'pad'),
  action('previous_vision', 'NIGHT VISION', '2', 'pad'),
  action('next_vision', 'THERMAL VISION', '3', 'pad'),
  action('goggles', 'EMF VISION', '4', 'pad'),
  action('switch_camera', 'STICKY CAM VISION', '5', 'pad', { playOnly: true }),
  action('sidekick', 'COOP ACTION', 'F', 'event', { playOnly: true }),
  action('push_to_talk', 'PUSH TO TALK', 'T', 'host', { hidden: true }),
  action('pause', 'PAUSE', 'Escape', 'pad'),
  action('quick_save', 'QUICK SAVE', 'F5', 'event', { playOnly: true }),
  action('quick_load', 'QUICK LOAD', 'F8', 'event', { playOnly: true }),
  action('console', 'CONSOLE', 'Backtick', 'host'),
  action('hide_hud', 'TOGGLE HUD', '', 'host'),
  action('settings_panel', 'SETTINGS PANEL', 'F10', 'host'),
]);

const pad = (id, label, button, options = {}) => freeze({ id, label, button, event: false, hidden: false, ...options });
// button: the console pad control it stands for and its stock physical button (kPadNames index), -1 for none
export const PAD_ACTIONS = freeze([
  pad('jump', 'JUMP / SPLIT JUMP', 3),
  pad('crouch', 'CROUCH / STAND UP / EXIT INTERACTION', 1),
  pad('back_to_wall', 'BACK TO WALL / SWITCH HAND / ZOOM IN OUT', 14),
  pad('fire', 'FIRE', 7),
  pad('alt_fire', 'ALTERNATE FIRE', 6),
  pad('interact', 'INTERACTION / RELOAD', 0),
  pad('scope', 'USE / EQUIP', 2),
  pad('whistle', 'WHISTLE / COOP ACTION', 4),
  pad('quick_inventory', 'QUICK INVENTORY', 5),
  pad('opsat', 'OPSAT', 13),
  pad('zoom', 'EEV VISION / ZOOM / SNIPER', 15),
  pad('night_vision', 'NIGHT VISION', 10),
  pad('thermal_vision', 'THERMAL VISION', 11),
  pad('emf_vision', 'EMF VISION', 8),
  pad('sticky_cam_vision', 'STICKY CAM VISION', 9),
  pad('pause', 'PAUSE', 12),
  pad('quick_save', 'QUICK SAVE', -1, { event: true }),
  pad('quick_load', 'QUICK LOAD', -1, { event: true }),
  pad('push_to_talk', 'PUSH TO TALK', -1, { event: true, hidden: true }),
  pad('toggle_hud', 'TOGGLE HUD', -1, { event: true }),
]);

// physical buttons in kPadNames order with their standard gamepad index; the touchpad halves are not in the standard
// mapping
export const PAD_BUTTONS = freeze(['A', 'B', 'X', 'Y', 'RB', 'LB', 'LT', 'RT', 'D-PAD UP', 'D-PAD DOWN', 'D-PAD LEFT',
  'D-PAD RIGHT', 'START', 'BACK', 'LS', 'RS'].map((name, button) => freeze({ name, button, index: [0, 1, 2, 3, 5, 4, 6, 7, 12, 13, 14, 15, 9, 8, 10, 11][button] })));

const key = (name, vk, codes = [], extra = {}) => freeze({ name, vk, codes: freeze(codes), ...extra });
const letters = [...'ABCDEFGHIJKLMNOPQRSTUVWXYZ'].map(letter => key(letter, letter.charCodeAt(0), [`Key${letter}`]));
const digits = [...'0123456789'].map(digit => key(digit, digit.charCodeAt(0), [`Digit${digit}`]));
const functions = [1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12].map(n => key(`F${n}`, 0x6f + n, [`F${n}`]));
const numpad = [...'0123456789'].map(digit => key(`Numpad${digit}`, 0x60 + Number(digit), [`Numpad${digit}`]));
export const KEY_NAMES = freeze([
  key('LMB', 0x01, [], { button: 0 }), key('RMB', 0x02, [], { button: 2 }), key('MMB', 0x04, [], { button: 1 }),
  key('Mouse4', 0x05, [], { button: 3 }), key('Mouse5', 0x06, [], { button: 4 }),
  key('Wheel Up', 0x0e, [], { wheel: 1 }), key('Wheel Down', 0x0f, [], { wheel: -1 }),
  key('Space', 0x20, ['Space']), key('Enter', 0x0d, ['Enter', 'NumpadEnter']), key('Escape', 0x1b, ['Escape']),
  key('Backspace', 0x08, ['Backspace']), key('Tab', 0x09, ['Tab']), key('Shift', 0x10, ['ShiftLeft', 'ShiftRight']),
  key('Ctrl', 0x11, ['ControlLeft', 'ControlRight']), key('Alt', 0x12, ['AltLeft', 'AltRight']), key('CapsLock', 0x14, ['CapsLock']),
  key('Up', 0x26, ['ArrowUp']), key('Down', 0x28, ['ArrowDown']), key('Left', 0x25, ['ArrowLeft']), key('Right', 0x27, ['ArrowRight']),
  key('Insert', 0x2d, ['Insert']), key('Delete', 0x2e, ['Delete']), key('Home', 0x24, ['Home']), key('End', 0x23, ['End']),
  key('PageUp', 0x21, ['PageUp']), key('PageDown', 0x22, ['PageDown']),
  ...letters, ...digits, ...functions, ...numpad,
  key('NumpadPlus', 0x6b, ['NumpadAdd']), key('NumpadMinus', 0x6d, ['NumpadSubtract']), key('NumpadStar', 0x6a, ['NumpadMultiply']),
  key('NumpadSlash', 0x6f, ['NumpadDivide']), key('NumpadPeriod', 0x6e, ['NumpadDecimal']),
  key('Backtick', 0xc0, ['Backquote']), key('Minus', 0xbd, ['Minus']), key('Equals', 0xbb, ['Equal']), key('LBracket', 0xdb, ['BracketLeft']),
  key('RBracket', 0xdd, ['BracketRight']), key('Backslash', 0xdc, ['Backslash']), key('Semicolon', 0xba, ['Semicolon']),
  key('Quote', 0xde, ['Quote']), key('Comma', 0xbc, ['Comma']), key('Period', 0xbe, ['Period']), key('Slash', 0xbf, ['Slash']),
]);

// keys the browser reaches that native leaves unnamed: held in the bitmap for "press a key" and the console, never bound
const UNNAMED_CODES = freeze({ F3: 0x72, NumLock: 0x90, ScrollLock: 0x91, Pause: 0x13, ContextMenu: 0x5d, MetaLeft: 0x5b, MetaRight: 0x5c,
  IntlBackslash: 0xe2, PrintScreen: 0x2c });

const byName = new Map(KEY_NAMES.map(entry => [entry.name.toLowerCase(), entry]));
const byCode = new Map([...KEY_NAMES.flatMap(entry => entry.codes.map(code => [code, entry.vk])), ...Object.entries(UNNAMED_CODES)]);
const byVk = new Map(KEY_NAMES.map(entry => [entry.vk, entry]));
const byButton = new Map(KEY_NAMES.filter(entry => entry.button !== undefined).map(entry => [entry.button, entry]));

export const keyNamed = name => typeof name === 'string' ? byName.get(name.trim().toLowerCase()) : undefined;
export const keyOfVk = vk => byVk.get(vk);
export const vkOfCode = code => byCode.get(code) ?? 0;
export const keyOfButton = button => byButton.get(button);
export const keyOfWheel = notch => notch > 0 ? byName.get('wheel up') : byName.get('wheel down');
export const isMouseKey = entry => entry?.button !== undefined || entry?.wheel !== undefined;
export const actionNamed = id => ACTIONS.find(entry => entry.id === id);
export const padActionNamed = id => PAD_ACTIONS.find(entry => entry.id === id);
export const padButtonNamed = name => typeof name === 'string' ? PAD_BUTTONS.find(entry => entry.name.toLowerCase() === name.trim().toLowerCase()) : undefined;
