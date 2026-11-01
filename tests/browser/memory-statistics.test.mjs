import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/memory-statistics-tests');
const temporary = `${directory}/tmp`;
mkdirSync(temporary, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './statistics.mjs'; await create();\n");
const env = { ...process.env, TMPDIR: temporary };
for (const platform of ['native', 'wasm']) {
  test(`${platform} authenticated memory statistics reflect coherent committed backing and exact ABI`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'statistics.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread',
      '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/memory-statistics.cpp',
      'src/browser/virtual-memory.cpp', 'src/browser/cpu_runtime.cpp', 'src/browser/image.cpp',
      'src/browser/kernel-provider.cpp', 'src/browser/section-provider.cpp', 'src/browser/contiguous-memory.cpp', 'src/kernel/crypto.cpp',
      ...flags, '-o', output], { encoding: 'utf8', timeout: 120000, env });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000, env });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /memory-statistics-contracts-ok/);
  });
}
