// The page's end of src/browser/host-messages.h: it drains the guest to host ring and fills the host to guest one.
export const HOST_MESSAGES = Object.freeze({ entries: 32, payloadBytes: 256, headerWords: 8,
  set: 1, unset: 2, quit: 3, console: 4, output: 5 });
const ENTRY_WORDS = 2 + HOST_MESSAGES.payloadBytes / 4;
export const HOST_MESSAGES_WORDS = HOST_MESSAGES.headerWords + 2 * HOST_MESSAGES.entries * ENTRY_WORDS;
const GUEST_HEAD = 0, GUEST_TAIL = 1, HOST_HEAD = 2, HOST_TAIL = 3, DROPPED = 4;
const GUEST_RING = HOST_MESSAGES.headerWords, HOST_RING = GUEST_RING + HOST_MESSAGES.entries * ENTRY_WORDS;

const fail = message => { throw Object.assign(new Error(message), { code: 'HOST_MESSAGES' }); };

export const createHostMessages = ({ buffer, byteOffset }) => {
  if (!(buffer instanceof SharedArrayBuffer)) { fail('Host messages require shared memory.'); }
  if (!Number.isSafeInteger(byteOffset) || byteOffset <= 0 || byteOffset % 4 || byteOffset > buffer.byteLength - HOST_MESSAGES_WORDS * 4) {
    fail('Host messages span is out of bounds.');
  }
  const words = new Uint32Array(buffer, byteOffset, HOST_MESSAGES_WORDS);
  const decoder = new TextDecoder(), encoder = new TextEncoder();
  const take = () => {
    const messages = [];
    let tail = Atomics.load(words, GUEST_TAIL);
    const head = Atomics.load(words, GUEST_HEAD);
    if (((head - tail) >>> 0) > HOST_MESSAGES.entries) { tail = (head - HOST_MESSAGES.entries) >>> 0; }
    while (tail !== head) {
      const entry = GUEST_RING + (tail % HOST_MESSAGES.entries) * ENTRY_WORDS;
      const kind = Atomics.load(words, entry), length = Atomics.load(words, entry + 1);
      if (length <= HOST_MESSAGES.payloadBytes) {
        const bytes = new Uint8Array(length);
        for (let n = 0; n < length; ++n) { bytes[n] = Atomics.load(words, entry + 2 + (n >> 2)) >>> ((n & 3) * 8) & 0xff; }
        messages.push({ kind, text: decoder.decode(bytes) });
      }
      tail = (tail + 1) >>> 0;
    }
    Atomics.store(words, GUEST_TAIL, tail);
    return messages;
  };
  const post = (kind, text) => {
    const bytes = encoder.encode(String(text));
    const head = Atomics.load(words, HOST_HEAD);
    if (bytes.length > HOST_MESSAGES.payloadBytes || ((head - Atomics.load(words, HOST_TAIL)) >>> 0) >= HOST_MESSAGES.entries) { return false; }
    const entry = HOST_RING + (head % HOST_MESSAGES.entries) * ENTRY_WORDS;
    Atomics.store(words, entry, kind);
    Atomics.store(words, entry + 1, bytes.length);
    for (let n = 0; n < HOST_MESSAGES.payloadBytes / 4; ++n) {
      let word = 0;
      for (let byte = 0; byte < 4; ++byte) { word |= (bytes[n * 4 + byte] ?? 0) << (byte * 8); }
      Atomics.store(words, entry + 2 + n, word >>> 0);
    }
    Atomics.store(words, HOST_HEAD, (head + 1) >>> 0);
    return true;
  };
  return Object.freeze({ take, post, get dropped() { return Atomics.load(words, DROPPED); } });
};

// a guest set or unset entry ("keys.fire=G", "volume=80", "padButtons.jump") applied to settings.mjs, which persists it
export const applyHostMessage = (settings, { kind, text }) => {
  if (kind !== HOST_MESSAGES.set && kind !== HOST_MESSAGES.unset) { return false; }
  const equals = text.indexOf('=');
  const path = kind === HOST_MESSAGES.set ? text.slice(0, equals) : text;
  if (kind === HOST_MESSAGES.set && equals < 0) { return false; }
  const value = kind === HOST_MESSAGES.set ? text.slice(equals + 1) : undefined;
  const dot = path.indexOf('.');
  const name = dot < 0 ? path : path.slice(0, dot);
  const current = settings.get(name);
  if (current === undefined) { return false; }
  if (dot >= 0) {
    if (!current || typeof current !== 'object') { return false; }
    const next = { ...current };
    if (value === undefined) { delete next[path.slice(dot + 1)]; } else { next[path.slice(dot + 1)] = value; }
    settings.set(name, next);
    return true;
  }
  if (value === undefined) { return false; }
  settings.set(name, typeof current === 'boolean' ? value === 'true' || value === '1' : value);
  return true;
};
