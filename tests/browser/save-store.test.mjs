import { test } from 'node:test';
import assert from 'node:assert/strict';
import { decodeSaveContainer, encodeSaveContainer, parseStorageImage, SAVE_FORMAT } from '../../src/browser/save-format.mjs';
import { createSaveStore } from '../../src/browser/save-store.mjs';

const game = 'c742d61c9b8d8c1c4f886a4a4a69ffb65ab3961e0283dcad33bfcfa963885143';
const otherGame = 'a'.repeat(64);

const encodeImage = (nodes, { serial = '0123456789ABCDEF0A1B' } = {}) => {
  const bytes = [];
  const put = (value, width) => { for (let n = 0; n < width; ++n) { bytes.push(Number((BigInt(value) >> BigInt(8 * n)) & 0xffn)); } };
  const text = value => { for (const char of value) { bytes.push(char.charCodeAt(0)); } };
  text('SCDAUSER');
  put(1, 4);
  for (let n = 0; n < 16; ++n) { bytes.push(n * 3); }
  text(serial);
  text('SCDATFSI');
  put(1, 4);
  put(nodes.length, 4);
  for (const { path, directory = false, attributes = directory ? 0x10 : 0x80, reservation = 0, data = [] } of nodes) {
    put(path.length, 2);
    text(path);
    put(directory ? 1 : 0, 1);
    put(attributes, 4);
    for (let n = 0; n < 4; ++n) { put(132000000000000000n + BigInt(n), 8); }
    put(reservation, 8);
    put(data.length, 8);
    if (data.length) {
      put(0, 4);
      put(data.length, 4);
      bytes.push(...data);
    }
  }
  return Uint8Array.from(bytes);
};

const progress = (marker) => encodeImage([
  { path: 'UDATA', directory: true },
  { path: 'UDATA\\4D530064', directory: true },
  { path: 'UDATA\\4D530064\\PROFILE', data: [marker, 1, 2, 3] },
]);

const memoryDirectory = () => {
  const files = new Map();
  const faults = [];
  return {
    files, faults,
    read: async name => files.get(name)?.slice(),
    write: async (name, bytes) => {
      const fault = faults.shift();
      if (fault?.partial !== undefined) { files.set(name, bytes.slice(0, fault.partial)); }
      if (fault?.drop) { return; }
      if (fault?.error) { throw fault.error; }
      files.set(name, bytes.slice());
    },
    remove: async name => { files.delete(name); },
    list: async () => [...files.keys()],
  };
};

const rejectsWith = async (promise, code) => {
  await assert.rejects(promise, error => { assert.equal(error.code, code, error.message); return true; });
};

test('containers round trip and name the reason they are rejected', async () => {
  const payload = progress(7);
  const bytes = await encodeSaveContainer({ header: { game, kind: 'slot', sequence: 3 }, payload });
  const decoded = await decodeSaveContainer(bytes, { game });
  assert.deepEqual(decoded.payload, payload);
  assert.equal(decoded.header.sequence, 3);
  assert.equal(decoded.header.schema, SAVE_FORMAT.schema);
  await rejectsWith(decodeSaveContainer(bytes, { game: otherGame }), 'GAME');
  await rejectsWith(decodeSaveContainer(new TextEncoder().encode('not a save file at all'), { game }), 'FORMAT');
  await rejectsWith(decodeSaveContainer(bytes.slice(0, bytes.length - 1), { game }), 'DAMAGED');
  const flipped = bytes.slice();
  flipped[flipped.length - 1] ^= 1;
  await rejectsWith(decodeSaveContainer(flipped, { game }), 'DAMAGED');
  const newerContainer = bytes.slice();
  newerContainer[8] = 2;
  await rejectsWith(decodeSaveContainer(newerContainer, { game }), 'VERSION');
  const newerSchema = await encodeSaveContainer({ header: { game, kind: 'slot', sequence: 1, schema: SAVE_FORMAT.schema + 1 }, payload });
  await rejectsWith(decodeSaveContainer(newerSchema, { game }), 'VERSION');
  const huge = bytes.slice();
  new DataView(huge.buffer).setUint32(12, SAVE_FORMAT.maximumHeaderBytes + 1, true);
  await rejectsWith(decodeSaveContainer(huge, { game }), 'FORMAT');
  await rejectsWith(decodeSaveContainer(new Uint8Array(SAVE_FORMAT.maximumPackageBytes + 1), { game }), 'TOO_LARGE');
});

