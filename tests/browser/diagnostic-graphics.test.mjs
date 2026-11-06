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
#define SYM_D3DDevice_Swap 0049CC50
#define SYM_D3DDevice_SetGammaRamp 0049A3D0
#define SYM_D3DDevice_EnableOverlay 00F14000
#define SYM_D3DDevice_SetRenderStateNotInline 0049EFF0
#define SYM_D3DDevice_SetRenderState_Simple 0049EFC0
#define SYM_D3DDevice_SetVertexShaderConstant1Fast 0049E8B0
#define SYM_D3DDevice_DrawVertices 004A61A0
#define SYM_D3DDevice_DrawIndexedVertices 004A6240
#define SYM_D3D_SimpleRenderStateEncode 004A8000
#define SYM_D3D_RenderState 004AB3D0
#define SYM_D3D_DeferredTextureState 004AB1D0
#define SYM_D3D_Streams 004ADB10
#define SYM_D3D_IndexData 004AB1CC
#define SYM_D3D_DirtyFlags 004AB1C8
#define SYM_D3D_InternalAddRef 0049BD00
#define SYM_D3D_InternalRelease 0049BF30
#define SYM_D3DResource_Release 0049BD80
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
X86_FUNC(__imp__sub_0049CC50) { throw browser::Fault(ctx, 0xfd001804, 4, "retained swap hardware"); }
X86_WEAK_FUNC(sub_0049CC50, __imp__sub_0049CC50)
`);
const retained = readFileSync('generated/game/x86_recomp.119.cpp', 'utf8');
const body = (symbol) => {
  const start = retained.indexOf(`X86_FUNC(__imp__sub_${symbol})`);
  const marker = `X86_WEAK_FUNC(sub_${symbol}, __imp__sub_${symbol})`;
  const end = retained.indexOf(marker, start);
  assert.ok(start >= 0 && end >= start, `retained CPU helper ${symbol} is absent`);
  return retained.slice(start, end + marker.length);
};
const overlaySymbol = readFileSync('config/scda_v2.toml', 'utf8').match(/^D3DDevice_EnableOverlay\s*=\s*0x([0-9a-f]+)/im)?.[1]?.toUpperCase().padStart(8, '0');
assert.ok(overlaySymbol, 'configured overlay helper is absent');
writeFileSync(`${directory}/retained.cpp`, `#include "cpu/x86_ops.h"
X86_FUNC(sub_004A17C0);
X86_FUNC(sub_0049DC30);
X86_FUNC(sub_0049BD80);
void DestroyResource(X86Context&, uint8_t*);
X86_FUNC(sub_0049BC20) { DestroyResource(ctx, base); }
${body(overlaySymbol).replaceAll(`sub_${overlaySymbol}`, 'sub_00F14000')}
${body('0049BD80')}
${body('0049BD00')}
${body('0049BF30')}
${body('0049DC30')}
${body('0049DE60')}
`);
writeFileSync(`${directory}/run.mjs`, "import create from './graphics.mjs'; await create();\n");
test('Present checkpoint is a bounded trusted build option isolated from retained CPU compile definitions', () => {
  const cmake = readFileSync('tools/browser/CMakeLists.txt', 'utf8');
  const start = cmake.indexOf('set(BROWSER_MENU_TRACE_FRAMES ');
  const end = cmake.indexOf('if((BROWSER_MENU_CAPTURE OR BROWSER_MENU)', start);
  assert.ok(start >= 0 && end > start, 'trusted frame checkpoint validation is absent');
  const script = `${directory}/checkpoint-option.cmake`;
  writeFileSync(script, `${cmake.slice(start, end)}\nmessage(STATUS "validated-frames=\${BROWSER_MENU_TRACE_FRAMES}")\n`);
  for (const [menu, value, accepted] of [
    ['OFF', undefined, true], ['OFF', '0', true], ['ON', '0', true], ['ON', '1', true], ['ON', '100', true], ['ON', '10000', true],
    ['OFF', '1', false], ['ON', '10001', false], ['ON', '-1', false], ['ON', '1.5', false], ['ON', 'abc', false],
    ['ON', '01', false], ['ON', '', false], ['ON', '1;2', false], ['ON', '9999999999999999999999999', false],
  ]) {
    const result = spawnSync('cmake', [`-DBROWSER_MENU=${menu}`, ...(value === undefined ? [] : [`-DBROWSER_MENU_TRACE_FRAMES=${value}`]), '-P', script], { encoding: 'utf8' });
    assert.equal(result.status === 0, accepted, `menu=${menu}, frames=${value}: ${result.stdout}\n${result.stderr}`);
    if (accepted) {
      assert.match(result.stdout, new RegExp(`validated-frames=${value ?? '0'}\\b`));
    }
  }
  assert.match(cmake, /;menu-trace-frames=\$\{BROWSER_MENU_TRACE_FRAMES\}/);
  const definition = cmake.split('\n').filter((line) => line.includes('COMPILE_DEFINITIONS') && line.includes('BROWSER_MENU_TRACE_FRAMES'));
  assert.equal(definition.length, 1);
  assert.match(definition[0], /set_property\(SOURCE \$\{root\}\/src\/browser\/boot-wasm\.cpp PROPERTY COMPILE_DEFINITIONS/);
  assert.match(readFileSync('src/browser/boot-wasm.cpp', 'utf8'), /ConfigurePresentCheckpoint\(BROWSER_MENU_TRACE_FRAMES\)/);
});

for (const platform of ['native', 'wasm']) {
  test(`${platform} owned diagnostic graphics contract`, () => {
    const wasm = platform === 'wasm';
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=1048576', '-sPTHREAD_POOL_SIZE=4', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const output = `${directory}/${wasm ? 'graphics.mjs' : 'native'}`;
    const env = { ...process.env, TMPDIR: directory };
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', `-I${directory}`, '-Isrc', 'tests/browser/diagnostic-graphics.cpp', `${directory}/original.cpp`, `${directory}/retained.cpp`, 'src/browser/diagnostic-graphics.cpp', 'src/browser/hook-graphics.cpp', 'src/browser/diagnostic-shaders.cpp', 'src/browser/push-state.cpp', 'src/browser/sdk-provider.cpp', 'src/browser/contiguous-memory.cpp', 'src/browser/image.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000, env });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000, env });
    assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
    assert.match(run.stdout, /diagnostic-graphics-contracts-ok/);
  });
}
