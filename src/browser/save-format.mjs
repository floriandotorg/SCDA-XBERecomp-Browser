// A saved progress container: magic, container version, header length, JSON header, the storage image the game runtime
// publishes (src/browser/storage-publisher.h) and a SHA-256 of everything before it.
export const SAVE_FORMAT = Object.freeze({
  magic: 'SCDASAVE', version: 1, schema: 1, maximumHeaderBytes: 65536, maximumPayloadBytes: 48 * 1024 * 1024,
  get maximumPackageBytes() { return 16 + this.maximumHeaderBytes + this.maximumPayloadBytes + 32; },
  kinds: ['slot', 'export', 'backup'],
});
// the bounds of the story profile namespace in src/browser/boot-wasm.cpp
const NAMESPACE = Object.freeze({ capacity: 32 * 1024 * 1024, entries: 256, unit: 16384 });

const fail = (code, message) => { throw Object.assign(new Error(message), { code }); };
const digest = async bytes => new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
const sameBytes = (left, right) => left.length === right.length && left.every((value, n) => value === right[n]);
const gameIdentity = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);

export const encodeSaveContainer = async ({ header, payload }) => {
  if (!(payload instanceof Uint8Array) || payload.length > SAVE_FORMAT.maximumPayloadBytes) { fail('TOO_LARGE', 'Saved progress is too large.'); }
  const complete = { schema: SAVE_FORMAT.schema, savedAt: new Date().toISOString(), ...header, payloadBytes: payload.length };
  const json = new TextEncoder().encode(JSON.stringify(complete));
  if (json.length > SAVE_FORMAT.maximumHeaderBytes) { fail('TOO_LARGE', 'Saved progress header is too large.'); }
  const bytes = new Uint8Array(16 + json.length + payload.length + 32);
  const view = new DataView(bytes.buffer);
  bytes.set(new TextEncoder().encode(SAVE_FORMAT.magic));
  view.setUint32(8, SAVE_FORMAT.version, true);
  view.setUint32(12, json.length, true);
  bytes.set(json, 16);
  bytes.set(payload, 16 + json.length);
  bytes.set(await digest(bytes.subarray(0, bytes.length - 32)), bytes.length - 32);
  return bytes;
};

export const decodeSaveContainer = async (bytes, { game }) => {
  if (!(bytes instanceof Uint8Array)) { fail('FORMAT', 'Not a saved progress file.'); }
  if (bytes.length > SAVE_FORMAT.maximumPackageBytes) { fail('TOO_LARGE', 'The file is larger than any saved progress.'); }
  if (bytes.length < 16 || new TextDecoder().decode(bytes.subarray(0, 8)) !== SAVE_FORMAT.magic) { fail('FORMAT', 'Not a saved progress file.'); }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(8, true) !== SAVE_FORMAT.version) { fail('VERSION', 'The saved progress was made by a different version of this page.'); }
  const headerBytes = view.getUint32(12, true);
  if (!headerBytes || headerBytes > SAVE_FORMAT.maximumHeaderBytes) { fail('FORMAT', 'Not a saved progress file.'); }
  if (bytes.length < 16 + headerBytes + 32) { fail('DAMAGED', 'The saved progress is incomplete.'); }
  if (!sameBytes(await digest(bytes.subarray(0, bytes.length - 32)), bytes.subarray(bytes.length - 32))) {
    fail('DAMAGED', 'The saved progress is damaged.');
  }
  let header;
  try { header = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes.subarray(16, 16 + headerBytes))); }
  catch { fail('DAMAGED', 'The saved progress header is damaged.'); }
  if (!header || typeof header !== 'object' || !gameIdentity(header.game)) { fail('DAMAGED', 'The saved progress header is damaged.'); }
  if (header.game !== game) { fail('GAME', 'The saved progress belongs to a different game executable.'); }
  if (!Number.isSafeInteger(header.schema) || header.schema < 1) { fail('DAMAGED', 'The saved progress header is damaged.'); }
  if (header.schema > SAVE_FORMAT.schema) { fail('VERSION', 'The saved progress was made by a newer version of this page.'); }
  const payload = bytes.slice(16 + headerBytes, bytes.length - 32);
  if (header.payloadBytes !== payload.length || !SAVE_FORMAT.kinds.includes(header.kind) ||
      !Number.isSafeInteger(header.sequence) || header.sequence < 1 || typeof header.savedAt !== 'string') {
    fail('DAMAGED', 'The saved progress header is damaged.');
  }
  return { header, payload };
};

