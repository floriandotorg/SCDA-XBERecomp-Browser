import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createSettings } from '../../src/browser/settings.mjs';

const memory = (initial) => {
  const values = new Map(initial ? [['scda.settings', initial]] : []);
  return { getItem: key => values.get(key) ?? null, setItem: (key, value) => { values.set(key, value); }, values };
};

test('defaults apply and invalid stored values fall back per setting', () => {
  const settings = createSettings({ storage: memory(JSON.stringify({ scaling: 'stretch', volume: 'loud', invertY: true, unknown: 1 })) });
  assert.equal(settings.get('scaling'), 'stretch');
  assert.equal(settings.get('volume'), 100);
  assert.equal(settings.get('invertY'), true);
  assert.equal(settings.get('filter'), 'smooth');
  assert.equal(settings.get('unknown'), undefined);
});

test('unreadable storage keeps defaults', () => {
  assert.equal(createSettings({ storage: memory('{') }).get('scaling'), 'fit');
  assert.equal(createSettings({ storage: { getItem: () => { throw new Error('denied'); }, setItem() { throw new Error('denied'); } } }).get('volume'), 100);
  assert.equal(createSettings({ storage: undefined }).get('developmentStart'), false);
});

test('set validates, clamps, persists and notifies', () => {
  const storage = memory();
  const settings = createSettings({ storage });
  const seen = [];
  const stop = settings.subscribe((name, value) => seen.push([name, value]));
  settings.set('volume', 140);
  settings.set('sensitivity', '2.5');
  settings.set('scaling', 'nonsense');
  settings.set('showFps', 1);
  assert.equal(settings.get('volume'), 100);
  assert.equal(settings.get('sensitivity'), 2.5);
  assert.equal(settings.get('scaling'), 'fit');
  assert.equal(settings.get('showFps'), true);
  assert.deepEqual(seen, [['sensitivity', 2.5], ['showFps', true]]);
  assert.equal(JSON.parse(storage.values.get('scda.settings')).sensitivity, 2.5);
  stop();
  settings.set('volume', 10);
  assert.equal(seen.length, 2);
  assert.throws(() => settings.set('unknown', 1), /unknown/i);
});

test('reset restores defaults and notifies changed values', () => {
  const settings = createSettings({ storage: memory() });
  settings.set('volume', 30);
  const seen = [];
  settings.subscribe((name, value) => seen.push([name, value]));
  settings.reset();
  assert.equal(settings.get('volume'), 100);
  assert.deepEqual(seen, [['volume', 100]]);
});

test('game settings that reach the guest default to the native values and persist', () => {
  const storage = memory();
  const settings = createSettings({ storage });
  assert.equal(settings.get('skipIntros'), false);
  assert.equal(settings.get('pauseOnFocusLoss'), true);
  settings.set('skipIntros', true);
  settings.set('pauseOnFocusLoss', 0);
  const reloaded = createSettings({ storage });
  assert.equal(reloaded.get('skipIntros'), true);
  assert.equal(reloaded.get('pauseOnFocusLoss'), false);
  assert.equal(createSettings({ storage: memory(JSON.stringify({ skipIntros: 'yes' })) }).get('skipIntros'), false);
});

test('binding maps keep known actions and canonical key names, drop the rest, and persist', () => {
  const storage = memory(JSON.stringify({ keys: { fire: 'g,Nonsense', unknown: 'W', jump: '' }, padButtons: { jump: 'b', crouch: 'Z', fire: '', none: 'A' } }));
  const settings = createSettings({ storage });
  assert.deepEqual(settings.get('keys'), { fire: 'G', jump: '' });
  assert.deepEqual(settings.get('padButtons'), { jump: 'B', fire: '' });
  const seen = [];
  settings.subscribe(name => seen.push(name));
  settings.set('keys', { fire: 'G', jump: '' });
  assert.deepEqual(seen, []);
  settings.set('keys', { fire: 'LMB' });
  settings.set('keys', 'nonsense');
  assert.deepEqual(seen, ['keys']);
  assert.deepEqual(createSettings({ storage }).get('keys'), { fire: 'LMB' });
  assert.deepEqual(createSettings({ storage: memory() }).get('padButtons'), {});
});

