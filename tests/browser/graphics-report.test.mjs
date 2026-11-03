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
    (x) => { x.events[0][2] = 19; },
    (x) => { x.events[0][3] = NaN; },
    (x) => { x.resources[0][5] = 2; },
    (x) => { x.resources[0][2] = 0x4000001; },
  ]) {
    const value = fixture();
    update(value);
    assert.throws(() => readGraphicsDiagnostic(value.module), { code: 'GRAPHICS_PROTOCOL' });
  }
});

test('graphics protocol failures identify the bounded field without losing its scalar value', () => {
  for (const [field, value, label] of [[3, 65, 'eventCount'], [4, 7, 'resourceCount'], [5, 16385, 'commandWords']]) {
    const { module, info } = fixture();
    info[field] = value;
    assert.throws(() => readGraphicsDiagnostic(module), (error) =>
      error.code === 'GRAPHICS_PROTOCOL' && error.message.includes(`${label}=${value}`));
  }
  const { module } = fixture();
  module._image_graphics_rendering = () => 1;
  module._image_renderer_info = (field) => [257, 0, 0, 0, 5, 0, 4, 0, 3, 0][field];
  assert.throws(() => readGraphicsDiagnostic(module), (error) =>
    error.code === 'GRAPHICS_PROTOCOL' && error.message.includes('renderer.pipelineCount=257'));
});

test('presented frames may advance past the last explicit GPU completion checkpoint', () => {
  const { module } = fixture();
  const counters = [2, 10, 4096, 0, 100, 0, 18, 0, 33, 0];
  module._image_graphics_rendering = () => 1;
  module._image_renderer_info = (field) => counters[field];
  const report = readGraphicsDiagnostic(module);
  assert.equal(report.submittedCommands, '100');
  assert.equal(report.completedCommands, '18');
  assert.equal(report.renderedFrames, '33');
  counters[8] = 101;
  assert.throws(() => readGraphicsDiagnostic(module), { code: 'GRAPHICS_PROTOCOL' });
  counters[8] = 33;
  counters[6] = 101;
  assert.throws(() => readGraphicsDiagnostic(module), { code: 'GRAPHICS_PROTOCOL' });
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

test('persisted display diagnostics describe completed bounded readback without exposing pixels', () => {
  const { module, events } = fixture();
  module._image_graphics_rendering = () => 1;
  module._image_renderer_info = (field) => [2, 10, 4096, 0, 100, 0, 100, 0, 33, 0][field];
  const words = [0x50001, 18 << 8, 0x271df27f, 1228800];
  module._image_persisted_display_word = (field) => words[field];
  events[1][2] = 18;
  const report = readGraphicsDiagnostic(module);
  assert.equal(report.events[1].request, 'PersistDisplay');
  assert.deepEqual(report.persistedDisplay, { common: 0x50001, format: 18 << 8, size: 0x271df27f, bytes: 1228800 });
  assert.equal(report.persistedDisplay.pixels, undefined);
  words[3] = 0x800001;
  assert.throws(() => readGraphicsDiagnostic(module), { code: 'GRAPHICS_PROTOCOL' });
  words.fill(0);
  assert.equal(readGraphicsDiagnostic(module).persistedDisplay, undefined);
  words[0] = 1;
  assert.throws(() => readGraphicsDiagnostic(module), { code: 'GRAPHICS_PROTOCOL' });
});
