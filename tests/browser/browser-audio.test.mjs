import { test } from 'node:test';
import assert from 'node:assert/strict';

const fixture = async ({ load = Promise.resolve(), autoplay = true, volume = 1 } = {}) => {
  const { createBrowserAudio } = await import('../../src/browser/browser-audio.mjs');
  const buffer = new SharedArrayBuffer(32 + 16384 * 6 * 4);
  const header = new Uint32Array(buffer, 0, 8);
  const document = new EventTarget(), window = new EventTarget();
  document.hasFocus = () => true;
  document.hidden = false;
  const events = [];
  const gain = { gain: { value: 1 }, connect: () => events.push('gain-connect'), disconnect: () => events.push('gain-disconnect') };
  let elapsed = 0, tick, gestured = autoplay;
  class Context extends EventTarget {
    constructor(options) { super(); this.sampleRate = options.sampleRate; this.state = 'suspended'; this.destination = {}; this.currentTime = 0; }
    audioWorklet = { addModule: () => load };
    createGain = () => gain;
    resume = async () => { events.push('resume'); if (gestured) { this.state = 'running'; } };
    close = async () => { events.push('close'); this.state = 'closed'; };
  }
  const nodes = [];
  const createNode = () => {
    const node = new EventTarget();
    node.connect = () => events.push('connect');
    node.disconnect = () => events.push('disconnect');
    node.port = { postMessage: message => events.push(`port-${message}`) };
    nodes.push(node);
    return node;
  };
  const audio = createBrowserAudio({ buffer, byteOffset: 0, samplesOffset: 32, document, window, volume,
    AudioContext: Context, createNode, now: () => elapsed,
    schedule: callback => { tick = callback; return 1; }, cancel: () => { tick = undefined; } });
  return { audio, header, events, nodes, get node() { return nodes.at(-1); }, gain, window, document,
    gesture: async type => { gestured = true; window.dispatchEvent(new Event(type)); await audio.enable(); },
    advance: value => { elapsed += value; tick?.(); } };
};

test('audio starts without a user control and focus loss flushes queued sound', async () => {
  const f = await fixture();
  await f.audio.enable();
  assert.deepEqual(f.events, ['resume', 'connect', 'gain-connect']);
  assert.equal(f.header[3], 1);
  f.header[0] = 10;
  f.window.dispatchEvent(new Event('blur'));
  assert.equal(f.header[3], 0);
  assert.equal(f.header[1], 10);
  f.window.dispatchEvent(new Event('focus'));
  assert.equal(f.header[3], 1);
  await f.audio.dispose();
  assert.equal(f.header[3], 0);
  assert.ok(f.events.includes('disconnect') && f.events.includes('close'));
});

test('a blocked autoplay starts on the next user gesture', async () => {
  const f = await fixture({ autoplay: false });
  await f.audio.enable();
  assert.equal(f.header[3], 0);
  await f.gesture('pointerdown');
  assert.equal(f.header[3], 1);
  await f.audio.dispose();
});

test('volume is applied at start and while playing', async () => {
  const f = await fixture({ volume: 0.25 });
  await f.audio.enable();
  assert.equal(f.gain.gain.value, 0.25);
  f.audio.setVolume(0.5);
  assert.equal(f.gain.gain.value, 0.5);
  f.audio.setVolume(7);
  assert.equal(f.gain.gain.value, 1);
  await f.audio.dispose();
});

test('retiring during worklet startup cannot connect or replay old samples', async () => {
  let loaded;
  const f = await fixture({ load: new Promise(resolve => { loaded = resolve; }) });
  await Promise.resolve();
  await f.audio.dispose();
  loaded();
  await f.audio.enable();
  assert.equal(f.header[3], 0);
  assert.equal(f.events.includes('connect'), false);
});

test('a failed worklet stops the ring and recovers on the next gesture', async () => {
  const f = await fixture();
  await f.audio.enable();
  f.node.dispatchEvent(new Event('processorerror'));
  assert.equal(f.header[3], 0);
  await f.gesture('keydown');
  assert.equal(f.header[3], 1);
  await f.audio.dispose();
});

test('a running context with a stalled device clock stops accepting stale audio', async () => {
  const f = await fixture();
  await f.audio.enable();
  f.advance(3000);
  assert.equal(f.header[3], 0);
  await f.audio.dispose();
});

test('mono output down-mixes to one channel on the gain node, stereo restores it', async () => {
  const f = await fixture();
  await f.audio.enable();
  f.audio.setOutput('mono');
  assert.deepEqual([f.gain.channelCount, f.gain.channelCountMode, f.gain.channelInterpretation], [1, 'explicit', 'speakers']);
  f.audio.setOutput('stereo');
  assert.deepEqual([f.gain.channelCount, f.gain.channelCountMode], [2, 'max']);
});

test('a replaced worklet stops its processor so only one node reads the ring', async () => {
  const f = await fixture();
  await f.audio.enable();
  const first = f.node;
  first.dispatchEvent(new Event('processorerror'));
  assert.ok(f.events.includes('port-stop'));
  await f.gesture('keydown');
  assert.equal(f.nodes.length, 2);
  assert.notEqual(f.node, first);
  assert.equal(f.header[3], 1);
  await f.audio.dispose();
});

test('a corrupt ring position recovers in place without replacing the worklet', async () => {
  const f = await fixture();
  await f.audio.enable();
  f.header[0] = 20000;
  f.header[7] = 1;
  f.advance(1000);
  assert.equal(f.header[7], 0);
  assert.equal(f.header[3], 1);
  assert.equal(f.header[1], 20000);
  assert.equal(f.nodes.length, 1);
  assert.equal(f.events.includes('disconnect'), false);
  await f.audio.dispose();
});
