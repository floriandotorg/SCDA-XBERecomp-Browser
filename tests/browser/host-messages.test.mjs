import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync } from 'node:fs';
import { HOST_MESSAGES, HOST_MESSAGES_WORDS, applyHostMessage, createHostMessages } from '../../src/browser/host-messages.mjs';

const ENTRY = 2 + HOST_MESSAGES.payloadBytes / 4;
const fixture = () => {
  const buffer = new SharedArrayBuffer(16 + HOST_MESSAGES_WORDS * 4);
  const words = new Uint32Array(buffer, 16, HOST_MESSAGES_WORDS);
  // the guest's Post, as host-messages.cpp writes it
  const guestPost = (kind, text, length) => {
    const bytes = new TextEncoder().encode(text);
    const head = words[0];
    const entry = 8 + (head % 32) * ENTRY;
    words[entry] = kind;
    words[entry + 1] = length ?? bytes.length;
    for (let n = 0; n < bytes.length; ++n) { words[entry + 2 + (n >> 2)] |= bytes[n] << ((n & 3) * 8); }
    words[0] = head + 1;
  };
  return { buffer, words, guestPost, messages: createHostMessages({ buffer, byteOffset: 16 }) };
};

test('the layout matches host-messages.h', () => {
  const header = readFileSync('src/browser/host-messages.h', 'utf8');
  const constant = name => Number(new RegExp(`${name} = (\\d+)`).exec(header)[1]);
  assert.equal(constant('kEntries'), HOST_MESSAGES.entries);
  assert.equal(constant('kPayloadBytes'), HOST_MESSAGES.payloadBytes);
  assert.equal(constant('kHeaderWords'), HOST_MESSAGES.headerWords);
  for (const kind of ['set', 'unset', 'quit', 'console', 'output']) {
    assert.equal(constant(`k${kind[0].toUpperCase()}${kind.slice(1)}`), HOST_MESSAGES[kind]);
  }
});

test('guest entries drain in order across the ring wrap, and a torn length is skipped', () => {
  const f = fixture();
  for (let round = 0; round < 3; ++round) {
    for (let n = 0; n < 20; ++n) { f.guestPost(HOST_MESSAGES.set, `volume=${round * 20 + n}`); }
    const taken = f.messages.take();
    assert.deepEqual(taken.map(entry => entry.text), Array.from({ length: 20 }, (_, n) => `volume=${round * 20 + n}`));
    for (let n = 8; n < HOST_MESSAGES_WORDS; ++n) { f.words[n] = 0; }
  }
  assert.equal(f.words[1], 60);
  f.guestPost(HOST_MESSAGES.quit, '', 4000);
  f.guestPost(HOST_MESSAGES.quit, '');
  assert.deepEqual(f.messages.take(), [{ kind: HOST_MESSAGES.quit, text: '' }]);
  assert.deepEqual(f.messages.take(), []);
});

test('host entries wait for room: a full ring refuses, and so does a payload over 256 bytes', () => {
  const f = fixture();
  for (let n = 0; n < 32; ++n) { assert.equal(f.messages.post(HOST_MESSAGES.console, `say ${n}`), true); }
  assert.equal(f.messages.post(HOST_MESSAGES.console, 'one too many'), false);
  f.words[3] = 1;
  assert.equal(f.messages.post(HOST_MESSAGES.console, 'room again'), true);
  assert.equal(f.words[2], 33);
  const entry = 8 + 32 * ENTRY + 0 * ENTRY;
  assert.equal(f.words[entry + 1], 'room again'.length);
  assert.equal(f.messages.post(HOST_MESSAGES.console, 'x'.repeat(257)), false);
});

test('set and unset entries update settings maps and values', () => {
  const values = { keys: { fire: 'G' }, volume: 100, skipIntros: false };
  const settings = { get: name => values[name], set: (name, value) => { values[name] = value; } };
  assert.ok(applyHostMessage(settings, { kind: HOST_MESSAGES.set, text: 'keys.jump=Space' }));
  assert.deepEqual(values.keys, { fire: 'G', jump: 'Space' });
  assert.ok(applyHostMessage(settings, { kind: HOST_MESSAGES.unset, text: 'keys.fire' }));
  assert.deepEqual(values.keys, { jump: 'Space' });
  assert.ok(applyHostMessage(settings, { kind: HOST_MESSAGES.set, text: 'volume=80' }));
  assert.equal(values.volume, '80');
  assert.ok(applyHostMessage(settings, { kind: HOST_MESSAGES.set, text: 'skipIntros=true' }));
  assert.equal(values.skipIntros, true);
  assert.equal(applyHostMessage(settings, { kind: HOST_MESSAGES.set, text: 'unknown=1' }), false);
  assert.equal(applyHostMessage(settings, { kind: HOST_MESSAGES.quit, text: '' }), false);
});

test('native guest rings post, take, wrap and drop', () => {
  mkdirSync('out/host-messages-tests', { recursive: true });
  const output = 'out/host-messages-tests/host-messages';
  const build = spawnSync('clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-fsanitize=address,undefined', '-Isrc', 'tests/browser/host-messages.cpp',
    'src/browser/host-messages.cpp', '-o', output], { encoding: 'utf8' });
  assert.equal(build.status, 0, build.stderr);
  const run = spawnSync(output, { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stdout + run.stderr);
  assert.match(run.stdout, /host-messages-contracts-ok/);
});
