import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readSilentAudioDiagnostic } from '../../src/browser/boot-session.mjs';

const fixture = () => {
  const words = [48000, 6, 48000, 0, 0, 0, 0, 0, 0, 0];
  return { words, module: { _image_audio_info: field => words[field] } };
};

test('silent audio report describes its bounded clocked sink without inventing frames', () => {
  assert.deepEqual(readSilentAudioDiagnostic(fixture().module), {
    mode: 'silent-timed', sampleRate: 48000, channels: 6, capacityFrames: 48000, paused: false,
    submittedFrames: '0', consumedFrames: '0', queuedFrames: '0', audibleFrames: 0,
  });
});

test('silent audio counters preserve uint64 precision and checked queue accounting', () => {
  const { module, words } = fixture();
  const submitted = (1n << 63n) + 517n;
  const consumed = submitted - 123n;
  words[3] = 1;
  for (const [field, value] of [[4, submitted], [6, consumed], [8, 123n]]) {
    words[field] = Number(value & 0xffffffffn);
    words[field + 1] = Number(value >> 32n);
  }
  assert.deepEqual(readSilentAudioDiagnostic(module), {
    mode: 'silent-timed', sampleRate: 48000, channels: 6, capacityFrames: 48000, paused: true,
    submittedFrames: submitted.toString(), consumedFrames: consumed.toString(), queuedFrames: '123', audibleFrames: 0,
  });
});

test('silent audio counters normalize signed Wasm i32 exports without losing bits', () => {
  const { module, words } = fixture();
  words[4] = -1;
  words[5] = -0x80000000;
  words[6] = -2;
  words[7] = -0x80000000;
  words[8] = 1;
  const report = readSilentAudioDiagnostic(module);
  assert.equal(report.submittedFrames, ((1n << 63n) + 0xffffffffn).toString());
  assert.equal(report.consumedFrames, ((1n << 63n) + 0xfffffffen).toString());
  assert.equal(report.queuedFrames, '1');
});

test('missing, malformed and inconsistent silent audio metadata fails explicitly', () => {
  const protocol = error => error.code === 'AUDIO_PROTOCOL';
  assert.throws(() => readSilentAudioDiagnostic({}), protocol);
  for (const [field, value] of [[0, 0], [0, 44100], [1, 2], [2, 0], [2, 48001], [3, 2],
    [4, -1], [5, 2 ** 32], [6, NaN], [7, 0.5], [8, undefined], [9, '0']]) {
    const { module, words } = fixture();
    words[field] = value;
    assert.throws(() => readSilentAudioDiagnostic(module), protocol);
  }
  for (const counters of [[0, 1, 0], [10, 2, 7], [48001, 0, 48001]]) {
    const { module, words } = fixture();
    [words[4], words[6], words[8]] = counters;
    assert.throws(() => readSilentAudioDiagnostic(module), protocol);
  }
});
