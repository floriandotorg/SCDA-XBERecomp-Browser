import { createShellController } from './boot-shell.mjs';
import { createMenuRuntime } from './menu-runtime.mjs';
import { createMissionMetrics } from './mission-metrics.mjs';
import { createMissionControls } from './mission-controls.mjs';
import { createBrowserAudio } from './browser-audio.mjs';
import { createSettings, SETTINGS } from './settings.mjs';
import { ACTIONS, PAD_ACTIONS, PAD_BUTTONS, keyNamed, keyOfButton, keyOfVk, keyOfWheel, vkOfCode } from './input-actions.mjs';
import { bind, bindPad, keyBinding, label as keyLabel, padBinding } from './input-bindings.mjs';
import { createWheelAccumulator } from './keyboard-input.mjs';
import { frameShape } from './frame-shape.mjs';
import { applyHostMessage, HOST_MESSAGES } from './host-messages.mjs';
import { createConsoleModel, routeHostMessage } from './console-model.mjs';
import { createTouchControls } from './touch-controls.mjs';
import { createSaveService } from './save-service.mjs';
import { SAVE_FORMAT } from './save-format.mjs';

const onOff = Object.freeze({ true: 'On', false: 'Off' });
// the render size a scale gives in Match window: the frame's 640 x 480 times the window's device rows over 480
let deviceRows = 480;
const renderSize = percent => {
  const scale = Math.min(3, deviceRows * percent / 100 / 480);
  return `${Math.floor(640 * scale)}x${Math.floor(480 * scale)}`;
};
const KEEP_CAPS = new Set(['EEV', 'OPSAT', 'EMF', 'HUD', 'COOP', 'LS', 'RS', 'LB', 'RB', 'LT', 'RT']);
const sentence = text => text.split(' ').map((word, n) => KEEP_CAPS.has(word) ? word : n ? word.toLowerCase() : word[0] + word.slice(1).toLowerCase()).join(' ');
const TABS = Object.freeze([
  { id: 'display', label: 'Display', rows: [
    { setting: 'scaling', label: 'Picture', values: { fit: 'Fit to window', integer: 'Pixel perfect', stretch: 'Stretch' } },
    { setting: 'aspect', label: 'Aspect ratio', values: { auto: 'Auto: match window', '4:3': '4:3', '16:10': '16:10', '16:9': '16:9', '21:9': '21:9' } },
    { setting: 'resolution', label: 'Resolution', values: { console: 'Console (480 lines)', window: 'Match window', dynamic: 'Dynamic' } },
    { setting: 'resolutionScale', label: 'Resolution scale', format: value => `${value}% (${renderSize(value)})` },
    { setting: 'filter', label: 'Upscaling', values: { smooth: 'Smooth', sharp: 'Sharp' } },
    { setting: 'fov', label: 'Field of view', format: value => value === 75 ? '75 (default)' : String(value) },
    { setting: 'brightness', label: 'Brightness', format: value => String(value) },
    { setting: 'contrast', label: 'Contrast', format: value => String(value) },
    { setting: 'frameCap', label: 'Frame rate cap', values: { 0: 'Display rate', 30: '30', 60: '60' } },
    { setting: 'fmvScale', label: 'Full screen movies', values: { fill: 'Fill', '4:3': '4:3' } },
    { fullscreen: true, label: 'Fullscreen' },
  ] },
  { id: 'graphics', label: 'Graphics', rows: [
    { setting: 'anisotropy', label: 'Anisotropic filtering', values: { 0: 'Off', 2: '2x', 4: '4x', 8: '8x', 16: '16x' } },
    { setting: 'msaa', label: 'Multisampling (MSAA)', restart: true, values: { 0: 'Off', 4: '4x' } },
    { setting: 'smaa', label: 'SMAA' },
    { setting: 'shadowResolution', label: 'Shadow resolution', values: { 512: '512 (console)', 1024: '1024', 2048: '2048', 4096: '4096' } },
    { setting: 'shadowFilter', label: 'Shadow filter', values: { console: 'Console', soft: 'Soft (SM3)' } },
    { setting: 'shadowDistance', label: 'Shadow distance', values: { 1: '1x', 2: '2x', 4: '4x', 8: '8x' } },
    { setting: 'shaderModel', label: 'Shader model', values: { '1.1': '1.1 (console)', '3.0': '3.0' } },
    { setting: 'hdrRendering', label: 'HDR rendering', restart: true, unavailable: shell => shell.settings.get('shaderModel') !== '3.0' && 'Off' },
    { setting: 'toneMapping', label: 'Tone mapping', unavailable: shell => shell.settings.get('shaderModel') !== '3.0' && 'Off' },
    { setting: 'hdrOutput', label: 'HDR output', unavailable: shell => !shell.hdrDisplay() && 'Not supported' },
    { setting: 'ambientOcclusion', label: 'Ambient occlusion', values: { off: 'Off', ssao: 'SSAO', gtao: 'GTAO' } },
    { setting: 'hdrHeadroom', label: 'HDR highlights', format: value => `${value.toFixed(1)}x`, shown: shell => shell.hdrDisplay() && shell.settings.get('hdrOutput') },
  ] },
  { id: 'audio', label: 'Audio', rows: [
    { setting: 'volume', label: 'Master volume', format: value => `${value}%` },
    { setting: 'audioOutput', label: 'Output', values: { stereo: 'Stereo', mono: 'Mono' } },
  ] },
  { id: 'controls', label: 'Controls', rows: [
    { setting: 'sensitivity', label: 'Free look sensitivity', format: value => value.toFixed(1) },
    { setting: 'weaponSensitivity', label: 'Weapon sensitivity', format: value => value.toFixed(1) },
    { setting: 'sniperSensitivity', label: 'Sniper sensitivity', format: value => value.toFixed(1) },
    { setting: 'invertY', label: 'Invert look' },
    { heading: 'Keyboard and mouse' },
    ...ACTIONS.filter(action => !action.hidden).map(action => ({ bind: action.id, label: sentence(action.label) })),
    { reset: 'keys', label: 'Restore defaults' },
  ] },
  { id: 'controller', label: 'Controller', rows: [
    ...PAD_ACTIONS.filter(action => !action.hidden).map(action => ({ padBind: action.id, label: sentence(action.label) })),
    { reset: 'padButtons', label: 'Restore defaults' },
  ] },
  { id: 'touch', label: 'Touch', rows: [
    { setting: 'touchControls', label: 'On-screen controls', values: { auto: 'Automatic', on: 'On', off: 'Off' } },
    { setting: 'touchOpacity', label: 'Opacity', format: value => `${value}%` },
    { setting: 'touchSize', label: 'Size', format: value => `${value}%` },
    { setting: 'touchLook', label: 'Look sensitivity', format: value => value.toFixed(1) },
    { setting: 'touchHaptics', label: 'Vibration' },
    { setting: 'touchFullscreen', label: 'Full screen on touch' },
  ] },
  { id: 'game', label: 'Game', rows: [
    { setting: 'language', label: 'Language', restart: true, values: { auto: 'Automatic', int: 'English', fra: 'Français', deu: 'Deutsch', ita: 'Italiano', esp: 'Español' } },
    { setting: 'skipIntros', label: 'Skip intros' },
    { setting: 'pauseOnFocusLoss', label: 'Pause when unfocused' },
  ] },
  { id: 'progress', label: 'Progress', rows: [
    { progress: 'saved', label: 'Saved progress' },
    { progress: 'storage', label: 'Browser storage' },
    { progress: 'export', label: 'Export to a file', button: 'Export' },
    { progress: 'import', label: 'Import from a file', button: 'Import' },
    { progress: 'restore', label: 'Progress before the import', button: 'Restore', shown: shell => shell.saves.state.backup },
    { progress: 'note' },
  ] },
  { id: 'developer', label: 'Developer', rows: [
    { setting: 'developmentStart', label: 'Skip to first mission' },
    { setting: 'showFps', label: 'Frame rate' },
    { setting: 'showDiagnostics', label: 'Diagnostics' },
    { setting: 'console', label: 'Console' },
  ] },
]);

