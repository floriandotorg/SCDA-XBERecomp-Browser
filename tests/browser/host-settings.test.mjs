import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const load = async () => existsSync('src/browser/host-settings.mjs') ? import('../../src/browser/host-settings.mjs') : {};
const pascal = field => field.split('_').map(part => part[0].toUpperCase() + part.slice(1)).join('');

test('C++ indices match the single host settings schema', async () => {
  const { HOST_SETTINGS_FIELDS, HOST_SETTINGS_SCHEMA, HOST_SETTINGS_WORDS, HOST_SETTINGS_BYTES } = await load();
  assert.ok(Array.isArray(HOST_SETTINGS_FIELDS));
  const header = readFileSync('src/browser/host-settings.h', 'utf8');
  const constant = name => Number(new RegExp(`constexpr uint32_t ${name} = (\\d+);`).exec(header)?.[1]);
  assert.equal(constant('kSchema'), HOST_SETTINGS_SCHEMA);
  assert.equal(constant('kWords'), HOST_SETTINGS_WORDS);
  assert.equal(constant('kBytes'), HOST_SETTINGS_BYTES);
  const declared = [...header.matchAll(/constexpr uint32_t k(\w+)(Bit|Word) = (\d+);/g)]
    .filter(([, name]) => !['Revision', 'Schema', 'Flags'].includes(name))
    .map(([, name, kind, value]) => `${name}:${kind}:${value}`).sort();
  const schema = HOST_SETTINGS_FIELDS.filter(entry => !['text', 'map', 'host'].includes(entry.type))
    .map(entry => `${pascal(entry.field)}:${entry.type === 'bool' ? 'Bit' : 'Word'}:${entry.type === 'bool' ? entry.bit : entry.word}`).sort();
  assert.deepEqual(declared, schema);
  for (const entry of HOST_SETTINGS_FIELDS.filter(item => item.type !== 'host' && !item.computed)) { assert.match(readFileSync('src/app/app.h', 'utf8'), new RegExp(`\\b${entry.field}\\b`)); }
  const keys = new Set(HOST_SETTINGS_FIELDS.map(entry => entry.key));
  const { SETTINGS } = await import('../../src/browser/settings.mjs');
  for (const key of new Set(HOST_SETTINGS_FIELDS.filter(entry => !entry.computed).map(entry => entry.key))) { assert.ok(Object.hasOwn(SETTINGS, key), key); }
});

test('encoder places flags, scalars and text lines', async () => {
  const { encodeHostSettings, validateHostSettings, HOST_SETTINGS_SCHEMA } = await load();
  const encoded = encodeHostSettings({ skipIntros: true, pauseOnFocusLoss: false });
  assert.equal(encoded.words.length, 64);
  assert.equal(encoded.words[0], 0);
  assert.equal(encoded.words[1], HOST_SETTINGS_SCHEMA);
  assert.equal(encoded.words[2], 1);
  assert.equal(encodeHostSettings({ skipIntros: false, pauseOnFocusLoss: true }).words[2], 2);
  assert.equal(encoded.bytes.length, 4096);
  assert.equal(validateHostSettings(encoded), encoded);
  assert.throws(() => validateHostSettings({ words: new Uint32Array(64), bytes: new Uint8Array(4096) }), /schema/);
  assert.throws(() => validateHostSettings({ words: [], bytes: new Uint8Array(4096) }), /host settings/);
  assert.throws(() => encodeHostSettings({ skipIntros: 'yes', pauseOnFocusLoss: true }), /skipIntros/);
});

const fixture = (offset = 8) => {
  const buffer = new SharedArrayBuffer(offset + 64 * 4 + 4096 + 8);
  const listeners = new Set();
  const values = { skipIntros: false, pauseOnFocusLoss: true, volume: 100 };
  const settings = {
    snapshot: () => ({ ...values }),
    subscribe: listener => { listeners.add(listener); return () => listeners.delete(listener); },
    set: (name, value) => { values[name] = value; for (const listener of listeners) { listener(name, value); } },
    listeners,
  };
  return { buffer, offset, settings, words: new Uint32Array(buffer, offset, 64) };
};

test('writer publishes under an even revision and follows setting changes', async () => {
  const { createHostSettingsWriter, readHostSettings, createHostSettingsView } = await load();
  const f = fixture();
  const writes = [];
  const writer = createHostSettingsWriter({ buffer: f.buffer, byteOffset: f.offset, settings: f.settings, onWrite: revision => writes.push(revision) });
  assert.equal(f.words[0], 2);
  assert.equal(f.words[2], 2);
  f.settings.set('skipIntros', true);
  assert.equal(f.words[0], 4);
  assert.equal(f.words[2], 3);
  f.settings.set('developmentStart', true);
  assert.equal(f.words[0], 4, 'a setting outside the schema rewrote the block');
  assert.deepEqual(writes, [2, 4]);
  assert.equal(writer.revision, 4);
  const read = readHostSettings(createHostSettingsView(f.buffer, f.offset));
  assert.equal(read.revision, 4);
  assert.equal(read.words[2], 3);
  writer.dispose();
  assert.equal(f.settings.listeners.size, 0);
  f.settings.set('skipIntros', false);
  assert.equal(f.words[0], 4);
});

test('torn writes: readers retry while a writer holds the block and writers wait their turn', async () => {
  const { createHostSettingsView, readHostSettings, writeHostSettings, encodeHostSettings } = await load();
  const f = fixture();
  const view = createHostSettingsView(f.buffer, f.offset);
  Atomics.store(f.words, 0, 7);
  assert.equal(readHostSettings(view, { attempts: 4 }), undefined);
  assert.throws(() => writeHostSettings(view, encodeHostSettings(f.settings.snapshot()), { attempts: 4 }), /locked/);
  let reads = 0;
  Atomics.store(f.words, 0, 8);
  const original = Atomics.load;
  Atomics.load = (array, index) => (array.buffer === f.buffer && array.length === 64 && index === 0 && reads++ === 1) ? (Atomics.store(f.words, 0, 10), original(array, index)) : original(array, index);
  try { assert.equal(readHostSettings(view).revision, 10); }
  finally { Atomics.load = original; }
  assert.ok(reads >= 3, 'a changed revision was not retried');
  assert.equal(writeHostSettings(view, encodeHostSettings(f.settings.snapshot())), 12);
  assert.throws(() => createHostSettingsView(f.buffer, 2), /span/);
  assert.throws(() => createHostSettingsView(new ArrayBuffer(8192), 0), /shared/);
});

const directory = resolve('out/host-settings-tests');
mkdirSync(directory, { recursive: true });
for (const platform of ['native', 'wasm']) {
  test(`${platform} app::Get readers never see a torn host settings write`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'settings.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=67108864', '-sPTHREAD_POOL_SIZE=4', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=thread'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-Isrc', 'tests/browser/host-settings.cpp',
      ...(existsSync('src/browser/host-settings.cpp') ? ['src/browser/host-settings.cpp'] : []), ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    writeFileSync(`${directory}/run.mjs`, "import create from './settings.mjs'; await create();\n");
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 60000 });
    assert.equal(run.status, 0, run.stdout + run.stderr);
    assert.match(run.stdout, /host-settings-contracts-ok/);
  });
}
