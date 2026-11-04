import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/mission-memory-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './memory.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} mission virtual arena is large enough, bounded and reusable`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'memory.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread',
      '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-DBROWSER_MISSION=1', '-Isrc',
      'tests/browser/mission-memory.cpp', 'src/browser/virtual-memory.cpp', 'src/browser/cpu_runtime.cpp', 'src/browser/image.cpp',
      ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /mission-memory-contracts-ok/);
  });
}