const createDiscStore = (indexedDB = globalThis.indexedDB) => {
  const run = async (mode, action) => {
    if (!indexedDB) { return undefined; }
    const database = await new Promise((resolve, reject) => {
      const request = indexedDB.open('scda-front-end', 1);
      request.onupgradeneeded = () => { request.result.createObjectStore('disc'); };
      request.onsuccess = () => resolve(request.result);
      request.onerror = () => reject(request.error);
    });
    try {
      return await new Promise((resolve, reject) => {
        const request = action(database.transaction('disc', mode).objectStore('disc'));
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
    } finally { database.close(); }
  };
  return Object.freeze({
    load: () => run('readonly', store => store.get('handle')),
    save: handle => run('readwrite', store => store.put(handle, 'handle')),
  });
};

export const mountMenuShell = ({ root = document, window = globalThis.window, settings = createSettings(), discs = createDiscStore(), createSaves = createSaveService } = {}) => {
  const find = selector => {
    const element = root.querySelector(selector);
    if (!element) { throw new Error(`Menu shell element ${selector} is missing.`); }
    return element;
  };
  const body = root.body;
  const stage = find('#stage'), input = find('#iso'), boot = find('#boot'), progressFile = find('#progress-file');
  const detail = find('#detail'), diagnostic = find('#gpu'), fps = find('#fps'), gear = find('#gear');
  const front = find('#front'), mainMenu = find('#main-menu'), loadingLayer = find('#loading'), errorLayer = find('#error');
  const pauseLayer = find('#pause'), settingsLayer = find('#settings'), consoleLayer = find('#console'), savingLayer = find('#saving');
  const consoleLog = consoleLayer.querySelector('.log'), consoleLine = consoleLayer.querySelector('.line');
  const tabList = settingsLayer.querySelector('.tabs'), rowList = settingsLayer.querySelector('.rows');
  // ?progress=<name> keeps a test's saved progress apart from the player's
  const saves = createSaves({ namespace: new URLSearchParams(window.location?.search ?? '').get('progress') ?? 'progress', onChange: state => progressChanged(state) });
  const touch = createTouchControls({ root, window, container: find('#touch'), settings, onMenu: () => { setPanel('pause'); } });
  // phones and tablets: any file in the picker (it cannot always tell an ISO by type, validation checks the image) and
  // a guest thread pool of 64 workers, still twice the mission's peak of about 25, not 129 at about 8 MB each
  const touchDevice = Boolean(window.matchMedia?.('(any-pointer: coarse)')?.matches) || (window.navigator?.maxTouchPoints ?? 0) > 1;
  if (touchDevice) { input.removeAttribute('accept'); }
  // iOS plays page audio with the ringer switch on silent only as a playback session
  try { if (window.navigator?.audioSession) { window.navigator.audioSession.type = 'playback'; } } catch {}
  let wakeLock;
  const keepAwake = async () => {
    if (screen !== 'game' || root.hidden || wakeLock || !window.navigator?.wakeLock) { return; }
    wakeLock = window.navigator.wakeLock.request('screen').then(lock => {
      lock.addEventListener('release', () => { wakeLock = undefined; });
      if (screen !== 'game') { void lock.release(); }
      return lock;
    }).catch(() => { wakeLock = undefined; });
  };
  const letSleep = () => { void wakeLock?.then(lock => lock?.release()).catch(() => {}); };
  let canvas = find('#game');
  let screen = 'title', panel = 'none', settingsReturn = 'none', tab = TABS[0].id;
  let presented = false, dismissed = false, quitting = false, pickToStart = false;
  let handle, motion, audio, keyboard, idleTimer, padFrame, padPrevious = new Set();
  let capture, layoutMap, hostMessages, consoleOpen = false;
  const consoleModel = createConsoleModel({ send: line => hostMessages?.post(HOST_MESSAGES.console, line) ?? false });
  const showConsoleLog = () => {
    consoleLog.textContent = consoleModel.lines.join('\n');
    consoleLog.scrollTop = consoleLog.scrollHeight;
  };
  const captureWheel = createWheelAccumulator();
  void globalThis.navigator?.keyboard?.getLayoutMap?.().then(map => { layoutMap = map; if (panel === 'settings') { refreshRows(); } }).catch(() => {});
  let started = 0, lastUpdated = 0, fpsFrames = 0, fpsSince = 0;
  const measurement = createMissionMetrics();
  const createEvidence = () => ({ developmentStart: settings.get('developmentStart'), renderedFrames: 0, firstPresentedFrameMilliseconds: null,
    adapter: null, validationErrors: [], resources: null, get performance() { return measurement.snapshot(); }, get progress() { return progressEvidence(); } });
  let evidence = createEvidence();

  const showDiagnostics = () => {
    diagnostic.hidden = !settings.get('showDiagnostics');
    if (!diagnostic.hidden) { diagnostic.textContent = JSON.stringify(evidence, null, 2); }
  };
  let shape = { aspect: 4 / 3 }, hostSettings, layoutTimer, displayRate = 0;
  // HDR output needs an HDR display and a browser whose WebGPU canvas takes an extended range
  const hdrQuery = window.matchMedia?.('(dynamic-range: high)');
  const hdrCanvas = typeof GPUCanvasContext !== 'undefined' && 'getConfiguration' in GPUCanvasContext.prototype;
  const hdrDisplay = () => Boolean(hdrQuery?.matches && hdrCanvas);
  const rowShell = { settings, hdrDisplay, saves };
  hdrQuery?.addEventListener?.('change', () => {
    hostSettings?.refresh();
    if (panel === 'settings') { renderSettings(); }
  });
  // the language a session starts in: Automatic follows the browser's, English when the game lacks it
  const bootLanguage = () => {
    const chosen = settings.get('language');
    if (chosen !== 'auto') { return chosen; }
    const code = String(window.navigator?.language ?? '').slice(0, 2).toLowerCase();
    return { fr: 'fra', de: 'deu', it: 'ita', es: 'esp' }[code] ?? 'int';
  };
  // the display's refresh rate, from animation frames: a frame cap at or above it is left to presentation
  const measureDisplayRate = () => {
    const times = [];
    const frame = time => {
      times.push(time);
      if (times.length < 61) { window.requestAnimationFrame?.(frame); return; }
      const intervals = times.slice(1).map((value, n) => value - times[n]).sort((left, right) => left - right);
      displayRate = Math.round(1000 / intervals[intervals.length >> 1]);
      hostSettings?.refresh();
    };
    window.requestAnimationFrame?.(frame);
  };
  measureDisplayRate();
  const deviceSize = () => {
    const ratio = window.devicePixelRatio || 1;
    const size = { deviceWidth: Math.max(1, Math.min(8192, Math.round(shape.width * ratio))), deviceHeight: Math.max(1, Math.min(8192, Math.round(shape.height * ratio))) };
    deviceRows = size.deviceHeight;
    return size;
  };
  // the stage keeps out of a phone's notch and rounded corners (menu.html insets it by the safe area)
  const layout = () => {
    shape = frameShape({ aspect: settings.get('aspect'), scaling: settings.get('scaling'), width: stage.clientWidth || window.innerWidth,
      height: stage.clientHeight || window.innerHeight, ratio: window.devicePixelRatio || 1 });
    stage.style.setProperty('--frame-width', `${shape.width}px`);
    stage.style.setProperty('--frame-height', `${shape.height}px`);
    stage.dataset.filter = settings.get('filter');
    hostSettings?.refresh();
  };
  // a window drag resizes many times a second: the guest's shape follows once it settles
  const resized = () => {
    clearTimeout(layoutTimer);
    layoutTimer = setTimeout(layout, 150);
  };

  const activeLayer = () => panel === 'settings' ? settingsLayer : panel === 'pause' ? pauseLayer :
    screen === 'error' ? errorLayer : screen === 'title' ? mainMenu : undefined;
  const stops = () => [...(activeLayer()?.querySelectorAll('[data-nav]') ?? [])]
    .filter(element => !element.disabled && element.getClientRects().length > 0);
  const focusDefault = () => {
    const layer = activeLayer();
    if (!layer) {
      if (screen === 'game') { canvas.focus({ preventScroll: true }); }
      return;
    }
    const preferred = layer === mainMenu && !boot.disabled ? boot : stops()[0];
    preferred?.focus({ preventScroll: true });
  };
  const moveFocus = direction => {
    const list = stops();
    if (!list.length) { return; }
    const index = list.indexOf(root.activeElement);
    const next = index < 0 ? (direction > 0 ? 0 : list.length - 1) : (index + direction + list.length) % list.length;
    list[next].focus({ preventScroll: true });
  };

  const choiceText = row => row.fullscreen ? onOff[Boolean(root.fullscreenElement)] :
    row.values ? row.values[settings.get(row.setting)] : onOff[settings.get(row.setting)];
  const toggleFullscreen = () => {
    const request = root.fullscreenElement ? root.exitFullscreen() : root.documentElement.requestFullscreen();
    void request?.catch(() => {});
  };
  const step = (row, direction) => {
    if (row.fullscreen) { toggleFullscreen(); return; }
    const options = SETTINGS[row.setting].options;
    if (!options) { settings.set(row.setting, !settings.get(row.setting)); return; }
    const index = options.indexOf(settings.get(row.setting));
    settings.set(row.setting, options[(index + direction + options.length) % options.length]);
  };
  const rowsOf = () => TABS.find(entry => entry.id === tab);
  const slotText = (row, slot) => {
    if (capture && capture.id === (row.bind ?? row.padBind) && capture.slot === slot && capture.pad === Boolean(row.padBind)) {
      return row.padBind ? 'Press a button' : 'Press a key';
    }
    if (row.padBind) {
      const button = padBinding(settings.get('padButtons'), row.padBind);
      return button >= 0 ? PAD_BUTTONS[button].name : 'Unbound';
    }
    const name = keyBinding(settings.get('keys'), row.bind)[slot];
    return name ? keyLabel(name, layoutMap) : slot ? '' : 'Unbound';
  };
  const refreshRows = () => {
    for (const element of rowList.querySelectorAll('[data-slot]')) {
      const row = rowsOf().rows[Number(element.dataset.row)];
      element.textContent = slotText(row, Number(element.dataset.slot));
      element.toggleAttribute('data-waiting', Boolean(capture && capture.element === element));
    }
    for (const element of rowList.querySelectorAll('[data-progress]')) {
      const row = rowsOf().rows[Number(element.dataset.row)];
      if (element.tagName === 'BUTTON') { element.disabled = !progressAvailable(row.progress); }
      else { element.textContent = progressText(row.progress); }
      element.closest('.row').hidden = row.shown ? !row.shown(rowShell) : !element.textContent && element.tagName !== 'BUTTON';
    }
    for (const element of rowList.querySelectorAll('[data-row]:not([data-slot]):not([data-reset]):not([data-progress])')) {
      const row = rowsOf().rows[Number(element.dataset.row)];
      if (row.format) {
        const setting = SETTINGS[row.setting], value = settings.get(row.setting);
        element.value = String(value);
        element.style.setProperty('--fill', `${(value - setting.min) / (setting.max - setting.min) * 100}%`);
        element.nextElementSibling.textContent = row.format(value);
      } else {
        const unavailable = row.unavailable?.(rowShell);
        element.disabled = Boolean(unavailable);
        element.querySelector('b').textContent = unavailable || choiceText(row);
      }
      if (row.shown) { element.closest('.row').hidden = !row.shown(rowShell); }
    }
  };
  const renderSettings = () => {
    tabList.replaceChildren(...TABS.map(entry => {
      const button = root.createElement('button');
      Object.assign(button, { type: 'button', textContent: entry.label, tabIndex: entry.id === tab ? 0 : -1 });
      button.setAttribute('role', 'tab');
      button.setAttribute('aria-selected', String(entry.id === tab));
      button.dataset.tab = entry.id;
      if (entry.id === tab) { button.dataset.nav = ''; }
      return button;
    }));
    const current = rowsOf();
    rowList.replaceChildren(...current.rows.map((row, index) => {
      const line = root.createElement(row.bind || row.padBind || row.reset || row.progress ? 'div' : 'label');
      line.className = row.heading ? 'row heading' : row.progress === 'note' ? 'row note' : 'row';
      line.hidden = Boolean(row.shown && !row.shown(rowShell));
      const name = root.createElement('span');
      name.textContent = row.restart ? `${row.label} (next start)` : row.label ?? row.heading;
      if (row.heading) {
        line.append(name);
      } else if (row.progress) {
        const value = root.createElement(row.button ? 'button' : 'span');
        value.dataset.row = String(index);
        value.dataset.progress = row.progress;
        if (row.button) {
          Object.assign(value, { type: 'button', className: 'slot', textContent: row.button });
          value.dataset.nav = '';
          value.addEventListener('click', () => { void progressAction(row.progress); });
        }
        line.append(...(row.label ? [name] : []), value);
      } else if (row.reset) {
        const button = root.createElement('button');
        Object.assign(button, { type: 'button', className: 'slot', textContent: 'Restore' });
        button.dataset.nav = '';
        button.dataset.row = String(index);
        button.dataset.reset = row.reset;
        button.addEventListener('click', () => { stopCapture(); settings.set(row.reset, {}); });
        line.append(name, button);
      } else if (row.bind || row.padBind) {
        const slots = root.createElement('div');
        slots.className = row.padBind ? 'binds single' : 'binds';
        for (let slot = 0; slot < (row.padBind ? 1 : 2); ++slot) {
          const button = root.createElement('button');
          Object.assign(button, { type: 'button', className: 'slot' });
          button.dataset.nav = '';
          button.dataset.row = String(index);
          button.dataset.slot = String(slot);
          button.addEventListener('click', event => {
            if (capture) { return; }
            event.stopPropagation();
            startCapture(row, slot, button);
          });
          slots.append(button);
        }
        line.append(name, slots);
      } else if (row.format) {
        const setting = SETTINGS[row.setting];
        const holder = root.createElement('div');
        holder.className = 'slider';
        const range = root.createElement('input');
        Object.assign(range, { type: 'range', min: String(setting.min), max: String(setting.max), step: String(setting.step) });
        range.dataset.nav = '';
        range.dataset.row = String(index);
        range.addEventListener('input', () => { settings.set(row.setting, range.value); });
        holder.append(range, root.createElement('output'));
        line.append(name, holder);
      } else {
        const cycler = root.createElement('button');
        cycler.type = 'button';
        cycler.className = 'cycler';
        cycler.dataset.nav = '';
        cycler.dataset.row = String(index);
        const previous = root.createElement('i'), value = root.createElement('b'), next = root.createElement('i');
        previous.textContent = '‹';
        next.textContent = '›';
        previous.dataset.direction = '-1';
        cycler.append(previous, value, next);
        // a tap on the left third steps back, as the arrow there shows
        cycler.addEventListener('click', event => {
          const box = cycler.getBoundingClientRect();
          const left = event.detail > 0 && event.clientX < box.left + box.width / 3;
          step(row, left || event.target.closest?.('[data-direction]') ? -1 : 1);
        });
        line.append(name, cycler);
      }
      return line;
    }));
    refreshRows();
  };
  // a binding slot waits for the next key, mouse button, wheel notch or controller button; Escape cancels, Delete
  // clears
  const stopCapture = () => {
    if (!capture) { return; }
    capture = undefined;
    refreshRows();
  };
  const startCapture = (row, slot, element) => {
    capture = { id: row.bind ?? row.padBind, slot, pad: Boolean(row.padBind), element, armed: false };
    refreshRows();
    setTimeout(() => { if (capture) { capture.armed = true; } }, 0);
  };
  const answerKey = name => {
    const current = capture;
    stopCapture();
    if (current.pad) {
      if (name === '') { settings.set('padButtons', bindPad(settings.get('padButtons'), current.id, -1)); }
      return;
    }
    settings.set('keys', bind(settings.get('keys'), current.id, name, current.slot));
    current.element?.focus({ preventScroll: true });
  };
  const captureKey = event => {
    if (!capture?.armed) { return false; }
    event.preventDefault();
    event.stopImmediatePropagation();
    if (event.code === 'Escape') { stopCapture(); return true; }
    if (event.code === 'Delete') { answerKey(''); return true; }
    const key = keyOfVk(vkOfCode(event.code));
    if (key && !capture.pad) { answerKey(key.name); }
    return true;
  };
  const captureMouse = event => {
    if (!capture?.armed || capture.pad) { return; }
    const key = keyOfButton(event.button);
    if (!key) { return; }
    event.preventDefault();
    event.stopImmediatePropagation();
    answerKey(key.name);
  };
  const captureWheelEvent = event => {
    if (!capture?.armed || capture.pad) { return; }
    event.preventDefault();
    const notches = captureWheel(event.deltaY, event.deltaMode);
    if (notches) { answerKey(keyOfWheel(notches).name); }
  };
  const capturePad = pressed => {
    if (!capture?.armed || !capture.pad) { return false; }
    const button = PAD_BUTTONS.find(entry => pressed.has(entry.index));
    if (!button) { return true; }
    const current = capture;
    stopCapture();
    settings.set('padButtons', bindPad(settings.get('padButtons'), current.id, button.button));
    current.element?.focus({ preventScroll: true });
    return true;
  };
  const settingsPanelKey = event => {
    const vk = vkOfCode(event.code);
    return Boolean(vk) && keyBinding(settings.get('keys'), 'settings_panel').some(name => keyNamed(name)?.vk === vk);
  };
  const selectTab = (id, focus = true) => {
    tab = id;
    progressNote = '';
    if (id === 'progress') { void saves.refresh().catch(() => {}); }
    renderSettings();
    if (focus) { tabList.querySelector('[data-nav]')?.focus({ preventScroll: true }); }
  };
  const adjust = (direction, ranges) => {
    const element = root.activeElement;
    if (element?.dataset.tab !== undefined) {
      const index = TABS.findIndex(entry => entry.id === tab);
      selectTab(TABS[(index + direction + TABS.length) % TABS.length].id);
    } else if (element?.classList.contains('cycler')) {
      step(rowsOf().rows[Number(element.dataset.row)], direction);
    } else if (ranges && element?.type === 'range') {
      if (direction > 0) { element.stepUp(); } else { element.stepDown(); }
      element.dispatchEvent(new Event('input'));
    }
  };

  const pollPad = () => {
    padFrame = undefined;
    if (!activeLayer()) { padPrevious = new Set(); return; }
    padFrame = window.requestAnimationFrame(pollPad);
    let pad;
    try { pad = Array.from(window.navigator.getGamepads?.() ?? []).find(value => value?.connected); }
    catch {}
    if (!pad || !root.hasFocus()) { padPrevious = new Set(); return; }
    const pressed = new Set();
    pad.buttons.forEach((button, n) => { if (button.pressed) { pressed.add(n); } });
    const [x = 0, y = 0] = pad.axes;
    if (y < -0.6) { pressed.add('up'); }
    if (y > 0.6) { pressed.add('down'); }
    if (x < -0.6) { pressed.add('left'); }
    if (x > 0.6) { pressed.add('right'); }
    const fresh = (...keys) => keys.some(key => pressed.has(key) && !padPrevious.has(key));
    const freshButtons = new Set([...pressed].filter(key => typeof key === 'number' && !padPrevious.has(key)));
    padPrevious = pressed;
    if (capture?.pad) {
      if (freshButtons.size) { capturePad(freshButtons); }
      return;
    }
    if (fresh(12, 'up')) { moveFocus(-1); }
    if (fresh(13, 'down')) { moveFocus(1); }
    if (fresh(14, 'left')) { adjust(-1, true); }
    if (fresh(15, 'right')) { adjust(1, true); }
    if (fresh(0)) { root.activeElement?.click?.(); }
    if (fresh(1)) { back(); }
  };

  const show = () => {
    body.dataset.screen = screen;
    body.dataset.panel = panel;
    front.classList.toggle('shown', screen !== 'game');
    loadingLayer.classList.toggle('shown', screen === 'loading');
    errorLayer.classList.toggle('shown', screen === 'error');
    pauseLayer.classList.toggle('shown', panel === 'pause');
    settingsLayer.classList.toggle('shown', panel === 'settings');
    fps.hidden = !(settings.get('showFps') && screen === 'game');
    if (screen !== 'game' && consoleOpen) { consoleOpen = false; }
    consoleLayer.classList.toggle('shown', consoleOpen);
    keyboard?.setHost({ panel: panel !== 'none', console: consoleOpen });
    keyboard?.setActive(screen === 'game' && panel === 'none' && !consoleOpen);
    motion?.setSuspended(screen === 'game' && panel !== 'none');
    touch.setGame(screen === 'game' && panel === 'none' && !consoleOpen);
    if (screen === 'game') { void keepAwake(); } else { letSleep(); }
    if (activeLayer() && padFrame === undefined) { padFrame = window.requestAnimationFrame(pollPad); }
  };
  const setPanel = next => {
    panel = next === 'pause' && screen !== 'game' ? 'none' : next;
    show();
    focusDefault();
  };
  const setScreen = next => {
    if (next === screen) { return; }
    screen = next;
    if (panel === 'pause' && screen !== 'game') { panel = 'none'; }
    show();
    focusDefault();
  };
  const openSettings = () => {
    settingsReturn = panel;
    renderSettings();
    setPanel('settings');
  };
  const resume = () => {
    setPanel('none');
    if (motion) { stage.click(); }
  };
  // the last change the guest made reaches storage before its runtime goes away
  const quit = async () => {
    if (quitting) { return; }
    quitting = true;
    await saves.settled(4000);
    panel = 'none';
    void controller.stop();
  };
  const back = () => {
    if (capture) { stopCapture(); return; }
    if (panel === 'settings') { stopCapture(); setPanel(settingsReturn); }
    else if (panel === 'pause') { resume(); }
    else if (screen === 'error') { dismissed = true; render(controller.state); }
  };

  const render = state => {
    const active = ['validating', 'booting', 'running'].includes(state.phase);
    boot.disabled = active || state.stopping || state.code === 'TEARDOWN';
    boot.setAttribute('aria-busy', String(active || state.stopping));
    if (!active && !state.stopping) {
      quitting = false;
      saves.stopped();
    }
    let next;
    if (state.stopping) { next = quitting ? 'title' : screen; }
    else if (active) { next = presented ? 'game' : 'loading'; }
    else if (!dismissed && (state.phase === 'error' || (state.phase === 'stopped' && state.code !== 'CANCELLED'))) { next = 'error'; }
    else { next = 'title'; }
    detail.textContent = next === 'error' && state.message ? `${state.code}: ${state.message}` : '';
    setScreen(next);
    if (screen === 'title' && !mainMenu.contains(root.activeElement) && panel === 'none') { focusDefault(); }
  };

  const createMotion = options => {
    motion = createMissionControls({ ...options, canvas, capture: stage, pauseOnFocusLoss: () => settings.get('pauseOnFocusLoss') });
    motion.setSuspended(screen === 'game' && panel !== 'none');
    return motion;
  };
  const createAudio = options => {
    audio = createBrowserAudio({ ...options, volume: settings.get('volume') / 100, output: settings.get('audioOutput') });
    return audio;
  };
  let progressNote = '', savingTimer;
  const gameActive = () => ['validating', 'booting', 'running'].includes(controller.state.phase) || controller.state.stopping;
  const savedWhen = value => new Date(value).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' });
  const progressText = kind => {
    const state = saves.state;
    if (kind === 'saved') {
      if (state.store === 'loading') { return 'Checking'; }
      if (!saves.enabled || state.activity === 'failed') { return 'Not saved'; }
      if (state.activity === 'saving' || state.activity === 'pending') { return 'Saving'; }
      return state.savedAt ? savedWhen(state.savedAt) : 'Nothing yet';
    }
    if (kind === 'storage') {
      if (!Number.isFinite(state.usage)) { return 'Unknown'; }
      return `${(state.usage / 1048576).toFixed(1)} MB, ${state.persisted ? 'kept' : 'may be cleared'}`;
    }
    if (kind === 'note') { return progressNote || (state.activity === 'failed' ? state.error?.message : '') || state.message || ''; }
    return '';
  };
  const progressAvailable = kind => {
    if (kind === 'export') { return Boolean(saves.accepted); }
    return saves.enabled && !gameActive();
  };
  const progressEvidence = () => {
    const { store, activity, savedAt, persisted, usage, quota, backup, error } = saves.state;
    return { store, activity, savedAt, persisted, usage, quota, backup, error: error?.code, unsaved: saves.unsaved };
  };
  const flashSaving = (text, tone = '', duration = 0) => {
    clearTimeout(savingTimer);
    savingLayer.textContent = text;
    savingLayer.hidden = !text;
    savingLayer.dataset.tone = tone;
    if (duration) { savingTimer = setTimeout(() => { savingLayer.hidden = true; }, duration); }
  };
  const progressChanged = state => {
    if (state.activity === 'saving' || state.activity === 'pending') { flashSaving('Saving'); }
    else if (state.activity === 'failed') { flashSaving(`Progress not saved: ${state.error?.message ?? 'unknown failure'}`, 'alert'); }
    else if (state.activity === 'saved') { flashSaving('Progress saved', '', 2500); }
    if (panel === 'settings' && tab === 'progress') { refreshRows(); }
    if (!diagnostic.hidden) { showDiagnostics(); }
  };
  void saves.loaded.then(result => { if (result.message) { flashSaving(result.message, 'alert', 10000); } });
  const importProgress = async file => {
    if (file.size > SAVE_FORMAT.maximumPackageBytes) { throw new Error('This file is too large to be saved progress.'); }
    const bytes = new Uint8Array(await file.arrayBuffer());
    const described = await saves.inspectPackage(bytes);
    const question = `Replace your progress with the progress saved ${savedWhen(described.savedAt)}?` +
      (saves.accepted ? ' Your current progress is kept and can be restored here.' : '');
    if (!window.confirm(question)) { return; }
    const { settings: imported } = await saves.importPackage(bytes);
    for (const [name, value] of Object.entries(imported ?? {})) {
      if (Object.hasOwn(SETTINGS, name)) { settings.set(name, value); }
    }
    progressNote = `Imported the progress saved ${savedWhen(described.savedAt)}.`;
  };
  const progressAction = async (kind, file) => {
    try {
      if (kind === 'export') {
        const bytes = await saves.exportPackage(settings.snapshot());
        const name = `progress-${new Date().toISOString().slice(0, 10)}.scdasave`;
        const url = URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' }));
        const link = root.createElement('a');
        Object.assign(link, { href: url, download: name });
        link.click();
        setTimeout(() => URL.revokeObjectURL(url), 60000);
        progressNote = `Exported ${name}.`;
      } else if (kind === 'import') {
        if (!file) {
          progressFile.click();
          return;
        }
        await importProgress(file);
      } else if (kind === 'restore') {
        if (!window.confirm('Go back to the progress you had before the last import?')) { return; }
        await saves.restoreBackup();
        progressNote = 'Restored the progress from before the last import.';
      }
    } catch (error) {
      progressNote = error.message;
    } finally {
      if (panel === 'settings' && tab === 'progress') { refreshRows(); }
    }
  };
  const controller = createShellController({ onChange: state => render(state), createRuntime: () => createMenuRuntime({
    developmentStart: settings.get('developmentStart'), settings, createMotion, createAudio, ...(touchDevice ? { pthreadPool: 64 } : {}),
    ...(saves.enabled && saves.accepted ? { storage: saves.accepted } : {}),
    onStorage: ({ image }) => { saves.commit(image); },
    onStoragePending: pending => { saves.pending(pending); },
    onKeyboard: value => { keyboard = value; touch.setKeyboard(value); show(); },
    virtualPads: () => { const pad = touch.pad(); return pad ? [pad] : []; },
    onHostMessages: value => { hostMessages = value; },
    onHostMessage: message => routeHostMessage(message, {
      quit: () => { if (screen === 'game') { quit(); } },
      output: text => { consoleModel.print(text); if (consoleOpen) { showConsoleLog(); } },
      setting: value => { applyHostMessage(settings, value); },
    }, HOST_MESSAGES),
    hostComputed: () => ({ effectiveAspect: shape.aspect, ...deviceSize(), displayRate, bootLanguage: bootLanguage(), hdrDisplay: hdrDisplay() }),
    onHostSettings: value => { hostSettings = value; },
    acquireCanvas: () => {
      if (!globalThis.navigator?.gpu || typeof canvas.transferControlToOffscreen !== 'function') {
        throw Object.assign(new Error('WebGPU and an offscreen canvas are required in this browser.'), { code: 'CAPABILITY' });
      }
      const replacement = canvas.cloneNode(false);
      canvas.replaceWith(replacement);
      canvas = replacement;
      started = performance.now();
      measurement.begin();
      lastUpdated = fpsSince = started;
      fpsFrames = 0;
      evidence = createEvidence();
      showDiagnostics();
      return canvas.transferControlToOffscreen();
    },
    onAdapter: details => { evidence.adapter = details; showDiagnostics(); },
    onPresented: details => {
      const time = performance.now();
      measurement.present(time, details);
      ++evidence.renderedFrames;
      evidence.resources = details.resources;
      evidence.lastCompletedCommand = details.command;
      if (details.hookStatus) { evidence.hookStatus = details.hookStatus; }
      if (evidence.firstPresentedFrameMilliseconds === null) {
        evidence.firstPresentedFrameMilliseconds = time - started;
        presented = true;
      }
      ++fpsFrames;
      if (time - fpsSince >= 500) {
        if (!fps.hidden) { fps.textContent = `${Math.round(fpsFrames * 1000 / (time - fpsSince))} FPS  ${((time - fpsSince) / fpsFrames).toFixed(1)} MS`; }
        fpsFrames = 0;
        fpsSince = time;
      }
      if (evidence.renderedFrames === 1 || time - lastUpdated >= 1000) { lastUpdated = time; showDiagnostics(); }
    },
    onDiagnostic: details => {
      if (details.io) { evidence.io = details.io; }
      else if (details.guest) { evidence.guest = details.guest; }
      else if (details.runtime) { evidence.runtime = details.runtime; }
      else if (details.hooks) { evidence.hooks = details.hooks; }
      else if (details.settings) { evidence.settings = details.settings; }
      else {
        evidence.validationErrors.push(details);
        if (evidence.validationErrors.length > 16) { evidence.validationErrors.shift(); }
      }
      if (!details.io) { showDiagnostics(); }
    },
  }) });

  const select = async (file, start) => {
    if (!file) { return; }
    await controller.select(file);
    if (start) { await begin(); }
  };
  // what this browser lacks, said before the picker: a phone may copy a whole disc image for it first
  const missingCapability = () => {
    if (!window.isSecureContext) { return 'Open this page over HTTPS or from localhost. Browsers allow shared memory and WebGPU only in a secure context.'; }
    if (!window.crossOriginIsolated || typeof SharedArrayBuffer !== 'function') { return 'The server must send the cross origin isolation headers that tools/browser/serve.mjs sends.'; }
    if (!window.navigator?.gpu) { return 'This browser has no WebGPU. Use Chrome 121 or newer on Android or a desktop, or Safari on iOS 26, iPadOS 26 or macOS 26 or newer.'; }
    return '';
  };
  const notify = message => {
    dismissed = false;
    setScreen('error');
    detail.textContent = message;
  };
  const choose = async () => {
    const missing = missingCapability();
    if (missing) { notify(`CAPABILITY: ${missing}`); return; }
    if (typeof window.showOpenFilePicker !== 'function') {
      pickToStart = true;
      input.click();
      return;
    }
    let picked;
    try { [picked] = await window.showOpenFilePicker({ id: 'disc', types: [{ description: 'Disc image', accept: { 'application/x-iso9660-image': ['.iso'] } }] }); }
    catch { return; }
    handle = picked;
    void discs.save(picked).catch(() => {});
    await select(await picked.getFile(), true);
  };
  // a new runtime starts from the newest progress in storage, so a write still in flight finishes first
  const begin = async () => {
    dismissed = false;
    presented = false;
    await saves.loaded;
    await saves.settled(10000);
    void controller.start();
  };
  const start = async () => {
    if (controller.state.source) { await begin(); return; }
    if (handle) {
      try {
        const granted = await handle.queryPermission({ mode: 'read' }) === 'granted' || await handle.requestPermission({ mode: 'read' }) === 'granted';
        if (granted) { await select(await handle.getFile(), true); return; }
      } catch { handle = undefined; }
    }
    await choose();
  };

  const clicks = event => {
    const action = event.target.closest?.('[data-action]')?.dataset.action;
    const tabButton = event.target.closest?.('[data-tab]');
    if (tabButton) { selectTab(tabButton.dataset.tab); }
    else if (action === 'settings') { openSettings(); }
    else if (action === 'back') { back(); }
    else if (action === 'reset') { settings.reset(); }
    else if (action === 'resume') { resume(); }
    else if (action === 'quit') { quit(); }
    else if (action === 'dismiss') { back(); }
  };
  const consoleKey = event => {
    const vk = vkOfCode(event.code);
    return Boolean(vk) && settings.get('console') && keyBinding(settings.get('keys'), 'console').some(name => keyNamed(name)?.vk === vk);
  };
  const setConsole = open => {
    consoleOpen = open;
    show();
    if (open) { consoleLine.focus({ preventScroll: true }); showConsoleLog(); } else { focusDefault(); }
  };
  const keydown = event => {
    if (captureKey(event)) { return; }
    if (consoleOpen) {
      event.stopImmediatePropagation();
      if (event.code === 'Escape' || (consoleKey(event) && !event.repeat)) {
        event.preventDefault();
        setConsole(false);
      } else if (event.code === 'Enter') {
        event.preventDefault();
        if (consoleModel.submit(consoleLine.value)) { consoleLine.value = ''; }
        showConsoleLog();
      } else if (event.code === 'ArrowUp' || event.code === 'ArrowDown') {
        event.preventDefault();
        consoleLine.value = consoleModel.recall(event.code === 'ArrowUp' ? -1 : 1);
      }
      return;
    }
    if (screen === 'game' && panel === 'none' && consoleKey(event) && !keyboard?.capturing()) {
      event.preventDefault();
      event.stopImmediatePropagation();
      setConsole(true);
      return;
    }
    const settingsKey = settingsPanelKey(event) && !keyboard?.capturing();
    if (screen === 'game' && panel === 'none') {
      if (settingsKey) {
        event.preventDefault();
        event.stopImmediatePropagation();
        setPanel('pause');
      }
      return;
    }
    if (!activeLayer()) { return; }
    if (screen === 'game') { event.stopImmediatePropagation(); }
    const element = root.activeElement;
    if (event.code === 'ArrowUp' || event.code === 'ArrowDown') {
      event.preventDefault();
      moveFocus(event.code === 'ArrowDown' ? 1 : -1);
    } else if ((event.code === 'ArrowLeft' || event.code === 'ArrowRight') && element?.type !== 'range') {
      event.preventDefault();
      adjust(event.code === 'ArrowRight' ? 1 : -1, false);
    } else if (event.code === 'Escape' || event.code === 'Backspace') {
      event.preventDefault();
      back();
    } else if (event.code === 'Delete' && element?.dataset.slot !== undefined) {
      event.preventDefault();
      const row = rowsOf().rows[Number(element.dataset.row)];
      if (row.padBind) { settings.set('padButtons', bindPad(settings.get('padButtons'), row.padBind, -1)); }
      else { settings.set('keys', bind(settings.get('keys'), row.bind, '', Number(element.dataset.slot))); }
    } else if (settingsKey && screen === 'game' && element?.type !== 'range') {
      event.preventDefault();
      resume();
    }
  };
  const keyup = event => { if (screen === 'game' && (panel !== 'none' || consoleOpen)) { event.stopImmediatePropagation(); } };
  const hover = event => {
    const target = event.target.closest?.('[data-nav]');
    if (target && !target.disabled && activeLayer()?.contains(target) && root.activeElement !== target) { target.focus({ preventScroll: true }); }
  };
  const activity = () => {
    if (screen !== 'game' || root.pointerLockElement) { body.dataset.cursor = 'idle'; return; }
    body.dataset.cursor = 'active';
    clearTimeout(idleTimer);
    idleTimer = setTimeout(() => { body.dataset.cursor = 'idle'; }, 2500);
  };
  const fullscreen = () => { layout(); if (panel === 'settings') { refreshRows(); } };
  const visibility = () => { if (!root.hidden) { void keepAwake(); } };
  const startClick = () => { void start(); };
  const unload = event => {
    if (!saves.unsaved) { return; }
    event.preventDefault();
    event.returnValue = '';
  };
  const pickedProgress = () => {
    const file = progressFile.files?.[0];
    progressFile.value = '';
    if (file) { void progressAction('import', file); }
  };
  const picked = () => {
    const file = input.files?.[0];
    const startAfter = pickToStart;
    pickToStart = false;
    input.value = '';
    handle = undefined;
    void select(file, startAfter);
  };
  const stopSettings = settings.subscribe(name => {
    if (name === 'scaling' || name === 'filter' || name === 'aspect' || name === 'resolution') { layout(); }
    if (name === 'volume') { audio?.setVolume(settings.get('volume') / 100); }
    if (name === 'audioOutput') { audio?.setOutput(settings.get('audioOutput')); }
    if (name === 'showFps') { show(); }
    if (name === 'showDiagnostics') { showDiagnostics(); }
    if (name === 'pauseOnFocusLoss') { motion?.refresh(); }
    if (panel === 'settings') { refreshRows(); }
  });

  boot.addEventListener('click', startClick);
  gear.addEventListener('click', () => { setPanel('pause'); });
  input.addEventListener('change', picked);
  progressFile.addEventListener('change', pickedProgress);
  root.addEventListener('click', clicks);
  root.addEventListener('pointermove', hover);
  root.addEventListener('mousemove', activity);
  root.addEventListener('pointerlockchange', activity);
  root.addEventListener('fullscreenchange', fullscreen);
  root.addEventListener('visibilitychange', visibility);
  window.addEventListener('keydown', keydown, true);
  window.addEventListener('keyup', keyup, true);
  window.addEventListener('mousedown', captureMouse, true);
  window.addEventListener('wheel', captureWheelEvent, { capture: true, passive: false });
  window.addEventListener('resize', resized);
  window.addEventListener('beforeunload', unload);
  layout();
  showDiagnostics();
  renderSettings();
  render(controller.state);
  show();
  focusDefault();
  void discs.load().then(stored => {
    if (stored && !controller.state.source && !handle) { handle = stored; }
  }).catch(() => {});

  return Object.freeze({
    controller,
    settings,
    dispose: async () => {
      stopSettings();
      clearTimeout(idleTimer);
      if (padFrame !== undefined) { window.cancelAnimationFrame(padFrame); }
      boot.removeEventListener('click', startClick);
      input.removeEventListener('change', picked);
      progressFile.removeEventListener('change', pickedProgress);
      root.removeEventListener('click', clicks);
      root.removeEventListener('pointermove', hover);
      root.removeEventListener('mousemove', activity);
      root.removeEventListener('pointerlockchange', activity);
      root.removeEventListener('fullscreenchange', fullscreen);
      root.removeEventListener('visibilitychange', visibility);
      touch.dispose();
      letSleep();
      window.removeEventListener('keydown', keydown, true);
      window.removeEventListener('keyup', keyup, true);
      window.removeEventListener('mousedown', captureMouse, true);
      window.removeEventListener('wheel', captureWheelEvent, { capture: true, passive: false });
      window.removeEventListener('resize', resized);
      window.removeEventListener('beforeunload', unload);
      clearTimeout(layoutTimer);
      clearTimeout(savingTimer);
      await controller.stop();
      saves.dispose();
    },
    beginMeasurement: () => { measurement.begin(); },
    get measurement() { return measurement.snapshot(); },
    get evidence() { return evidence; },
    get input() { return keyboard?.snapshot(); },
    touch,
    saves,
    get audio() { return audio?.snapshot(); },
  });
};
