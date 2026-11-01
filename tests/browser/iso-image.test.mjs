import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { prepareDiscImage } from '../../src/browser/iso-image.mjs';

const setup = () => {
  const bytes = new Uint8Array((1 << 20) + 3).fill(7);
  const manifest = {
    schema: 1, image: 'default.xbe', xbeSize: bytes.length,
    xbeSha256: createHash('sha256').update(bytes).digest('hex'),
    requiredStoryFiles: [{ path: 'story.dat', minBytes: 1 }],
  };
  const layout = { base: 0x10000, end: 0x13000, entry: 0x11000 };
  const reads = [];
  const calls = [];
  const module = {
    HEAPU8: new Uint8Array(1),
    _image_allocate: (size) => {
      calls.push('allocate');
      module.HEAPU8 = new Uint8Array(size + 16);
      return 16;
    },
    _image_prepare: (...args) => {
      calls.push(args);
      assert.deepEqual(module.HEAPU8.subarray(16), bytes);
      return 1;
    },
    _image_reset: () => { calls.push('reset'); },
    _image_sections: () => 2,
    _image_imports: () => 3,
    _image_tls_bytes: () => 20,
    _image_backing: () => 1234,
  };
  const disc = {
    find: (path) => ({ size: path === 'default.xbe' ? bytes.length : 1, dir: false }),
    read: (path, position, length) => {
      reads.push([path, position, length]);
      return bytes.slice(position, position + length);
    },
  };
  return { bytes, manifest, layout, reads, calls, module, disc };
};

test('disc preparation copies only authenticated returned bytes into the current Wasm heap', async () => {
  const source = setup();
  const pending = prepareDiscImage(source);
  source.layout.entry = 0x12000;
  const result = await pending;
  assert.deepEqual(source.calls, ['allocate', [0x10000, 0x13000, 0x11000]]);
  assert.deepEqual(source.reads.map((read) => read.slice(1)), [[0, 1 << 20], [1 << 20, 3]]);
  assert.deepEqual(result, { sections: 2, imports: 3, tlsBytes: 20, backingBytes: 1234 });
});

test('invalid identity, layout and cancellation cannot allocate or prepare guest memory', async () => {
  for (const mode of ['identity', 'layout', 'cancel']) {
    const source = setup();
    const controller = new AbortController();
    if (mode === 'identity') { source.manifest.xbeSha256 = '0'.repeat(64); }
    if (mode === 'layout') { source.layout.end = source.layout.base; }
    if (mode === 'cancel') {
      const read = source.disc.read;
      source.disc.read = (...args) => { controller.abort(); return read(...args); };
    }
    await assert.rejects(prepareDiscImage({ ...source, signal: controller.signal }));
    assert.deepEqual(source.calls, []);
    if (mode === 'layout') { assert.deepEqual(source.reads, []); }
  }
});

test('failed allocation, parser rejection and copy traps release the unpublished image', async () => {
  for (const mode of ['allocate', 'parse', 'copy']) {
    const source = setup();
    if (mode === 'allocate') { source.module._image_allocate = () => 0; }
    if (mode === 'parse') { source.module._image_prepare = () => 0; }
    if (mode === 'copy') { source.module._image_allocate = () => 16; }
    await assert.rejects(prepareDiscImage(source));
    assert.equal(source.calls.at(-1), 'reset');
  }
});
