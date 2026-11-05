import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';

const directory = 'out/resident-buffers-tests';
mkdirSync(directory, { recursive: true });
for (const platform of ['native', 'wasm']) {
  test(`${platform} resident buffer versions and raw vertex decoding`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'resident.mjs' : 'native'}`;
    const flags = wasm ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sINITIAL_MEMORY=134217728', '-sALLOW_MEMORY_GROWTH=1', '-sEXIT_RUNTIME=1'] : ['-fsanitize=address,undefined'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/resident-buffers.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    writeFileSync(`${directory}/run.mjs`, "import create from './resident.mjs'; await create();\n");
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(run.status, 0, run.stdout + run.stderr);
  });
}
