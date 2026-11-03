import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { createBootSession } from '../../src/browser/boot-session.mjs';
import { createIsoMailbox } from '../../src/browser/iso-mailbox.mjs';

const directory = resolve('out/menu-launch-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './launch.mjs'; await create();\n");
test('host launch protocol matches native printable path bounds and preserves opaque data', async () => {
  const boundary = async (path, hasData) => {
    const identity = 'a1'.repeat(32);
    const encoded = new TextEncoder().encode(path);
    const ownership = new Int32Array(new SharedArrayBuffer(4));
    ownership[0] = 1;
    const messages = [];
    const module = {
      HEAPU8: new Uint8Array(new SharedArrayBuffer(128)),
      _boot_build_identity: () => 32, _boot_build_identity_size: () => 64,
      _mount_disc: () => 1, _image_bind: () => 1, _image_reset: () => {},
      _image_coverage_count: () => 0, _image_progress_address: () => 4,
      _image_execute: () => 0, _image_fault_message: () => 0, _image_fault_message_size: () => 0,
      _image_entered: () => 1, _image_returned: () => 0, _image_thread_id: () => 16,
      _image_tls_callbacks: () => 0, _image_fault_address: () => 0, _image_register: () => 0,
      _image_fault_ordinal: () => 0, _image_entry_mapped: () => 1, _image_bound: () => 1,
      _image_active_workers: () => 0, _image_backing: () => 128,
      _image_sdk_trace_count: () => 0, _image_sdk_trace_word: () => 0,
      _image_disc_reads_words: () => 8, _image_disc_reads_word: () => 0,
      _image_graphics_info: () => 0, _image_graphics_event_word: () => 0, _image_graphics_resource_word: () => 0,
      _image_audio_info: (field) => [48000, 6, 48000, 0, 0, 0, 0, 0, 0, 0][field],
      _image_launch_word: (field, n) => field === 0 ? encoded.length : field === 1 ? encoded[n] : field === 2 ? Number(hasData) : n & 255,
    };
    module.HEAPU8.set(new TextEncoder().encode(identity), 32);
    const session = createBootSession({
      create: async () => module, prepare: async () => ({}),
      manifest: { identity, ranges: {}, kernelExports: [] }, postMessage: (message) => messages.push(message),
    });
    await session({ data: { port: { close: () => {} }, mailbox: createIsoMailbox(), ownership, generation: 1, size: 4096 } });
    assert.equal(messages.length, 1);
    return messages[0];
  };
  const path = `D:\\${'a'.repeat(245)}\\default.xbe`;
  assert.equal(path.length, 260);
  for (const hasData of [false, true]) {
    const result = await boundary(path, hasData);
    assert.equal(result.code, 'LAUNCH_BOUNDARY_UNCLASSIFIED');
    assert.equal(result.ok, false);
    assert.deepEqual(result.launchRequest, {
      path, hasData, data: hasData ? Uint8Array.from({ length: 3072 }, (_, n) => n & 255) : undefined,
    });
  }
  for (const invalid of [`a${path}`, 'D:\\bad\n\\default.xbe', 'D:\\bad\u0080\\default.xbe']) {
    assert.equal((await boundary(invalid, false)).code, 'LAUNCH_PROTOCOL');
  }
});

for (const platform of ['native', 'wasm']) {
  test(`${platform} launch boundary preserves ABI and owned data`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'launch.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=134217728', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', '-Igenerated/game', 'tests/browser/menu-launch.cpp', 'src/browser/menu-launch.cpp', 'src/browser/image.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /menu-launch-contracts-ok/);
  });
}