test('per view sensitivities share the free look range', () => {
  const settings = createSettings({ storage: memory() });
  assert.equal(settings.get('weaponSensitivity'), 1);
  settings.set('sniperSensitivity', 9);
  assert.equal(settings.get('sniperSensitivity'), 5);
  settings.set('weaponSensitivity', 0.04);
  assert.equal(settings.get('weaponSensitivity'), 0.1);
});

test('the simple settings pack normalises, persists and resets', () => {
  const storage = memory();
  const settings = createSettings({ storage });
  assert.deepEqual(['anisotropy', 'brightness', 'contrast', 'frameCap', 'language', 'audioOutput'].map(name => settings.get(name)), [0, 50, 50, 60, 'auto', 'stereo']);
  settings.set('anisotropy', 16);
  settings.set('anisotropy', 3);
  settings.set('brightness', 140);
  settings.set('frameCap', 30);
  settings.set('language', 'fra');
  settings.set('audioOutput', 'mono');
  const reloaded = createSettings({ storage });
  assert.deepEqual(['anisotropy', 'brightness', 'frameCap', 'language', 'audioOutput'].map(name => reloaded.get(name)), [16, 100, 30, 'fra', 'mono']);
  reloaded.reset();
  assert.equal(reloaded.get('frameCap'), 60);
});

test('MSAA offers off and 4x only: 2 and 8 become 4, garbage stays off; SMAA is a switch', () => {
  const settings = createSettings({ storage: memory(JSON.stringify({ msaa: 8, smaa: 1 })) });
  assert.equal(settings.get('msaa'), 4);
  assert.equal(settings.get('smaa'), true);
  settings.set('msaa', 2);
  assert.equal(settings.get('msaa'), 4);
  settings.set('msaa', 0);
  assert.equal(settings.get('msaa'), 0);
  settings.set('msaa', 'nonsense');
  assert.equal(settings.get('msaa'), 0);
  assert.equal(createSettings({ storage: memory(JSON.stringify({ msaa: 'x' })) }).get('msaa'), 0);
});

test('shader model 3.0 renders HDR and tone maps only when asked; HDR output defaults on with a 5x headroom', () => {
  const settings = createSettings({ storage: memory() });
  assert.deepEqual(['shaderModel', 'hdrRendering', 'toneMapping', 'hdrOutput', 'hdrHeadroom'].map(name => settings.get(name)), ['1.1', false, false, true, 5]);
  settings.set('shaderModel', '2.0');
  assert.equal(settings.get('shaderModel'), '1.1');
  settings.set('shaderModel', '3.0');
  settings.set('hdrHeadroom', 12);
  assert.equal(settings.get('shaderModel'), '3.0');
  assert.equal(settings.get('hdrHeadroom'), 10);
  settings.set('hdrHeadroom', 2.3);
  assert.equal(settings.get('hdrHeadroom'), 2.5);
  settings.set('hdrHeadroom', 0);
  assert.equal(settings.get('hdrHeadroom'), 1);
  assert.equal(createSettings({ storage: memory(JSON.stringify({ hdrRendering: 1, toneMapping: 'yes' })) }).get('hdrRendering'), true);
});

test('ambient occlusion is off, SSAO or GTAO, off by default', () => {
  const settings = createSettings({ storage: memory(JSON.stringify({ ambientOcclusion: 'hbao' })) });
  assert.equal(settings.get('ambientOcclusion'), 'off');
  settings.set('ambientOcclusion', 'gtao');
  assert.equal(settings.get('ambientOcclusion'), 'gtao');
  assert.equal(createSettings({ storage: memory(JSON.stringify({ ambientOcclusion: 'ssao' })) }).get('ambientOcclusion'), 'ssao');
});
