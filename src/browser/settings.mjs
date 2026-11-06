import { actionNamed, keyNamed, padActionNamed, padButtonNamed } from './input-actions.mjs';

const normalizeKeys = value => Object.fromEntries(Object.entries(value).filter(([id, names]) => actionNamed(id) && typeof names === 'string')
  .map(([id, names]) => [id, names.split(',').map(keyNamed).filter(Boolean).map(entry => entry.name).join(',')]));
const normalizePads = value => Object.fromEntries(Object.entries(value).filter(([id, name]) => padActionNamed(id) && typeof name === 'string' &&
  (name === '' || padButtonNamed(name))).map(([id, name]) => [id, name && padButtonNamed(name).name]));

export const SETTINGS = Object.freeze({
  scaling: Object.freeze({ initial: 'fit', options: Object.freeze(['fit', 'integer', 'stretch']) }),
  filter: Object.freeze({ initial: 'smooth', options: Object.freeze(['smooth', 'sharp']) }),
  aspect: Object.freeze({ initial: 'auto', options: Object.freeze(['auto', '4:3', '16:10', '16:9', '21:9']) }),
  fov: Object.freeze({ initial: 75, min: 45, max: 140, step: 1 }),
  brightness: Object.freeze({ initial: 50, min: 0, max: 100, step: 1 }),
  contrast: Object.freeze({ initial: 50, min: 0, max: 100, step: 1 }),
  frameCap: Object.freeze({ initial: 60, options: Object.freeze([0, 30, 60]) }),
  anisotropy: Object.freeze({ initial: 0, options: Object.freeze([0, 2, 4, 8, 16]) }),
  msaa: Object.freeze({ initial: 0, options: Object.freeze([0, 4]), coerce: value => Number(value) > 1 ? 4 : 0 }),
  smaa: Object.freeze({ initial: false }),
  shadowResolution: Object.freeze({ initial: 512, options: Object.freeze([512, 1024, 2048, 4096]) }),
  shadowFilter: Object.freeze({ initial: 'console', options: Object.freeze(['console', 'soft']) }),
  shadowDistance: Object.freeze({ initial: 1, options: Object.freeze([1, 2, 4, 8]) }),
  shaderModel: Object.freeze({ initial: '1.1', options: Object.freeze(['1.1', '3.0']) }),
  hdrRendering: Object.freeze({ initial: false }),
  toneMapping: Object.freeze({ initial: false }),
  hdrOutput: Object.freeze({ initial: true }),
  hdrHeadroom: Object.freeze({ initial: 5, min: 1, max: 10, step: 0.5 }),
  language: Object.freeze({ initial: 'auto', options: Object.freeze(['auto', 'int', 'fra', 'deu', 'ita', 'esp']) }),
  audioOutput: Object.freeze({ initial: 'stereo', options: Object.freeze(['stereo', 'mono']) }),
  resolution: Object.freeze({ initial: 'console', options: Object.freeze(['console', 'window', 'dynamic']) }),
  resolutionScale: Object.freeze({ initial: 100, min: 50, max: 200, step: 5 }),
  fmvScale: Object.freeze({ initial: 'fill', options: Object.freeze(['fill', '4:3']) }),
  volume: Object.freeze({ initial: 100, min: 0, max: 100, step: 5 }),
  sensitivity: Object.freeze({ initial: 1, min: 0.1, max: 5, step: 0.1 }),
  weaponSensitivity: Object.freeze({ initial: 1, min: 0.1, max: 5, step: 0.1 }),
  sniperSensitivity: Object.freeze({ initial: 1, min: 0.1, max: 5, step: 0.1 }),
  invertY: Object.freeze({ initial: false }),
  keys: Object.freeze({ initial: Object.freeze({}), map: normalizeKeys }),
  padButtons: Object.freeze({ initial: Object.freeze({}), map: normalizePads }),
  skipIntros: Object.freeze({ initial: false }),
  pauseOnFocusLoss: Object.freeze({ initial: true }),
  developmentStart: Object.freeze({ initial: false }),
  showFps: Object.freeze({ initial: false }),
  showDiagnostics: Object.freeze({ initial: false }),
  console: Object.freeze({ initial: false }),
});

const normalize = (name, value) => {
  const setting = SETTINGS[name];
  if (setting.map) { return value && typeof value === 'object' && !Array.isArray(value) ? Object.freeze(setting.map(value)) : undefined; }
  if (setting.coerce && value !== undefined && value !== null && Number.isFinite(Number(value))) { return setting.coerce(value); }
  if (setting.options) { return setting.options.includes(value) ? value : undefined; }
  if (typeof setting.initial === 'boolean') { return typeof value === 'boolean' || typeof value === 'number' ? Boolean(value) : undefined; }
  const number = typeof value === 'string' && value.trim() !== '' ? Number(value) : value;
  if (typeof number !== 'number' || !Number.isFinite(number)) { return undefined; }
  const stepped = Math.round(number / setting.step) * setting.step;
  return Math.max(setting.min, Math.min(setting.max, Number(stepped.toFixed(4))));
};

const setting = name => SETTINGS[name];

const initialValues = () => Object.fromEntries(Object.entries(SETTINGS).map(([name, setting]) => [name, setting.initial]));

const load = (storage, key) => {
  const values = initialValues();
  let stored;
  try { stored = JSON.parse(storage?.getItem(key) ?? '{}'); }
  catch { return values; }
  if (!stored || typeof stored !== 'object') { return values; }
  for (const name of Object.keys(SETTINGS)) {
    const value = Object.hasOwn(stored, name) ? normalize(name, stored[name]) : undefined;
    if (value !== undefined) { values[name] = value; }
  }
  return values;
};

export const createSettings = ({ storage = globalThis.localStorage, key = 'scda.settings' } = {}) => {
  const values = load(storage, key);
  const listeners = new Set();
  const save = () => {
    try { storage?.setItem(key, JSON.stringify(values)); }
    catch {}
  };
  const assign = (name, value) => {
    const next = normalize(name, value);
    if (next === undefined || next === values[name] || (setting(name).map && JSON.stringify(next) === JSON.stringify(values[name]))) { return false; }
    values[name] = next;
    for (const listener of listeners) { listener(name, next); }
    return true;
  };
  return Object.freeze({
    get: name => values[name],
    snapshot: () => ({ ...values }),
    set: (name, value) => {
      if (!Object.hasOwn(SETTINGS, name)) { throw new RangeError(`Unknown setting ${name}.`); }
      if (assign(name, value)) { save(); }
    },
    reset: () => {
      let changed = false;
      for (const [name, value] of Object.entries(initialValues())) { changed = assign(name, value) || changed; }
      if (changed) { save(); }
    },
    subscribe: listener => {
      listeners.add(listener);
      return () => { listeners.delete(listener); };
    },
  });
};
