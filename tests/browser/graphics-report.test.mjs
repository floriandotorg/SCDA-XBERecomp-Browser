import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readGraphicsDiagnostic } from '../../src/browser/boot-session.mjs';

const fixture = () => {
  const info = [1, 65, 0, 2, 1, 128];
  const events = [[64, 0, 16, 640, 480, 0, 0, 0, 0], [65, 0, 6, 0x80100000, 0, 0, 0, 0, 0]];
  const resources = [[0x80100000, 0x80101000, 1228800, 18 << 8, 0, 0]];
  return {
    info, events, resources,
    module: {
      _image_graphics_info: (field) => info[field],
      _image_graphics_event_word: (index, field) => events[index][field],
      _image_graphics_resource_word: (index, field) => resources[index][field],
    },
  };
};

test('diagnostic graphics distinguishes CPU effects, request retention and rendered pixels', () => {
  const { module } = fixture();
  const report = readGraphicsDiagnostic(module);
  assert.equal(report.mode, 'diagnostic-cpu');
  assert.equal(report.initialized, true);
  assert.equal(report.renderedFrames, 0);
  assert.equal(report.totalEvents, '65');
  assert.equal(report.droppedEvents, '63');
  assert.equal(report.commandWords, 128);
  assert.equal(report.events[0].request, 'CpuClear');
  assert.equal(report.events[1].request, 'Present');
  assert.equal(report.resources[0].unavailable, false);
});

test('graphics decoding bounds every count, field and ordering before publishing a report', () => {
  for (const update of [
    (x) => { x.module._image_graphics_info = undefined; },
    (x) => { x.info[0] = 2; },
    (x) => { x.info[1] = 1; },
    (x) => { x.info[3] = 65; },
    (x) => { x.info[4] = 7; },
    (x) => { x.info[5] = 16385; },
    (x) => { x.events[1][0] = 64; },
    (x) => { x.events[0][2] = 18; },
    (x) => { x.events[0][3] = NaN; },
    (x) => { x.resources[0][5] = 2; },
    (x) => { x.resources[0][2] = 0x4000001; },
  ]) {
    const value = fixture();
    update(value);
    assert.throws(() => readGraphicsDiagnostic(value.module), { code: 'GRAPHICS_PROTOCOL' });
  }
});

test('graphics event sequence retains full precision and accepts signed Wasm words', () => {
  const { module, info, events } = fixture();
  info[1] = -1;
  info[2] = 0x200000;
  events[0][0] = -2;
  events[0][1] = 0x200000;
  events[1][0] = -1;
  events[1][1] = 0x200000;
  const report = readGraphicsDiagnostic(module);
  assert.equal(report.totalEvents, '9007203549708287');
  assert.equal(report.events[1].sequence, report.totalEvents);
});