test('storage images list their files and reject paths outside the namespace', () => {
  const listing = parseStorageImage(progress(1));
  assert.deepEqual(listing.files.map(({ path, directory, size }) => [path, directory, size]),
    [['UDATA', true, 0], ['UDATA\\4D530064', true, 0], ['UDATA\\4D530064\\PROFILE', false, 4]]);
  assert.equal(listing.bytes, 4);
  for (const [nodes, reason] of [
    [[{ path: '..\\ESCAPE', data: [1] }], 'parent path'],
    [[{ path: 'C:\\FILE', data: [1] }], 'drive path'],
    [[{ path: 'lower', data: [1] }], 'lower case path'],
    [[{ path: 'MISSING\\FILE', data: [1] }], 'orphan'],
    [[{ path: 'B', data: [1] }, { path: 'A', data: [1] }], 'unordered'],
    [[{ path: 'DIR', directory: true, attributes: 0x80 }], 'directory attribute'],
    [[{ path: 'FILE', reservation: 100, data: [1] }], 'unaligned reservation'],
    [[{ path: 'FILE', reservation: 64 * 1024 * 1024 }], 'reservation over capacity'],
    [Array.from({ length: 256 }, (_, n) => ({ path: `F${1000 + n}`, data: [1] })), 'too many entries'],
  ]) {
    assert.throws(() => parseStorageImage(encodeImage(nodes)), error => error.code === 'DAMAGED', reason);
  }
  assert.throws(() => parseStorageImage(encodeImage([], { serial: '0123456789abcdef0a1b' })), error => error.code === 'DAMAGED');
  const truncated = progress(1);
  assert.throws(() => parseStorageImage(truncated.slice(0, truncated.length - 1)), error => error.code === 'DAMAGED');
  const claimed = progress(1);
  new DataView(claimed.buffer).setBigUint64(claimed.length - 4 - 8 - 8, 1n << 40n, true);
  assert.throws(() => parseStorageImage(claimed), error => error.code === 'DAMAGED');
  const chunked = (zeros, literal) => {
    const image = progress(1);
    const view = new DataView(image.buffer);
    view.setUint32(image.length - 4 - 8, zeros, true);
    view.setUint32(image.length - 4 - 4, literal, true);
    return image;
  };
  assert.equal(parseStorageImage(chunked(0, 4)).bytes, 4);
  for (const [zeros, literal] of [[0, 0], [1, 4], [0xffffffff, 0], [0, 5]]) {
    assert.throws(() => parseStorageImage(chunked(zeros, literal)), error => error.code === 'DAMAGED', `${zeros} ${literal}`);
  }
});

test('commits alternate slots and a reopened store loads the newest accepted progress', async () => {
  const directory = memoryDirectory();
  const store = createSaveStore({ directory, game });
  assert.equal((await store.load()).state, 'empty');
  const first = await store.commit(progress(1));
  const second = await store.commit(progress(2));
  assert.equal(second.sequence, first.sequence + 1);
  assert.deepEqual([...directory.files.keys()].sort(), ['slot-a.scdasave', 'slot-b.scdasave']);
  const reopened = await createSaveStore({ directory, game }).load();
  assert.equal(reopened.state, 'loaded');
  assert.equal(reopened.sequence, second.sequence);
  assert.deepEqual(reopened.payload, progress(2));
});

test('a failed or unverifiable write never replaces the accepted progress', async () => {
  for (const [fault, code] of [
    [{ partial: 10, error: Object.assign(new Error('full'), { name: 'QuotaExceededError' }) }, 'QUOTA'],
    [{ partial: 40, error: new Error('torn') }, 'IO'],
    [{ drop: true }, 'WRITE_VERIFY'],
    [{ error: Object.assign(new Error('locked'), { name: 'NoModificationAllowedError' }) }, 'LOCKED'],
  ]) {
    const directory = memoryDirectory();
    const store = createSaveStore({ directory, game });
    await store.load();
    await store.commit(progress(1));
    await store.commit(progress(2));
    directory.faults.push(fault);
    await rejectsWith(store.commit(progress(3)), code);
    const reopened = await createSaveStore({ directory, game }).load();
    assert.equal(reopened.state, 'loaded', code);
    assert.deepEqual(reopened.payload, progress(2), code);
    const retried = await store.commit(progress(4));
    assert.deepEqual((await createSaveStore({ directory, game }).load()).payload, progress(4), code);
    assert.equal(retried.sequence, 3, code);
  }
});

