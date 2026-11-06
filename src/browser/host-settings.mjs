// The one schema of settings that reach the guest: app::Settings field, its place in the shared block, and the
// settings.mjs key it comes from. src/browser/host-settings.h carries the matching indices.
export const HOST_SETTINGS_SCHEMA = 1;
export const HOST_SETTINGS_WORDS = 64;
export const HOST_SETTINGS_BYTES = 4096;
export const HOST_SETTINGS_SPAN = HOST_SETTINGS_WORDS * 4 + HOST_SETTINGS_BYTES;
export const HOST_SETTINGS_FIELDS = Object.freeze([
  Object.freeze({ field: 'skip_intros', key: 'skipIntros', type: 'bool', bit: 0 }),
  Object.freeze({ field: 'pause_on_focus_loss', key: 'pauseOnFocusLoss', type: 'bool', bit: 1 }),
  Object.freeze({ field: 'invert_mouse', key: 'invertY', type: 'bool', bit: 2 }),
  Object.freeze({ field: 'mouse_sensitivity', key: 'sensitivity', type: 'f32', word: 3 }),
  Object.freeze({ field: 'free_look_sensitivity', key: 'sensitivity', type: 'f32', word: 4 }),
  Object.freeze({ field: 'weapon_sensitivity', key: 'weaponSensitivity', type: 'f32', word: 5 }),
  Object.freeze({ field: 'sniper_sensitivity', key: 'sniperSensitivity', type: 'f32', word: 6 }),
  Object.freeze({ field: 'fov', key: 'fov', type: 'u32', word: 7 }),
  Object.freeze({ field: 'fmv_scale', key: 'fmvScale', type: 'u32', word: 8, values: Object.freeze(['fill', '4:3']) }),
  // the frame's shape the page shows (frame-shape.mjs), computed from the aspect setting and the window
  Object.freeze({ field: 'aspect', key: 'effectiveAspect', type: 'f32', word: 9, computed: true }),
  // render scale (C07): the mode is the browser's own, the percent native's render_percent; the canvas's device pixels
  Object.freeze({ field: 'resolution', key: 'resolution', type: 'u32', word: 10, values: Object.freeze(['console', 'window', 'dynamic']), browser: true }),
  Object.freeze({ field: 'render_percent', key: 'resolutionScale', type: 'u32', word: 11 }),
  Object.freeze({ field: 'device_width', key: 'deviceWidth', type: 'u32', word: 12, computed: true }),
  Object.freeze({ field: 'device_height', key: 'deviceHeight', type: 'u32', word: 13, computed: true }),
  // the simple settings pack (C12)
  Object.freeze({ field: 'aniso', key: 'anisotropy', type: 'u32', word: 14 }),
  Object.freeze({ field: 'brightness', key: 'brightness', type: 'u32', word: 15 }),
  Object.freeze({ field: 'contrast', key: 'contrast', type: 'u32', word: 16 }),
  Object.freeze({ field: 'fps_cap', key: 'frameCap', type: 'u32', word: 17 }),
  Object.freeze({ field: 'display_rate', key: 'displayRate', type: 'u32', word: 18, computed: true }),
  Object.freeze({ field: 'audio_output', key: 'audioOutput', type: 'u32', word: 19, values: Object.freeze(['auto', 'stereo', 'surround', 'mono']) }),
  Object.freeze({ field: 'language', key: 'bootLanguage', type: 'text', computed: true }),
  // anti-aliasing (C08)
  Object.freeze({ field: 'msaa', key: 'msaa', type: 'u32', word: 20 }),
  Object.freeze({ field: 'smaa', key: 'smaa', type: 'bool', bit: 3 }),
  // shadows (C09): shadow_filter is native's 1 (console) or 3 (shader model 3 soft)
  Object.freeze({ field: 'shadow_resolution', key: 'shadowResolution', type: 'u32', word: 21 }),
  Object.freeze({ field: 'shadow_filter', key: 'shadowFilter', type: 'u32', word: 22, values: Object.freeze(['off', 'console', 'two', 'soft']) }),
  Object.freeze({ field: 'shadow_distance', key: 'shadowDistance', type: 'u32', word: 23 }),
  Object.freeze({ field: 'shader_model', key: 'shaderModel', type: 'u32', word: 24, values: Object.freeze(['1.1', '3.0']) }),
  Object.freeze({ field: 'hdr_rendering', key: 'hdrRendering', type: 'bool', bit: 4 }),
  Object.freeze({ field: 'tone_mapping', key: 'toneMapping', type: 'bool', bit: 5 }),
  Object.freeze({ field: 'hdr_output', key: 'hdrOutput', type: 'bool', bit: 6 }),
  Object.freeze({ field: 'hdr_headroom', key: 'hdrHeadroom', type: 'f32', word: 25, browser: true }),
  // the page's display shows HDR and its browser takes an extended range canvas
  Object.freeze({ field: 'hdr_display', key: 'hdrDisplay', type: 'bool', bit: 7, computed: true }),
  Object.freeze({ field: 'keys', key: 'keys', type: 'map' }),
  Object.freeze({ field: 'pad_buttons', key: 'padButtons', type: 'map' }),
  // browser settings the in-game options pages show and step (settings-rows.cpp), as host.<key>=<value> lines
  ...['scaling', 'filter', 'volume', 'showFps', 'skipIntros', 'pauseOnFocusLoss', 'sensitivity', 'weaponSensitivity',
    'sniperSensitivity', 'invertY', 'aspect', 'fov', 'fmvScale', 'resolution', 'resolutionScale', 'anisotropy', 'brightness', 'contrast', 'frameCap', 'language', 'audioOutput', 'msaa', 'smaa', 'shadowResolution', 'shadowFilter', 'shadowDistance', 'shaderModel', 'hdrRendering', 'toneMapping', 'hdrOutput'].map(key => Object.freeze({ field: `host.${key}`, key, type: 'host' })),
]);
const REVISION = 0, SCHEMA = 1, FLAGS = 2;
const ATTEMPTS = 1 << 16;
const keys = new Set(HOST_SETTINGS_FIELDS.map(entry => entry.key));

