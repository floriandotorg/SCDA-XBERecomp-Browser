export const SETTINGS = Object.freeze({
  scaling: Object.freeze({ initial: 'fit', options: Object.freeze(['fit', 'integer', 'stretch']) }),
  filter: Object.freeze({ initial: 'smooth', options: Object.freeze(['smooth', 'sharp']) }),
  volume: Object.freeze({ initial: 100, min: 0, max: 100, step: 5 }),
  sensitivity: Object.freeze({ initial: 1, min: 0.1, max: 5, step: 0.1 }),
  invertY: Object.freeze({ initial: false }),
  developmentStart: Object.freeze({ initial: false }),
  showFps: Object.freeze({ initial: false }),
  showDiagnostics: Object.freeze({ initial: false }),
});

const normalize = (name, value) => {
  const setting = SETTINGS[name];
  if (setting.options) { return setting.options.includes(value) ? value : undefined; }
  if (typeof setting.initial === 'boolean') { return typeof value === 'boolean' || typeof value === 'number' ? Boolean(value) : undefined; }
  const number = typeof value === 'string' && value.trim() !== '' ? Number(value) : value;
  if (typeof number !== 'number' || !Number.isFinite(number)) { return undefined; }
  const stepped = Math.round(number / setting.step) * setting.step;
  return Math.max(setting.min, Math.min(setting.max, Number(stepped.toFixed(4))));
};

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
    if (next === undefined || next === values[name]) { return false; }
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
