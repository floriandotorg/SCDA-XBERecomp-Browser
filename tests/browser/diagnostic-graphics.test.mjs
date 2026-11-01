import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/diagnostic-graphics-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/image_symbols.h`, `#pragma once
#define SYM_XNetStartup 00F11000
#define SYM_WSAStartup 00F12000
#define SYM_Direct3D_CreateDevice 00F13000
#define SYM_D3D_Device 004AB670
#define SYM_D3D_pDevice 004AB668
#define SYM_D3D_InitDefaultState 004A1A80
#define SYM_D3DDevice_Clear 0049CE00
#define SYM_Lock2DSurface 0049DE60
#define SYM_D3D_BlockOnResource 004A17C0
#define SYM_D3DDevice_Swap 004A1C00
`);
writeFileSync(`${directory}/original.cpp`, `#include "browser/cpu_runtime.h"
#include "cpu/x86_ops.h"
extern std::atomic<uint32_t> originals;
X86_FUNC(__imp__sub_00F11000) { ++originals; ctx.esp += 8; }
X86_FUNC(__imp__sub_00F12000) { ++originals; ctx.esp += 12; }
X86_FUNC(__imp__sub_00F13000) { ++originals; throw browser::Fault(ctx, 0xfd001804, 4, "retained physical miniport"); }
X86_WEAK_FUNC(sub_00F11000, __imp__sub_00F11000)
X86_WEAK_FUNC(sub_00F12000, __imp__sub_00F12000)
X86_WEAK_FUNC(sub_00F13000, __imp__sub_00F13000)
X86_FUNC(__imp__sub_0049CE00) { throw browser::Fault(ctx, 0xfd001804, 4, "retained clear hardware"); }
X86_WEAK_FUNC(sub_0049CE00, __imp__sub_0049CE00)
X86_FUNC(__imp__sub_004A17C0) { throw browser::Fault(ctx, 0xfd001804, 4, "retained resource fence hardware"); }
X86_WEAK_FUNC(sub_004A17C0, __imp__sub_004A17C0)
X86_FUNC(__imp__sub_004A1C00) { throw browser::Fault(ctx, 0xfd001804, 4, "retained swap hardware"); }
X86_WEAK_FUNC(sub_004A1C00, __imp__sub_004A1C00)
`);
const retained = readFileSync('generated/game/x86_recomp.119.cpp', 'utf8');
const body = (symbol) => {
  const start = retained.indexOf(`X86_FUNC(__imp__sub_${symbol})`);
  const marker = `X86_WEAK_FUNC(sub_${symbol}, __imp__sub_${symbol})`;
  const end = retained.indexOf(marker, start);
  assert.ok(start >= 0 && end >= start, `retained CPU helper ${symbol} is absent`);
  return retained.slice(start, end + marker.length);
};
writeFileSync(`${directory}/retained.cpp`, `#include "cpu/x86_ops.h"
X86_FUNC(sub_004A17C0);
X86_FUNC(sub_0049DC30);
${body('0049DC30')}
${body('0049DE60')}
`);
writeFileSync(`${directory}/run.mjs`, "import create from './graphics.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} owned diagnostic graphics contract`, () => {
    const wasm = platform === 'wasm';
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=1048576', '-sPTHREAD_POOL_SIZE=4', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const output = `${directory}/${wasm ? 'graphics.mjs' : 'native'}`;
    const env = { ...process.env, TMPDIR: directory };
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', `-I${directory}`, '-Isrc', 'tests/browser/diagnostic-graphics.cpp', `${directory}/original.cpp`, `${directory}/retained.cpp`, 'src/browser/diagnostic-graphics.cpp', 'src/browser/sdk-provider.cpp', 'src/browser/contiguous-memory.cpp', 'src/browser/image.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000, env });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000, env });
    assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
    assert.match(run.stdout, /diagnostic-graphics-contracts-ok/);
  });
}