const fail = message => { throw Object.assign(new Error(message), { code: 'HOST_SETTINGS' }); };

export const encodeHostSettings = values => {
  const words = new Uint32Array(HOST_SETTINGS_WORDS);
  words[SCHEMA] = HOST_SETTINGS_SCHEMA;
  const lines = [];
  for (const entry of HOST_SETTINGS_FIELDS) {
    const value = values?.[entry.key];
    const invalid = () => fail(`Invalid host setting ${entry.key}.`);
    if (value === undefined) { continue; }
    if (entry.type === 'bool') {
      if (typeof value !== 'boolean') { invalid(); }
      if (value) { words[FLAGS] |= 1 << entry.bit; }
    } else if (entry.type === 'u32') {
      const number = entry.values ? entry.values.indexOf(value) : value;
      if (!Number.isInteger(number) || number < 0 || number > 0xffffffff) { invalid(); }
      words[entry.word] = number;
    } else if (entry.type === 'f32') {
      if (!Number.isFinite(value)) { invalid(); }
      words[entry.word] = new Uint32Array(Float32Array.of(value).buffer)[0];
    } else if (entry.type === 'text') {
      if (typeof value !== 'string' || /[\n=\0]/.test(value)) { invalid(); }
      lines.push(`${entry.field}=${value}`);
    } else if (entry.type === 'host') {
      if (!['string', 'number', 'boolean'].includes(typeof value) || /[\n=\0]/.test(String(value))) { invalid(); }
      lines.push(`${entry.field}=${value}`);
    } else if (entry.type === 'map') {
      if (!value || typeof value !== 'object') { invalid(); }
      for (const [name, item] of Object.entries(value)) {
        if (typeof item !== 'string' || /[\n=\0]/.test(name + item)) { invalid(); }
        lines.push(`${entry.field}.${name}=${item}`);
      }
    }
  }
  const text = new TextEncoder().encode(lines.map(line => `${line}\n`).join(''));
  if (text.length >= HOST_SETTINGS_BYTES) { fail('Host settings text exceeds its block.'); }
  const bytes = new Uint8Array(HOST_SETTINGS_BYTES);
  bytes.set(text);
  return { words, bytes };
};

