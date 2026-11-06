import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/mission-input-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './input.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} the simulation entry samples level time and leaves mouse look to mouse.cpp`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'input.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=134217728', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const source = 'src/browser/mission-input.cpp';
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread',
      '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/mission-input.cpp',
      ...(existsSync(source) ? [source] : []), 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /mission-input-contracts-ok/);
  });
}
