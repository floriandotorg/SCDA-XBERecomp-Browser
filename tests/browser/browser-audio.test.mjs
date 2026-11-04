import { test } from 'node:test';
import assert from 'node:assert/strict';

const fixture = async (load = Promise.resolve()) => {
  const { createBrowserAudio } = await import('../../src/browser/browser-audio.mjs');
  const buffer = new SharedArrayBuffer(32 + 16384 * 6 * 4);
  const header = new Uint32Array(buffer, 0, 8);
  const button = new EventTarget(), document = new EventTarget(), window = new EventTarget(), status = {};
  document.hasFocus = () => true;
  document.hidden = false;
  const events = [];
  let elapsed = 0, tick;
  class Context extends EventTarget {
    constructor(options) { super(); this.sampleRate = options.sampleRate; this.state = 'suspended'; this.destination = {}; this.currentTime = 0; }
    audioWorklet = { addModule: () => load };
    resume = async () => { events.push('resume'); this.state = 'running'; };
    close = async () => { events.push('close'); this.state = 'closed'; };
  }
  const node = new EventTarget();
  node.connect = () => events.push('connect');
  node.disconnect = () => events.push('disconnect');
  const audio = createBrowserAudio({ buffer, byteOffset: 0, samplesOffset: 32, button, status, document, window,
    AudioContext: Context, createNode: () => node, now: () => elapsed,
    schedule: callback => { tick = callback; return 1; }, cancel: () => { tick = undefined; } });
  return { audio, header, button, status, events, node, window, document,
    advance: value => { elapsed += value; tick?.(); } };
};

test('audio requires explicit activation and focus loss flushes queued sound', async () => {
  const f = await fixture();
  assert.equal(f.header[3], 0);
  assert.equal(f.button.disabled, false);
  await f.audio.enable();
  assert.deepEqual(f.events, ['resume', 'connect']);
  assert.equal(f.header[3], 1);
  f.header[0] = 10;
  f.window.dispatchEvent(new Event('blur'));
  assert.equal(f.header[3], 0);
  assert.equal(f.header[1], 10);
  f.window.dispatchEvent(new Event('focus'));
  assert.equal(f.header[3], 1);
  await f.audio.dispose();
  assert.equal(f.header[3], 0);
  assert.equal(f.button.disabled, true);
  assert.ok(f.events.includes('disconnect') && f.events.includes('close'));
});

test('retiring during worklet startup cannot connect or replay old samples', async () => {
  let loaded;
  const f = await fixture(new Promise(resolve => { loaded = resolve; }));
  const enabling = f.audio.enable();
  await Promise.resolve();
  await f.audio.dispose();
  loaded();
  await enabling;
  assert.equal(f.header[3], 0);
  assert.equal(f.events.includes('connect'), false);
});

test('a failed worklet exposes a retry without leaving the ring active', async () => {
  const f = await fixture();
  await f.audio.enable();
  f.node.dispatchEvent(new Event('processorerror'));
  assert.equal(f.header[3], 0);
  assert.equal(f.button.disabled, false);
  assert.match(f.status.textContent, /retry|enable/i);
  await f.audio.dispose();
});

test('a running context with a stalled device clock stops accepting stale audio and exposes a retry', async () => {
  const f = await fixture();
  await f.audio.enable();
  f.advance(3000);
  assert.equal(f.header[3], 0);
  assert.equal(f.button.disabled, false);
  assert.match(f.status.textContent, /clock stopped/i);
  await f.audio.dispose();
});