const canonicalPath = path => path.length > 0 && path.length <= 240 && /^[\x20-\x7e]+$/.test(path) && !/[a-z/:*?"]/.test(path) &&
  path.split('\\').every(part => part && part !== '.' && part !== '..');

// The file listing of a storage image, validated the way the game runtime restores it.
export const parseStorageImage = payload => {
  const damaged = reason => fail('DAMAGED', `The saved progress is damaged: ${reason}.`);
  if (!(payload instanceof Uint8Array)) { damaged('no image'); }
  const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
  let offset = 0;
  const take = width => {
    if (payload.length - offset < width) { damaged('truncated'); }
    const value = width === 8 ? view.getBigUint64(offset, true) : width === 4 ? view.getUint32(offset, true) : width === 2 ? view.getUint16(offset, true) : view.getUint8(offset);
    offset += width;
    return value;
  };
  const text = length => {
    if (payload.length - offset < length) { damaged('truncated'); }
    const value = String.fromCharCode(...payload.subarray(offset, offset + length));
    offset += length;
    return value;
  };
  if (text(8) !== 'SCDAUSER' || take(4) !== 1) { damaged('unknown image'); }
  offset += 16;
  if (!/^[0-9A-F]{20}$/.test(text(20))) { damaged('console serial'); }
  if (text(8) !== 'SCDATFSI' || take(4) !== 1) { damaged('unknown file image'); }
  const count = take(4);
  if (count >= NAMESPACE.entries) { damaged('too many files'); }
  const directories = new Set(['']);
  const files = [];
  let allocated = NAMESPACE.unit, previous = '', bytes = 0;
  for (let n = 0; n < count; ++n) {
    const length = take(2);
    if (!length || length > 240) { damaged('path length'); }
    const path = text(length);
    if (!canonicalPath(path) || path <= previous) { damaged('path'); }
    const separator = path.lastIndexOf('\\');
    if (!directories.has(separator < 0 ? '' : path.slice(0, separator))) { damaged('orphaned file'); }
    const directory = take(1);
    const attributes = take(4);
    const times = [take(8), take(8), take(8), take(8)];
    const reservation = take(8), size = take(8);
    if (directory > 1 || (attributes & ~0xb7) || Boolean(attributes & 0x10) !== Boolean(directory)) { damaged('attributes'); }
    if (directory && (size || reservation)) { damaged('directory size'); }
    if (reservation % BigInt(NAMESPACE.unit) || reservation > BigInt(NAMESPACE.capacity) || size > BigInt(NAMESPACE.capacity)) { damaged('size'); }
    const allocation = directory ? NAMESPACE.unit : Math.max(Math.ceil(Number(size) / NAMESPACE.unit) * NAMESPACE.unit, Number(reservation));
    if (allocation > NAMESPACE.capacity - allocated) { damaged('capacity'); }
    for (let filled = 0; filled < Number(size);) {
      const zeros = take(4), literal = take(4);
      if (!(zeros + literal) || zeros + literal > Number(size) - filled) { damaged('file chunks'); }
      if (payload.length - offset < literal) { damaged('truncated'); }
      offset += literal;
      filled += zeros + literal;
    }
    allocated += allocation;
    bytes += Number(size);
    if (directory) { directories.add(path); }
    files.push({ path, directory: Boolean(directory), size: Number(size), modified: Number((times[2] - 116444736000000000n) / 10000n) });
    previous = path;
  }
  if (offset !== payload.length) { damaged('trailing bytes'); }
  return { files, bytes };
};