export const validateHostSettings = encoded => {
  if (!(encoded?.words instanceof Uint32Array) || encoded.words.length !== HOST_SETTINGS_WORDS ||
      !(encoded.bytes instanceof Uint8Array) || encoded.bytes.length !== HOST_SETTINGS_BYTES) {
    fail('Invalid host settings.');
  }
  if (encoded.words[SCHEMA] !== HOST_SETTINGS_SCHEMA) { fail('Host settings schema mismatch.'); }
  return encoded;
};

export const createHostSettingsView = (buffer, byteOffset) => {
  if (!(buffer instanceof SharedArrayBuffer)) { fail('Host settings require shared memory.'); }
  if (!Number.isSafeInteger(byteOffset) || byteOffset <= 0 || byteOffset % 4 || byteOffset > buffer.byteLength - HOST_SETTINGS_SPAN) {
    fail('Host settings span is out of bounds.');
  }
  return { words: new Uint32Array(buffer, byteOffset, HOST_SETTINGS_WORDS),
    bytes: new Uint8Array(buffer, byteOffset + HOST_SETTINGS_WORDS * 4, HOST_SETTINGS_BYTES) };
};

export const writeHostSettings = ({ words, bytes }, encoded, { attempts = ATTEMPTS } = {}) => {
  validateHostSettings(encoded);
  let revision;
  for (let n = 0; ; ++n) {
    revision = Atomics.load(words, REVISION);
    if (!(revision & 1) && Atomics.compareExchange(words, REVISION, revision, (revision + 1) >>> 0) === revision) { break; }
    if (n === attempts) { fail('Host settings block stayed locked.'); }
  }
  for (let n = 1; n < HOST_SETTINGS_WORDS; ++n) { Atomics.store(words, n, encoded.words[n]); }
  for (let n = 0; n < HOST_SETTINGS_BYTES; ++n) {
    if (Atomics.load(bytes, n) !== encoded.bytes[n]) { Atomics.store(bytes, n, encoded.bytes[n]); }
  }
  const next = (revision + 2) >>> 0;
  Atomics.store(words, REVISION, next);
  return next;
};

export const readHostSettings = ({ words, bytes }, { attempts = ATTEMPTS } = {}) => {
  for (let n = 0; n < attempts; ++n) {
    const revision = Atomics.load(words, REVISION);
    if (revision & 1) { continue; }
    const copy = { revision, words: Uint32Array.from(words), bytes: Uint8Array.from(bytes) };
    if (Atomics.load(words, REVISION) === revision) { return copy; }
  }
  return undefined;
};

// computed: values the page works out (the frame's shape), merged over the settings; refresh writes them again
export const createHostSettingsWriter = ({ buffer, byteOffset, settings, computed = () => ({}), onWrite = () => {} }) => {
  if (typeof settings?.snapshot !== 'function' || typeof settings.subscribe !== 'function') { fail('Host settings writer requires settings.'); }
  const view = createHostSettingsView(buffer, byteOffset);
  let revision;
  const write = () => {
    revision = writeHostSettings(view, encodeHostSettings({ ...settings.snapshot(), ...computed() }));
    onWrite(revision);
  };
  write();
  const unsubscribe = settings.subscribe(name => { if (keys.has(name)) { write(); } });
  return Object.freeze({ dispose: unsubscribe, refresh: write, get revision() { return revision; } });
};

// hook-graphics.h kHookStatusWords: fog latched by the last presented frame, optional effects left undrawn and the
// frame's fog passes
export const readHookStatus = words => {
  const floats = new Float32Array(Uint32Array.from(words.subarray(0, 3)).buffer);
  return { fog: { slope: floats[0], offset: floats[1], amount: floats[2], passes: Atomics.load(words, 4), ramps: Atomics.load(words, 5) },
    unsupportedEffects: Atomics.load(words, 3), drawnEffects: Atomics.load(words, 6) };
};
