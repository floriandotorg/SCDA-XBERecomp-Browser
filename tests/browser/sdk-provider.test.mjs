import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { createBootSession } from '../../src/browser/boot-session.mjs';
import { createIsoMailbox } from '../../src/browser/iso-mailbox.mjs';

const directory = resolve('out/sdk-provider-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/image_symbols.h`, '#pragma once\n#define SYM_XNetStartup 00F11000\n#define SYM_WSAStartup 00F12000\n#define SYM_Direct3D_CreateDevice 00F13000\n');
writeFileSync(`${directory}/sdk-fixture.h`, `#pragma once
#include "cpu/x86_ops.h"
#define SDK_FIXTURE_D3D_ADDRESS 0x00F13000
#define SDK_FIXTURE_D3D sub_00F13000
#define SDK_FIXTURE_D3D_ORIGINAL __imp__sub_00F13000
X86_FUNC(SDK_FIXTURE_D3D);
X86_FUNC(SDK_FIXTURE_D3D_ORIGINAL);
#define SDK_FIXTURE_XNET_ADDRESS 0x00F11000
#define SDK_FIXTURE_WSA_ADDRESS 0x00F12000
#define SDK_FIXTURE_XNET sub_00F11000
#define SDK_FIXTURE_WSA sub_00F12000
#define SDK_FIXTURE_XNET_ORIGINAL __imp__sub_00F11000
#define SDK_FIXTURE_WSA_ORIGINAL __imp__sub_00F12000
X86_FUNC(SDK_FIXTURE_XNET);
X86_FUNC(SDK_FIXTURE_WSA);
X86_FUNC(SDK_FIXTURE_XNET_ORIGINAL);
X86_FUNC(SDK_FIXTURE_WSA_ORIGINAL);
`);
writeFileSync(`${directory}/original.cpp`, `#include "sdk-fixture.h"
void SdkFixtureOriginal(X86Context&, uint8_t*, uint32_t);
X86_FUNC(__imp__sub_00F11000) { SdkFixtureOriginal(ctx, base, 1); }
X86_FUNC(__imp__sub_00F12000) { SdkFixtureOriginal(ctx, base, 2); }
X86_FUNC(__imp__sub_00F13000) { SdkFixtureOriginal(ctx, base, 3); }
X86_WEAK_FUNC(sub_00F13000, __imp__sub_00F13000)
X86_WEAK_FUNC(sub_00F11000, __imp__sub_00F11000)
X86_WEAK_FUNC(sub_00F12000, __imp__sub_00F12000)
`);
writeFileSync(`${directory}/run.mjs`, "import create from './sdk.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} transparent SDK entries retain original ABI and physical trap with owned bounded observation`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'sdk.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const env = { ...process.env, TMPDIR: directory };
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread',
      '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', `-I${directory}`, '-Isrc', 'tests/browser/sdk-provider.cpp',
      `${directory}/original.cpp`, 'src/browser/sdk-provider.cpp', 'src/browser/image.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output],
    { encoding: 'utf8', timeout: 120000, env });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000, env });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /sdk-provider-contracts-ok/);
  });
}

const sessionFixture = () => {
  const identity = 'ab'.repeat(32);
  const ownership = new Int32Array(new SharedArrayBuffer(4));
  Atomics.store(ownership, 0, 1);
  const messages = [];
  const module = {
    HEAPU8: new Uint8Array(new SharedArrayBuffer(128)), _boot_build_identity: () => 32, _boot_build_identity_size: () => 64,
    _mount_disc: () => 1, _image_bind: () => 1, _image_bound: () => 1, _image_declare_import: () => 1,
    _image_coverage_count: () => 1, _image_coverage_slot: () => 0x10000, _image_coverage_ordinal: () => 44,
    _image_coverage_target: () => 0xfe0000b0, _image_coverage_kind: () => 3, _image_execute: () => 0,
    _image_entered: () => 1, _image_returned: () => 1, _image_thread_id: () => 20, _image_tls_callbacks: () => 0,
    _image_fault_address: () => 44, _image_fault_ordinal: () => 44, _image_fault_message: () => 0,
    _image_fault_message_size: () => 0, _image_register: () => 0, _image_progress_address: () => 4,
    _image_entry_mapped: () => 1, _image_active_workers: () => 0, _image_backing: () => 4096, _image_reset: () => {},
    _image_disc_reads_words: () => 8, _image_disc_reads_word: () => 0,
    _image_audio_info: field => [48000, 6, 48000, 0, 0, 0, 0, 0, 0, 0][field],
    _image_graphics_checkpoint: () => 0,
    _image_graphics_info: () => 0, _image_graphics_event_word: () => 0, _image_graphics_resource_word: () => 0,
  };
  module.HEAPU8.set(new TextEncoder().encode(identity), 32);
  const options = { create: async () => module, prepare: async () => ({}), postMessage: (message) => messages.push(message),
    manifest: { identity, ranges: {}, kernelExports: [{ ordinal: 44, kind: 'function', name: 'HalGetInterruptVector' }] } };
  const data = { port: { close: () => {} }, mailbox: createIsoMailbox(), ownership, generation: 1, size: 4096 };
  return { options, data, module, messages };
};

