import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readMissionDiagnostic } from '../../src/browser/boot-session.mjs';

const fixture = () => {
  const words = [5, 0, 2, 0, 3, 1, 1, 1, 2, 40, 8, 7];
  const module = { _image_mission_count: () => 1, _image_mission_total_word: field => field === 0 ? 5 : 0,
    _image_mission_word: (index, field) => words[field] };
  return { words, module };
};

test('mission diagnostics report bounded pre-dispatch classification without raw strings or downstream success', () => {
  const { words, module } = fixture();
  const result = readMissionDiagnostic(module);
  assert.deepEqual(result, { kind: 'original-dispatch-observation', totalEvents: '5', droppedEvents: '4', events: [
    { sequence: '5', workerId: '2', state: 'Request', requestObserved: true, command: { valid: true, menuPage: 1, mode: 1, difficulty: 2 }, commandLength: 40, mapLength: 8, profileLength: 7 },
  ] });
  words.fill(0);
  assert.equal(result.events[0].commandLength, 40);
  assert.equal(JSON.stringify(result).includes('missionSucceeded'), false);
});

test('syntax validity alone is not a verified original request', () => {
  const { words, module } = fixture();
  for (const state of [0, 1, 2]) {
    words[4] = state;
    assert.equal(readMissionDiagnostic(module).events[0].requestObserved, false);
  }
  assert.equal(readMissionDiagnostic({}), undefined);
});

test('malformed or unbounded mission snapshots fail closed', () => {
  for (const [field, value] of [[0, 0], [2, 0], [4, 4], [5, 2], [6, -1], [9, 1025], [10, 129], [11, 129], [0, NaN]]) {
    const { words, module } = fixture();
    words[field] = value;
    assert.throws(() => readMissionDiagnostic(module), error => error.code === 'MISSION_PROTOCOL');
  }
  const { module } = fixture();
  module._image_mission_count = () => 9;
  assert.throws(() => readMissionDiagnostic(module), error => error.code === 'MISSION_PROTOCOL');
  assert.throws(() => readMissionDiagnostic({ _image_mission_count: () => 0 }), error => error.code === 'MISSION_PROTOCOL');
});