test('damaged progress is set aside, never deleted, and does not block new progress', async () => {
  const directory = memoryDirectory();
  const store = createSaveStore({ directory, game });
  await store.load();
  await store.commit(progress(1));
  await store.commit(progress(2));
  for (const name of ['slot-a.scdasave', 'slot-b.scdasave']) { directory.files.get(name)[30] ^= 0xff; }
  const damagedBytes = directory.files.get('slot-a.scdasave').slice();
  const reopened = createSaveStore({ directory, game });
  const loaded = await reopened.load();
  assert.equal(loaded.state, 'damaged');
  assert.equal(loaded.payload, undefined);
  const kept = [...directory.files.keys()].filter(name => name.startsWith('damaged-'));
  assert.equal(kept.length, 2);
  assert.ok(kept.some(name => directory.files.get(name).every((value, n) => value === damagedBytes[n])));
  await reopened.commit(progress(5));
  assert.equal([...directory.files.keys()].filter(name => name.startsWith('damaged-')).length, 2);
  assert.deepEqual((await createSaveStore({ directory, game }).load()).payload, progress(5));

  const single = memoryDirectory();
  const partial = createSaveStore({ directory: single, game });
  await partial.load();
  await partial.commit(progress(1));
  await partial.commit(progress(2));
  single.files.get('slot-b.scdasave')[40] ^= 0xff;
  const recovered = await createSaveStore({ directory: single, game }).load();
  assert.equal(recovered.state, 'loaded');
  assert.deepEqual(recovered.payload, progress(1));
});

test('progress from a newer schema is reported and never overwritten', async () => {
  const directory = memoryDirectory();
  const newer = await encodeSaveContainer({ header: { game, kind: 'slot', sequence: 9, schema: SAVE_FORMAT.schema + 1 }, payload: progress(1) });
  directory.files.set('slot-a.scdasave', newer.slice());
  const store = createSaveStore({ directory, game });
  assert.equal((await store.load()).state, 'incompatible');
  await rejectsWith(store.commit(progress(2)), 'INCOMPATIBLE');
  await rejectsWith(store.importPackage(await encodeSaveContainer({ header: { game, kind: 'export', sequence: 1 }, payload: progress(3) })), 'INCOMPATIBLE');
  assert.deepEqual(directory.files.get('slot-a.scdasave'), newer);
  assert.deepEqual([...directory.files.keys()], ['slot-a.scdasave']);
});

test('export and import move progress and settings between isolated stores with a backup', async () => {
  const source = createSaveStore({ directory: memoryDirectory(), game });
  await source.load();
  await rejectsWith(source.exportPackage({ settings: {} }), 'EMPTY');
  await source.commit(progress(1));
  await source.commit(progress(2));
  const exported = await source.exportPackage({ settings: { volume: 40 } });
  const described = await source.inspectPackage(exported);
  assert.equal(described.files.length, 3);
  assert.deepEqual(described.settings, { volume: 40 });

  const directory = memoryDirectory();
  const target = createSaveStore({ directory, game });
  await target.load();
  await target.commit(progress(8));
  const imported = await target.importPackage(exported);
  assert.deepEqual(imported.settings, { volume: 40 });
  assert.ok(directory.files.has('backup.scdasave'));
  assert.deepEqual((await createSaveStore({ directory, game }).load()).payload, progress(2));
  assert.equal(target.status().backup, true);
  await target.restoreBackup();
  assert.deepEqual((await createSaveStore({ directory, game }).load()).payload, progress(8));

  const before = new Map([...directory.files].map(([name, bytes]) => [name, bytes.slice()]));
  const foreign = await encodeSaveContainer({ header: { game: otherGame, kind: 'export', sequence: 1 }, payload: progress(4) });
  await rejectsWith(target.importPackage(foreign), 'GAME');
  const escaping = await encodeSaveContainer({ header: { game, kind: 'export', sequence: 1 }, payload: encodeImage([{ path: '..\\X', data: [1] }]) });
  await rejectsWith(target.importPackage(escaping), 'DAMAGED');
  await rejectsWith(target.importPackage(exported.slice(0, 20)), 'DAMAGED');
  assert.deepEqual(new Map([...directory.files].map(([name, bytes]) => [name, bytes.slice()])), before);

  const fresh = memoryDirectory();
  const empty = createSaveStore({ directory: fresh, game });
  await empty.load();
  await empty.importPackage(exported);
  assert.ok(!fresh.files.has('backup.scdasave'));
  assert.deepEqual((await createSaveStore({ directory: fresh, game }).load()).payload, progress(2));
});
