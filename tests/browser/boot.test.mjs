import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { inventedImage } from './image-fixture.mjs';
import { syntheticDisc } from './iso-wasm-fixture.mjs';
import { createBootSession } from '../../src/browser/boot-session.mjs';
import { createIsoMailbox } from '../../src/browser/iso-mailbox.mjs';

const directory = resolve('out/boot-tests');
mkdirSync(directory, { recursive: true });

test('boot owner pairs retained dispatch and image, rejects missing exports and resets isolated sessions', async () => {
  const identity = 'c3'.repeat(32);
  writeFileSync(`${directory}/build_identity.h`, `#pragma once\n#define BROWSER_BUILD_ID "${identity}"\n`);
  const audioEntries = [
    'IDirectSound_Release', 'IDirectSoundBuffer_Unlock', 'IDirectSoundBuffer_Release', 'DirectSoundUseFullHRTF',
    'CDirectSoundStream_AddRef', 'CDirectSoundStream_Release', 'CDirectSoundStream_GetInfo', 'CDirectSoundStream_Discontinuity',
    'CDirectSoundStream_Flush', 'CDirectSoundStream_GetStatus', 'CDirectSoundStream_Process', 'CDirectSoundStream_Pause',
    'CDirectSoundStream_FlushEx', 'CDirectSoundStream_SetVolume', 'CDirectSoundStream_SetFilter', 'CDirectSoundStream_SetHeadroom',
    'CDirectSoundStream_SetMixBinVolumes', 'IDirectSound_GetSpeakerConfig', 'IDirectSound_SetEffectData', 'IDirectSound_EnableHeadphones',
    'IDirectSound_SetMixBinHeadroom', 'IDirectSoundBuffer_SetVolume', 'IDirectSoundBuffer_SetFilter', 'IDirectSoundBuffer_SetHeadroom',
    'IDirectSoundBuffer_SetMixBinVolumes', 'IDirectSoundBuffer_Play', 'IDirectSoundBuffer_Stop', 'IDirectSoundBuffer_SetLoopRegion',
    'IDirectSoundBuffer_GetStatus', 'IDirectSoundBuffer_GetCurrentPosition', 'IDirectSoundBuffer_SetCurrentPosition', 'IDirectSoundBuffer_Lock',
    'DirectSoundDoWork', 'XAudioDownloadEffectsImage', 'CDirectSoundStream_SetFrequency', 'CDirectSoundStream_SetMixBins',
    'CDirectSoundStream_SetMaxDistance', 'CDirectSoundStream_SetMinDistance', 'CDirectSoundStream_SetPosition', 'CDirectSoundStream_SetVelocity',
    'CDirectSoundStream_SetI3DL2Source', 'IDirectSoundBuffer_SetFrequency', 'IDirectSoundBuffer_SetMixBins', 'IDirectSoundBuffer_SetMaxDistance',
    'IDirectSoundBuffer_SetMinDistance', 'IDirectSoundBuffer_SetPosition', 'IDirectSoundBuffer_SetVelocity', 'IDirectSoundBuffer_SetI3DL2Source',
    'IDirectSoundBuffer_SetBufferData', 'IDirectSound_CommitDeferredSettings', 'IDirectSound_CreateSoundBuffer', 'IDirectSound_SetDopplerFactor',
    'IDirectSound_SetOrientation', 'IDirectSound_SetPosition', 'IDirectSound_SetRolloffFactor', 'IDirectSound_SetVelocity',
    'IDirectSound_SetI3DL2Listener', 'DirectSoundCreate', 'DirectSoundCreateBuffer', 'DirectSoundCreateStream',
  ];
  const audioAddress = (name) => 0x14000 + audioEntries.indexOf(name) * 0x10;
  const audioSymbols = audioEntries.map((name) =>
    `#define SYM_${name} ${audioAddress(name).toString(16).toUpperCase().padStart(8, '0')}\n#define HAS_${name} 1`);
  writeFileSync(`${directory}/image_symbols.h`, '#pragma once\n#define SYM_XNetStartup 00011040\n#define SYM_WSAStartup 00011080\n#define SYM_Direct3D_CreateDevice 000110C0\n#define SYM_D3D_Device 00020000\n#define SYM_D3D_pDevice 00023000\n#define SYM_D3D_InitDefaultState 00011000\n' +
    audioSymbols.join('\n') + '\n' +
    '#define SYM_CDirectSoundStream_SetFormat FFFF0000\n#define HAS_CDirectSoundStream_SetFormat 0\n' +
    '#define SYM_IDirectSound_SetDistanceFactor FFFF0008\n#define HAS_IDirectSound_SetDistanceFactor 0\n' +
    '#define SYM_DSound_StreamVtable 000121C0\n#define HAS_DSound_StreamVtable 1\n' +
    '#define SYM_DSound_StreamVtable2 00012200\n#define HAS_DSound_StreamVtable2 1\n' +
    '#define SYM_DSound_HrtfFilters 0006B000\n#define HAS_DSound_HrtfFilters 1\n' +
    '#define SYM_DSound_HrtfIndex 0006B100\n#define HAS_DSound_HrtfIndex 1\n');
  const streamVtable = [
    'CDirectSoundStream_AddRef', 'CDirectSoundStream_Release', 'CDirectSoundStream_GetInfo', 'CDirectSoundStream_GetStatus',
    'CDirectSoundStream_Process', 'CDirectSoundStream_Discontinuity', 'CDirectSoundStream_Flush',
  ].map(audioAddress).concat([0x11040, 0x11080, 0x110c0, 0x11040, 0x11080]);
  writeFileSync(`${directory}/metadata.cpp`, `#include "browser/cpu_runtime.h"
#include "browser/audio-entries.h"
#include "cpu/x86_ops.h"
#include <cstring>
#include <emscripten/emscripten.h>
#include <new>
#include <stdexcept>
uint32_t failureMode = 0;
extern "C" EMSCRIPTEN_KEEPALIVE void fixture_failure_mode(uint32_t mode) { failureMode = mode; }
X86_FUNC(sub_00011040);
X86_FUNC(sub_00011080);
X86_FUNC(sub_000110C0);
X86_FUNC(__imp__sub_000110C0) { ctx.esp += 28; }
X86_FUNC(__imp__sub_00011040) { ctx.esp += 8; }
X86_FUNC(__imp__sub_00011080) { ctx.esp += 12; }
#define FIXTURE_AUDIO_DECL(name, id, args) X86_FUNC(BROWSER_AUDIO_GUEST(name));
BROWSER_AUDIO_ENTRIES(FIXTURE_AUDIO_DECL)
#define FIXTURE_AUDIO_ORIGINAL(name, id, args) X86_FUNC(BROWSER_AUDIO_ORIGINAL(name)) { ctx.esp += 4; }
BROWSER_AUDIO_ENTRIES(FIXTURE_AUDIO_ORIGINAL)
const auto entry = [](X86Context& context, uint8_t* base) {
  if (failureMode == 1) { throw std::bad_alloc(); }
  if (failureMode == 2) { throw std::logic_error("synthetic host logic failure"); }
  auto& memory = browser::Memory::From(base);
  memory.Read(context, 0x120f0, &context.ebx, 4);
  if (context.ebx != 0xc0000010) { throw std::runtime_error("incorrect launch data cell target"); }
  memory.Read(context, context.ebx, &context.eax, 4);
  if (context.eax) { throw std::runtime_error("cold boot launch page is not null"); }
  const uint32_t launchPage = 0x12345000;
  memory.Write(context, context.ebx, &launchPage, 4);
  memory.Read(context, context.ebx, &context.ecx, 4);
  if (context.ecx != launchPage) { throw std::runtime_error("launch data cell is not writable"); }
  uint32_t cacheCountAddress = 0, cacheCount = 0;
  memory.Read(context, 0x12138, &cacheCountAddress, 4);
  memory.Read(context, cacheCountAddress, &cacheCount, 4);
  if (cacheCount != 1) { throw std::runtime_error("cache partition count does not match owned virtual backing"); }
  uint32_t imageNameAddress = 0, imageName[2] = {};
  memory.Read(context, 0x12154, &imageNameAddress, 4);
  memory.Read(context, imageNameAddress, imageName, sizeof(imageName));
  constexpr char expectedName[] = "\\\\Device\\\\CdRom0\\\\default.xbe";
  char storedName[sizeof(expectedName)] = {};
  if (imageName[0] != ((sizeof(expectedName) << 16) | (sizeof(expectedName) - 1))) {
    throw std::runtime_error("image file name descriptor is invalid");
  }
  memory.Read(context, imageName[1], storedName, sizeof(storedName));
  if (std::memcmp(storedName, expectedName, sizeof(storedName))) { throw std::runtime_error("image file name is outside the mounted optical namespace"); }
  uint32_t tickAddress = 0, beforeTick = 0, afterTick = 0;
  memory.Read(context, 0x1215c, &tickAddress, 4);
  memory.Read(context, tickAddress, &beforeTick, 4);
  std::this_thread::sleep_for(std::chrono::milliseconds(3));
  browser::Runtime::From(context).Poll(context, base);
  memory.Read(context, tickAddress, &afterTick, 4);
  if (afterTick == beforeTick) { throw std::runtime_error("session tick count did not advance without armed timers"); }
  const uint32_t savedStack = context.esp;
  context.esp -= 12;
  const std::array<uint32_t, 3> sdkFrame = {0x11020, 0x202, 0};
  memory.Write(context, context.esp, sdkFrame.data(), sizeof(sdkFrame));
  sub_00011040(context, base);
  context.esp = savedStack - 12;
  sub_00011080(context, base);
  context.esp = savedStack + 8;
};
#define FIXTURE_AUDIO_MAP(name, id, args) { BROWSER_AUDIO_ADDRESS(name), BROWSER_AUDIO_GUEST(name) },
const X86FuncMapping X86FuncMappings[] = {{0x11000, entry}, {0x11040, sub_00011040}, {0x11080, sub_00011080}, {0x110c0, sub_000110C0},
  BROWSER_AUDIO_ENTRIES(FIXTURE_AUDIO_MAP)
  {0, nullptr}};
const uint32_t X86Continuations[] = {0};
const uint32_t X86ImageBase = 0x10000, X86ImageEnd = 0x13000, X86EntryPoint = 0x11000;
`);
  const build = spawnSync('em++', ['-std=c++20', '-O1', '-pthread', '-fwasm-exceptions', '-DX86_COMPACT_MEMORY', '-Isrc', `-I${directory}`,
    'src/browser/boot-wasm.cpp', 'src/browser/disc-wasm.cpp', 'src/kernel/disc_index.cpp',
    'src/browser/image.cpp', 'src/browser/sdk-provider.cpp', 'src/browser/diagnostic-graphics.cpp', 'src/browser/hook-graphics.cpp', 'src/browser/diagnostic-shaders.cpp', 'src/browser/push-state.cpp', 'src/browser/silent-audio.cpp', 'src/browser/audio-core.cpp', 'src/browser/audio-effects.cpp', 'src/browser/audio-provider.cpp', 'src/browser/kernel-provider.cpp', 'src/browser/thread-provider.cpp', 'src/browser/synchronization-provider.cpp', 'src/browser/dispatcher.cpp', 'src/browser/dispatcher-handles.cpp', 'src/browser/dispatcher-object-abi.cpp', 'src/browser/guest-wait.cpp', 'src/browser/platform-profile.cpp', 'src/browser/shutdown-provider.cpp', 'src/browser/dpc-provider.cpp', 'src/browser/timer-provider.cpp', 'src/browser/cpu_runtime.cpp', 'src/browser/virtual-memory.cpp', 'src/browser/contiguous-memory.cpp', 'src/browser/string-provider.cpp', 'src/browser/section-provider.cpp', 'src/browser/file-provider.cpp', 'src/browser/temporary-filesystem.cpp', 'src/browser/raw-cache.cpp', 'src/browser/cache-fatx.cpp', 'src/kernel/crypto.cpp', `${directory}/metadata.cpp`,
    '--no-entry', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0',
    '-sINITIAL_MEMORY=134217728', '-sALLOW_MEMORY_GROWTH=1', '-sEXPORTED_RUNTIME_METHODS=HEAPU8', '-o', `${directory}/boot.mjs`,
  ], { encoding: 'utf8', timeout: 120000, env: { ...process.env, TMPDIR: directory } });
  assert.equal(build.status, 0, build.stderr);
  const { default: create } = await import(pathToFileURL(`${directory}/boot.mjs`));
  const fixture = syntheticDisc();
  const discBytes = new Uint8Array(await fixture.arrayBuffer());
  const module = await create({ readIsoBytes: (offset, destination, length) => {
    module.HEAPU8.set(discBytes.subarray(offset, offset + length), destination);
    return length;
  } });
  const compiledIdentity = () => {
    const start = module._boot_build_identity() >>> 0;
    assert.equal(module._boot_build_identity_size(), 64);
    return new TextDecoder().decode(module.HEAPU8.subarray(start, start + 64));
  };
  assert.equal(compiledIdentity(), identity);
  const ownership = new Int32Array(new SharedArrayBuffer(4));
  Atomics.store(ownership, 0, 1);
  const messages = [];
  let closed = false;
  await createBootSession({
    create: async () => module,
    manifest: { identity: 'd4'.repeat(32) },
    prepare: async () => { assert.fail('A mixed build must not prepare an image'); },
    postMessage: (message) => messages.push(message),
  })({ data: { ownership, generation: 1, mailbox: createIsoMailbox(), size: fixture.size, port: { close: () => { closed = true; } } } });
  assert.equal(messages[0].code, 'BUILD_IDENTITY');
  assert.equal(closed, true);
  assert.equal(module._image_sections(), 0);
  const find = () => {
    const path = new TextEncoder().encode('default.xbe');
    module.HEAPU8.set(path, module._validation_path());
    return module._validation_find(path.length);
  };
  assert.equal(module._mount_disc(fixture.size), 1);
  assert.equal(find(), 1);
  assert.equal(module._validation_size(), 7);
  assert.equal(module._validation_read(5, 10), 2);
  assert.deepEqual([...module.HEAPU8.slice(module._validation_buffer(), module._validation_buffer() + 2)], [60, 70]);
  for (const size of [NaN, Infinity, -1, 0.5, 2 ** 53]) {
    assert.equal(module._mount_disc(size), 0);
    assert.equal(find(), 0);
  }
  const writeStreamVtables = (bytes) => {
    const view = new DataView(bytes.buffer);
    for (const [slot, target] of streamVtable.entries()) {
      view.setUint32(0x7c0 + slot * 4, target, true);
      view.setUint32(0x800 + slot * 4, target, true);
    }
    return bytes;
  };
  const prepare = (entry = 0x11000) => {
    const bytes = new Uint8Array(0x900);
    bytes.set(inventedImage());
    writeStreamVtables(bytes);
    new DataView(bytes.buffer).setUint32(0x288, 0x300, true);
    const address = module._image_allocate(bytes.length);
    module.HEAPU8.set(bytes, address);
    return module._image_prepare(0x10000, 0x13000, entry);
  };
  assert.equal(module._image_bind(), 0);
  assert.equal(prepare(0x11010), 0);
  for (let n = 0; n < 3; ++n) {
    assert.equal(prepare(), 1);
    assert.equal(module._image_sections(), 2);
    assert.equal(module._image_disc_reads_words(), 8);
    assert.deepEqual(Array.from({ length: 8 }, (_, index) => module._image_disc_reads_word(index)), Array(8).fill(0));
    assert.equal(module._image_entry_mapped(), 1);
    assert.equal(module._image_imports(), 1);
    assert.equal(module._image_bind(), 0);
    assert.equal(module._image_coverage_count(), 1);
    assert.equal(module._image_coverage_slot(0), 0x12040);
    assert.equal(module._image_coverage_ordinal(0), 1);
    assert.equal(module._image_coverage_target(0), 0);
    assert.equal(module._image_coverage_kind(0), 0);
    assert.equal(module._image_coverage_ordinal(1), 0);
    assert.equal(module._image_fault_ordinal(), 1);
    assert.equal(module._image_bound(), 0);
    assert.equal(module._image_active_workers(), 0);
    module._image_reset();
    assert.equal(compiledIdentity(), identity);
    assert.equal(module._image_sections(), 0);
    assert.equal(module._image_coverage_count(), 0);
    assert.equal(module._image_trace_count(), 0);
    assert.equal(module._image_trace_word(0, 0), 0);
    assert.equal(module._image_sdk_trace_count(), 0);
    assert.equal(module._image_sdk_trace_word(0, 0), 0);
    assert.deepEqual(Array.from({ length: 6 }, (_, field) => module._image_graphics_info(field)), Array(6).fill(0));
    assert.deepEqual(Array.from({ length: 10 }, (_, field) => module._image_audio_info(field)), Array(10).fill(0));
    assert.equal(module._image_graphics_checkpoint(), 0);
    assert.equal(module._image_graphics_event_word(0, 0), 0);
    assert.equal(module._image_graphics_resource_word(0, 0), 0);
    assert.equal(module._image_disc_reads_words(), 0);
    assert.deepEqual(Array.from({ length: 8 }, (_, index) => module._image_disc_reads_word(index)), Array(8).fill(0));
    assert.equal(module._image_entry_mapped(), 0);
    assert.equal(module._image_fault_ordinal(), 0);
  }
  const supported = new Uint8Array(0x900);
  supported.set(inventedImage());
  writeStreamVtables(supported);
  const imports = new DataView(supported.buffer);
  imports.setUint32(0x288, 0x300, true);
  imports.setUint32(0x158, 0x120a0 ^ 0x5b6d40b6, true);
  imports.setUint32(0x60c, 0, true);
  for (const [n, ordinal] of [277, 291, 294, 161, 252, 253, 129, 15, 17, 23, 301, 24, 346, 340, 322, 324, 47, 107, 113, 184, 164, 165, 171, 178, 289, 279, 67, 190, 202, 211, 218, 236, 255, 187, 258, 149, 327, 328, 40, 219, 196, 128, 200, 203, 215, 326, 217, 156, 126, 323, 181, 41, 42, 335, 336, 337, 338, 339, 166, 226, 199, 2, 356, 0].entries()) {
    imports.setUint32(0x6a0 + n * 4, ordinal ? 0x80000000 + ordinal : 0, true);
  }
  for (let n = 0; n < 3; ++n) {
    const address = module._image_allocate(supported.length);
    module.HEAPU8.set(supported, address);
    assert.equal(module._image_prepare(0x10000, 0x13000, 0x11000), 1);
    assert.deepEqual(Array.from({ length: 10 }, (_, field) => module._image_audio_info(field)), [48000, 6, 48000, 0, 0, 0, 0, 0, 0, 0]);
    assert.equal(module._image_declare_import(322, 0), 0);
    assert.equal(module._image_declare_import(324, 0), 0);
    assert.equal(module._image_declare_import(164, 0), 0);
    assert.equal(module._image_declare_import(322, 1), 1);
    assert.equal(module._image_declare_import(324, 1), 1);
    assert.equal(module._image_bind(), 1);
    assert.equal(module._image_bound(), 1);
    assert.equal(module._image_coverage_count(), 63);
    for (let n = 0; n < 63; ++n) {
      const ordinal = module._image_coverage_ordinal(n);
      if (ordinal === 40 || ordinal === 326 || ordinal === 156 || ordinal === 323 || ordinal === 41 || ordinal === 42 || ordinal === 356) {
        assert.equal(module._image_coverage_kind(n), 2);
        assert.notEqual(module._image_coverage_target(n) >>> 0, 0);
      } else if (ordinal === 322 || ordinal === 324 || ordinal === 164) {
        assert.equal(module._image_coverage_kind(n), 2);
        const target = ordinal === 322 ? 0xc0000000 : ordinal === 324 ? 0xc0000008 : 0xc0000010;
        assert.equal(module._image_coverage_target(n) >>> 0, target);
      } else {
        assert.equal(module._image_coverage_kind(n), 1);
        assert.equal(module._image_coverage_target(n) >>> 0, 0xfe000000 + ordinal * 4);
      }
    }
    assert.equal(typeof module._image_progress_address, 'function');
    const progressAddress = module._image_progress_address() >>> 0;
    assert.ok(progressAddress > 0 && progressAddress <= module.HEAPU8.byteLength - 4);
    assert.equal(progressAddress % 4, 0);
    const progress = new Uint32Array(module.HEAPU8.buffer, progressAddress, 1);
    const beforeExecution = Atomics.load(progress, 0);
    assert.equal(module._image_sdk_trace_count(), 0);
    assert.equal(module._image_execute(), 1);
    assert.equal(module._image_sdk_trace_count(), 2);
    assert.equal(module._image_sdk_trace_word(0, 4), 1);
    assert.equal(module._image_sdk_trace_word(1, 4), 2);
    assert.equal(module._image_sdk_trace_word(0, 8), 0x11020);
    assert.equal(module._image_sdk_trace_word(1, 9), 2);
    assert.equal(module._image_sdk_trace_word(1, 12), 0x202);
    assert.equal(module._image_sdk_trace_word(2, 4), 0);
    assert.equal(module._image_sdk_trace_word(0, 10), 1);
    assert.equal(module._image_sdk_trace_word(1, 10), 0);
    assert.equal(module._image_sdk_trace_word(0, 31), 0);
    assert.notEqual(Atomics.load(progress, 0), beforeExecution);
    assert.equal(module._image_entered(), 1);
    assert.equal(module._image_returned(), 1);
    assert.equal(module._image_active_workers(), 0);
    assert.equal(module._image_fault_ordinal(), 0);
    assert.equal(module._image_declare_import(164, 1), 0);
    assert.equal(module._image_bind(), 0);
    module._image_reset();
    assert.equal(module._image_audio_info(0), 0);
    assert.equal(module._image_graphics_checkpoint(), 0);
  }
  for (const [mode, expected] of [[1, 'bad_alloc'], [2, 'synthetic host logic failure']]) {
    const address = module._image_allocate(supported.length);
    module.HEAPU8.set(supported, address);
    assert.equal(module._image_prepare(0x10000, 0x13000, 0x11000), 1);
    assert.equal(module._image_bind(), 1);
    module._fixture_failure_mode(mode);
    assert.equal(module._image_execute(), 0);
    const start = module._image_fault_message() >>> 0;
    const length = module._image_fault_message_size();
    const message = new TextDecoder().decode(module.HEAPU8.subarray(start, start + length));
    assert.ok(message.includes(expected), message);
    assert.equal(module._image_fault_ordinal(), 0);
    assert.equal(module._image_entered(), 1);
    assert.equal(module._image_returned(), 0);
    assert.equal(module._image_active_workers(), 0);
    module._image_reset();
  }
  assert.equal(prepare(), 1);
  assert.equal(module._image_allocate(0), 0);
  assert.equal(module._image_sections(), 0);
});