test('actual boot session result carries distinct bounded SDK entries without changing physical coverage or outcome', async () => {
  const { options, data, module, messages } = sessionFixture();
  const records = [
    [9, 1, 2, 0, 1, 0x120000, 0x88, 0x20100, 0xabcdef, 1, 1, 0, 0xffffffff, ...Array(18).fill(0)],
    [10, 1, 3, 0, 2, 0x130000, 0x8c, 0x20200, 0x123456, 2, 0, 0, 0x202, 0x30000, ...Array(17).fill(0)],
  ];
  module._image_sdk_trace_count = () => records.length;
  module._image_sdk_trace_word = (index, field) => records[index][field] | 0;
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'UNSUPPORTED_EXPORT');
  assert.equal(messages[0].ordinal, 44);
  assert.equal(messages[0].coverage[0].resolution, 'function-trap');
  assert.deepEqual(messages[0].kernelCalls, []);
  assert.deepEqual(messages[0].sdkCalls, [
    { sequence: '4294967305', workerId: '2', id: 1, name: 'XNetStartup', behavior: 'Unavailable', fsBase: 0x120000, threadHandle: 0x88, esp: 0x20100, returnAddress: 0xabcdef, arguments: [0xffffffff] },
    { sequence: '4294967306', workerId: '3', id: 2, name: 'WSAStartup', behavior: 'Original', fsBase: 0x130000, threadHandle: 0x8c, esp: 0x20200, returnAddress: 0x123456, arguments: [0x202, 0x30000] },
  ]);
});

test('device trace preserves raw presentation and six scalar arguments', async () => {
  const { options, data, module, messages } = sessionFixture();
  const words = [1, 0, 1, 0, 3, 1, 2, 3, 4, 6, 0, 13, 0, 1, 2, 3, 4, 5,
    0, 0, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17];
  module._image_sdk_trace_count = () => 1;
  module._image_sdk_trace_word = (index, field) => words[field];
  await createBootSession(options)({ data });
  assert.deepEqual(messages[0].sdkCalls[0].arguments, [0, 1, 2, 3, 4, 5]);
  assert.deepEqual(messages[0].sdkCalls[0].presentation, { width: 0, height: 0, format: 7, count: 8, autoDepth: 13, depthFmt: 14, flags: 15, interval: 17, raw: [0, 0, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17] });
});

test('diagnostic CPU device admission remains distinct from original physical initialization', async () => {
  const { options, data, module, messages } = sessionFixture();
  const words = [1, 0, 1, 0, 3, 1, 2, 3, 4, 6, 2, 13, 0, 1, 0, 64, 0x1000, 0x2000,
    640, 480, 18, 1, ...Array(9).fill(0)];
  module._image_sdk_trace_count = () => 1;
  module._image_sdk_trace_word = (index, field) => words[field];
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'UNSUPPORTED_EXPORT');
  assert.equal(messages[0].sdkCalls[0].behavior, 'Diagnostic');
  assert.equal(messages[0].sdkCalls[0].presentation.width, 640);
  assert.equal(messages[0].ordinal, 44);
});

test('SDK diagnostic decoding rejects unbounded malformed or unidentified entries', async () => {
  for (const mutation of [
    (module) => { module._image_sdk_trace_count = () => 65; },
    (module) => { module._image_sdk_trace_count = () => -1; },
    (module) => { module._image_sdk_trace_count = () => 0.5; },
    (module) => { delete module._image_sdk_trace_count; },
    (module) => { delete module._image_sdk_trace_word; },
    (module, words) => { words[4] = 4; },
    (module, words) => { words[9] = 2; },
    (module, words) => { words[11] = 13; },
    (module, words) => { words[13] = 1; },
    (module, words) => { words[18] = 1; },
    (module, words) => { words[4] = 3; words[9] = 6; words[11] = 13; words[10] = 1; },
    (module, words) => { words[0] = 0; },
    (module, words) => { words[2] = 0; },
    (module, words) => { words[10] = 2; },
    (module, words) => { words[4] = 2; words[9] = 2; words[10] = 1; },
  ]) {
    const { options, data, module, messages } = sessionFixture();
    const words = [1, 0, 1, 0, 1, 0x120000, 0x88, 0x20100, 0xabcdef, 1, ...Array(21).fill(0)];
    module._image_sdk_trace_count = () => 1;
    module._image_sdk_trace_word = (index, field) => words[field];
    mutation(module, words);
    await createBootSession(options)({ data });
    assert.equal(messages[0].code, 'SDK_TRACE');
    assert.equal(messages[0].sdkCalls, undefined);
    assert.equal(messages[0].coverage, undefined);
  }
});

test('retirement while reading SDK entries suppresses the final guest report', async () => {
  const { options, data, module, messages } = sessionFixture();
  module._image_sdk_trace_count = () => 1;
  module._image_sdk_trace_word = (index, field) => {
    Atomics.store(data.ownership, 0, 2);
    return [1, 0, 1, 0, 1, 0, 0, 0, 0, 1, ...Array(21).fill(0)][field];
  };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'CANCELLED');
  assert.equal(messages[0].sdkCalls, undefined);
});
