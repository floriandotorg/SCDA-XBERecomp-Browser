import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readMenuStateDiagnostic, readMenuFileDiagnostics } from '../../src/browser/menu-state-report.mjs';

test('menu file diagnostics own bounded original and resolved names without altering statuses', () => {
  const heap = new Uint8Array(new SharedArrayBuffer(1024));
  heap.set(new TextEncoder().encode('D:\\PUBLIC.DAT\0'), 16);
  heap.set(new TextEncoder().encode('PUBLIC.DAT\0'), 256);
  const module = {
    HEAPU8: heap,
    _image_file_open_count: () => 1,
    _image_file_open_word: (_, field) => [0xc0000033, 0xfffffffd, 1, 0x20, 0x40100080, 0x80, 512, 1][field],
    _image_file_open_path: (_, field) => field === 0 ? 16 : 256,
  };
  assert.deepEqual(readMenuFileDiagnostics(module), [
    { status: 0xc0000033, root: 0xfffffffd, disposition: 1, options: 0x20, access: 0x40100080, attributes: 0x80, allocationBytes: '4294967808', originalPath: 'D:\\PUBLIC.DAT', resolvedPath: 'PUBLIC.DAT' },
  ]);
  heap[16] = 0;
  assert.equal(readMenuFileDiagnostics({}), undefined);
  assert.throws(() => readMenuFileDiagnostics({ ...module, _image_file_open_count: () => 33 }), { code: 'GRAPHICS_PROTOCOL' });
  assert.throws(() => readMenuFileDiagnostics({ ...module, _image_file_open_word: () => NaN }), { code: 'GRAPHICS_PROTOCOL' });
  assert.throws(() => readMenuFileDiagnostics({ ...module, _image_file_open_path: () => 0 }), { code: 'GRAPHICS_PROTOCOL' });
  assert.throws(() => readMenuFileDiagnostics({ ...module, HEAPU8: new Uint8Array(16) }), { code: 'GRAPHICS_PROTOCOL' });
  assert.throws(() => readMenuFileDiagnostics({ ...module, HEAPU8: new Uint8Array(1024).fill(1) }), { code: 'GRAPHICS_PROTOCOL' });
  assert.throws(() => readMenuFileDiagnostics({ ...module, HEAPU8: undefined }), { code: 'GRAPHICS_PROTOCOL' });
});

const fixture = () => {
  const groups = new Map([
    [1, Array(166).fill(0)], [2, Array(2048).fill(0)], [3, Array(4 * 32).fill(0)],
    [4, [3, 7]], [5, [1, 2]], [6, [0, 1, 2, 3]], [7, Array(60).fill(0)],
    [8, Array(192 * 4).fill(0)], [9, Array(16 * 4).fill(0)],
    [10, [1, 2, 3, 4, 0, 0, 640, 480, 0, 0x3f800000]],
    [11, Array.from({ length: 24 }, (_, n) => n)], [12, Array(32).fill(0)], [13, [1, 2, 3]],
    [14, Array(8).fill(0)],
  ]);
  const calls = [];
  return { groups, calls, module: {
    _image_graphics_capture_word: (group, index) => {
      calls.push([group, index]);
      const words = groups.get(group) ?? [];
      return index === 0xffffffff ? words.length : words[index];
    },
  } };
};

test('menu state capture preserves owned shader words, signed words and bounded constants', () => {
  const { module, groups } = fixture();
  groups.get(6)[2] = -1;
  const report = readMenuStateDiagnostic(module);
  assert.equal(report.vertex.handle, 3);
  assert.equal(report.pixel.handle, 7);
  assert.deepEqual(report.vertex.declaration, [1, 2]);
  assert.deepEqual(report.vertex.program, [0, 1, 0xffffffff, 3]);
  assert.equal(report.vertex.constants.length, 768);
  assert.equal(report.pixel.constants.length, 64);
  assert.deepEqual(report.presentation, [1, 2, 3, 4, 0, 0, 640, 480, 0, 0x3f800000]);
  assert.equal(report.renderState.length, 166);
  assert.deepEqual(report.textures[0], [0, 1, 2, 3, 4, 5]);
  assert.equal(report.streams.length, 16);
  assert.deepEqual(report.index, [1, 2, 3]);
  assert.equal(report.targets.length, 2);
});

test('legacy boot artifacts without capture exports do not claim state evidence', () => {
  assert.equal(readMenuStateDiagnostic({}), undefined);
});

test('menu state capture rejects malformed, unaligned and excessive payloads before publication', () => {
  for (const [group, words] of [
    [1, Array(167).fill(0)], [2, Array(2049).fill(0)], [3, Array(129).fill(0)],
    [4, [1]], [5, Array(1025).fill(0)], [6, [1]], [6, Array(548).fill(0)],
    [7, Array(61).fill(0)], [8, Array(769).fill(0)], [9, Array(65).fill(0)],
    [10, []], [11, Array(25).fill(0)], [12, []], [13, [1]], [14, []], [5, [NaN]], [5, [0x100000000]], [5, [-0x80000001]],
  ]) {
    const { module, groups } = fixture();
    groups.set(group, words);
    assert.throws(() => readMenuStateDiagnostic(module), { code: 'GRAPHICS_PROTOCOL' });
  }
  const { module } = fixture();
  module._image_graphics_capture_word = () => 0xffffffff;
  assert.throws(() => readMenuStateDiagnostic(module), { code: 'GRAPHICS_PROTOCOL' });
});
