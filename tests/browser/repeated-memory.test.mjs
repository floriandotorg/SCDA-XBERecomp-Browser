import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
const directory = 'out/repeated-memory-tests';
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './repeated.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} repeated copies and fills preserve instruction faults, aliases and overlap`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'repeated.mjs' : 'native'}`;
    const flags = wasm ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sINITIAL_MEMORY=201326592', '-sPTHREAD_POOL_SIZE=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1'] : ['-fsanitize=address,undefined'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O2', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/repeated-memory.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /repeated-memory-contracts-ok/);
  });
}
