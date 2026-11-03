import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/audio-provider-tests');
mkdirSync(directory, { recursive: true });

const entries = [
  ['IDirectSound_Release', 1], ['IDirectSoundBuffer_Unlock', 5], ['IDirectSoundBuffer_Release', 1],
  ['DirectSoundUseFullHRTF', 0], ['CDirectSoundStream_AddRef', 1], ['CDirectSoundStream_Release', 1],
  ['CDirectSoundStream_GetInfo', 2], ['CDirectSoundStream_Discontinuity', 1], ['CDirectSoundStream_Flush', 1],
  ['CDirectSoundStream_GetStatus', 2], ['CDirectSoundStream_Process', 3], ['CDirectSoundStream_Pause', 2],
  ['CDirectSoundStream_FlushEx', 4], ['CDirectSoundStream_SetVolume', 2], ['CDirectSoundStream_SetFilter', 2],
  ['CDirectSoundStream_SetHeadroom', 2], ['CDirectSoundStream_SetMixBinVolumes', 2], ['IDirectSound_GetSpeakerConfig', 2],
  ['IDirectSound_SetEffectData', 6], ['IDirectSound_EnableHeadphones', 2], ['IDirectSound_SetMixBinHeadroom', 3],
  ['IDirectSoundBuffer_SetVolume', 2], ['IDirectSoundBuffer_SetFilter', 2], ['IDirectSoundBuffer_SetHeadroom', 2],
  ['IDirectSoundBuffer_SetMixBinVolumes', 2], ['IDirectSoundBuffer_Play', 4], ['IDirectSoundBuffer_Stop', 1],
  ['IDirectSoundBuffer_SetLoopRegion', 3], ['IDirectSoundBuffer_GetStatus', 2], ['IDirectSoundBuffer_GetCurrentPosition', 3],
  ['IDirectSoundBuffer_SetCurrentPosition', 2], ['IDirectSoundBuffer_Lock', 8], ['DirectSoundDoWork', 0],
  ['XAudioDownloadEffectsImage', 4], ['CDirectSoundStream_SetFrequency', 2], ['CDirectSoundStream_SetMixBins', 2],
  ['CDirectSoundStream_SetMaxDistance', 3], ['CDirectSoundStream_SetMinDistance', 3], ['CDirectSoundStream_SetPosition', 5],
  ['CDirectSoundStream_SetVelocity', 5], ['CDirectSoundStream_SetI3DL2Source', 3], ['IDirectSoundBuffer_SetFrequency', 2],
  ['IDirectSoundBuffer_SetMixBins', 2], ['IDirectSoundBuffer_SetMaxDistance', 3], ['IDirectSoundBuffer_SetMinDistance', 3],
  ['IDirectSoundBuffer_SetPosition', 5], ['IDirectSoundBuffer_SetVelocity', 5], ['IDirectSoundBuffer_SetI3DL2Source', 3],
  ['IDirectSoundBuffer_SetBufferData', 3], ['IDirectSound_CommitDeferredSettings', 1], ['IDirectSound_CreateSoundBuffer', 4],
  ['IDirectSound_SetDopplerFactor', 3], ['IDirectSound_SetOrientation', 8], ['IDirectSound_SetPosition', 5],
  ['IDirectSound_SetRolloffFactor', 3], ['IDirectSound_SetVelocity', 5], ['IDirectSound_SetI3DL2Listener', 3],
  ['DirectSoundCreate', 3], ['DirectSoundCreateBuffer', 2], ['DirectSoundCreateStream', 2],
];
assert.equal(entries.length, 60);

const symbols = ['#pragma once',
  '#define SYM_XNetStartup 00F11000', '#define SYM_WSAStartup 00F12000', '#define SYM_Direct3D_CreateDevice 00F13000',
  ...entries.map(([name], n) => `#define SYM_${name} ${(0xF30000 + n * 0x10).toString(16).toUpperCase().padStart(8, '0')}`),
  ...entries.map(([name]) => `#define HAS_${name} 1`),
  '#define SYM_CDirectSoundStream_SetFormat FFFF0000', '#define HAS_CDirectSoundStream_SetFormat 0',
  '#define SYM_IDirectSound_SetDistanceFactor FFFF0008', '#define HAS_IDirectSound_SetDistanceFactor 0',
  '#define SYM_DSound_StreamVtable 00F21000', '#define HAS_DSound_StreamVtable 1',
  '#define SYM_DSound_StreamVtable2 00F21040', '#define HAS_DSound_StreamVtable2 1',
  '#define SYM_DSound_HrtfFilters 00F22000', '#define HAS_DSound_HrtfFilters 1',
  '#define SYM_DSound_HrtfIndex 00F23000', '#define HAS_DSound_HrtfIndex 1',
  '',
];
writeFileSync(`${directory}/image_symbols.h`, symbols.join('\n'));
writeFileSync(`${directory}/run.mjs`, "import create from './audio-provider.mjs'; await create();\n");

for (const platform of ['native', 'wasm']) {
  test(`${platform} bounded session audio provider honors owned strong wrappers, truthful control/timing and named unsupported operations`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'audio-provider.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const env = { ...process.env, TMPDIR: directory };
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread',
      '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', `-I${directory}`, '-Isrc', 'tests/browser/audio-provider.cpp',
      'src/browser/audio-provider.cpp', 'src/browser/audio-core.cpp', 'src/browser/audio-effects.cpp', 'src/browser/sdk-provider.cpp', 'src/browser/silent-audio.cpp', 'src/kernel/disc_index.cpp',
      'src/browser/timer-provider.cpp', 'src/browser/dpc-provider.cpp', 'src/browser/image.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output],
    { encoding: 'utf8', timeout: 120000, env });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 60000, env });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /audio-provider-contracts-ok/);
  });
}
