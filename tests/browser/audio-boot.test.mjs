import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { inventedImage } from './image-fixture.mjs';
import { syntheticDisc } from './iso-wasm-fixture.mjs';

const directory = resolve('out/audio-boot-tests');
mkdirSync(directory, { recursive: true });

test('boot session clock drives silent audio consumption without further guest audio calls', async () => {
  const identity = 'a1'.repeat(32);
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
#include <array>
#include <chrono>
#include <cstring>
#include <emscripten/emscripten.h>
#include <stdexcept>
#include <thread>
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
  auto& memory = browser::Memory::From(base);
  constexpr uint32_t wave = 0x12240, desc = 0x12250, device = 0x12270, buffer = 0x12274, status = 0x12278;
  const std::array<uint32_t, 4> waveFormat = {(2u << 16) | 1u, 48000u, 48000u * 4u, (16u << 16) | 4u};
  const std::array<uint32_t, 5> bufferDesc = {20u, 0u, 9600u, wave, 0u};
  memory.Write(context, wave, waveFormat.data(), sizeof(waveFormat));
  memory.Write(context, desc, bufferDesc.data(), sizeof(bufferDesc));
  const uint32_t savedStack = context.esp;
  const auto dword = [&](uint32_t address) {
    uint32_t value = 0;
    memory.Read(context, address, &value, 4);
    return value;
  };
  const auto call = [&](X86Func* function, std::initializer_list<uint32_t> arguments) {
    context.esp -= 4 * uint32_t(arguments.size() + 1);
    uint32_t frame[8] = {0x11020};
    uint32_t slot = 1;
    for (const uint32_t argument : arguments) { frame[slot++] = argument; }
    memory.Write(context, context.esp, frame, 4 * uint32_t(arguments.size() + 1));
    function(context, base);
    context.esp = savedStack;
  };
  call(BROWSER_AUDIO_GUEST(DirectSoundCreate), {0u, device, 0u});
  call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {desc, buffer});
  call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Play), {dword(buffer), 0u, 0u, 1u});
  for (int n = 0; n < 6; ++n) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    browser::Runtime::From(context).Poll(context, base);
  }
  call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetStatus), {dword(buffer), status});
  if (!(dword(status) & 1u)) { throw std::runtime_error("looping buffer lost its playing status while clocked"); }
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
    'src/browser/image.cpp', 'src/browser/sdk-provider.cpp', 'src/browser/diagnostic-graphics.cpp', 'src/browser/hook-graphics.cpp', 'src/browser/diagnostic-shaders.cpp', 'src/browser/push-state.cpp', 'src/browser/silent-audio.cpp', 'src/browser/audio-core.cpp', 'src/browser/audio-effects.cpp', 'src/browser/audio-provider.cpp', 'src/browser/kernel-provider.cpp', 'src/browser/thread-provider.cpp', 'src/browser/synchronization-provider.cpp', 'src/browser/dispatcher.cpp', 'src/browser/dispatcher-handles.cpp', 'src/browser/dispatcher-object-abi.cpp', 'src/browser/guest-wait.cpp', 'src/browser/platform-profile.cpp', 'src/browser/shutdown-provider.cpp', 'src/browser/dpc-provider.cpp', 'src/browser/timer-provider.cpp', 'src/browser/cpu_runtime.cpp', 'src/browser/virtual-memory.cpp', 'src/browser/contiguous-memory.cpp', 'src/browser/string-provider.cpp', 'src/browser/section-provider.cpp', 'src/browser/file-provider.cpp', 'src/browser/temporary-filesystem.cpp', 'src/browser/storage-publisher.cpp', 'src/browser/raw-cache.cpp', 'src/browser/cache-fatx.cpp', 'src/kernel/crypto.cpp', `${directory}/metadata.cpp`,
    '--no-entry', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0',
    '-sINITIAL_MEMORY=134217728', '-sALLOW_MEMORY_GROWTH=1', '-sEXPORTED_RUNTIME_METHODS=HEAPU8', '-o', `${directory}/boot.mjs`,
  ], { encoding: 'utf8', timeout: 180000, env: { ...process.env, TMPDIR: directory } });
  assert.equal(build.status, 0, build.stderr);
  const { default: create } = await import(pathToFileURL(`${directory}/boot.mjs`));
  const fixture = syntheticDisc();
  const discBytes = new Uint8Array(await fixture.arrayBuffer());
  const module = await create({ readIsoBytes: (offset, destination, length) => {
    module.HEAPU8.set(discBytes.subarray(offset, offset + length), destination);
    return length;
  } });
  assert.equal(module._mount_disc(fixture.size), 1);
  const bytes = new Uint8Array(0x900);
  bytes.set(inventedImage());
  const view = new DataView(bytes.buffer);
  for (const [slot, target] of streamVtable.entries()) {
    view.setUint32(0x7c0 + slot * 4, target, true);
    view.setUint32(0x800 + slot * 4, target, true);
  }
  view.setUint32(0x288, 0x300, true);
  view.setUint32(0x158, 0x120a0 ^ 0x5b6d40b6, true);
  view.setUint32(0x60c, 0, true);
  const ordinals = [277, 291, 294, 161, 252, 253, 129, 15, 17, 23, 301, 24, 346, 340, 322, 324, 47, 107, 113, 184, 164, 165, 171, 178, 289, 279, 67, 190, 202, 211, 218, 236, 255, 187, 258, 149, 327, 328, 40, 219, 196, 128, 200, 203, 215, 326, 217, 156, 126, 323, 181, 41, 42, 335, 336, 337, 338, 339, 166, 226, 199, 2, 356, 0];
  ordinals.forEach((ordinal, n) => view.setUint32(0x6a0 + n * 4, ordinal ? 0x80000000 + ordinal : 0, true));
  const address = module._image_allocate(bytes.length);
  module.HEAPU8.set(bytes, address);
  assert.equal(module._image_prepare(0x10000, 0x13000, 0x11000), 1);
  assert.deepEqual(Array.from({ length: 10 }, (_, field) => module._image_audio_info(field)), [48000, 6, 48000, 0, 0, 0, 0, 0, 0, 0]);
  assert.equal(module._image_declare_import(322, 1), 1);
  assert.equal(module._image_declare_import(324, 1), 1);
  assert.equal(module._image_bind(), 1);
  assert.equal(module._image_execute(), 1, 'entry faulted while playing a clocked looping buffer');
  const submitted = BigInt(module._image_audio_info(4)) + (BigInt(module._image_audio_info(5)) << 32n);
  const consumed = BigInt(module._image_audio_info(6)) + (BigInt(module._image_audio_info(7)) << 32n);
  assert.ok(consumed > 48000n, `clock-driven consumption stalled at the sink capacity ${consumed}; the session clock never fed the provider`);
  assert.ok(submitted > 48000n, `the session clock never refilled the silent sink; submitted ${submitted}`);
  assert.ok(consumed <= submitted, 'consumed frames exceeded submitted frames');
  assert.equal(module._image_audio_info(10), 1, 'provider voice count not exported');
  const arenaFree = module._image_audio_info(11);
  assert.ok(arenaFree > 0 && arenaFree < 4 * 1024 * 1024, `provider arena accounting not exported: ${arenaFree}`);
  assert.equal(module._image_audio_info(17) !== 0, true, 'provider device identity not exported');
  assert.equal(module._image_fault_ordinal(), 0);
  module._image_reset();
  assert.equal(module._image_audio_info(0), 0);
});
