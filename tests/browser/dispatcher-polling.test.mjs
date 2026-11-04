import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/dispatcher-polling-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './polling.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} idle dispatcher waits sleep while deadlines, signals and cancellation advance`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'polling.mjs' : 'native'}`;
    const flags = wasm ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0',
      '-sINITIAL_MEMORY=134217728', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1',
      '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '-sEXIT_RUNTIME=1'] : ['-fsanitize=address,undefined'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME',
      '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/dispatcher-polling.cpp', 'src/browser/dispatcher.cpp',
      'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /dispatcher-polling-contracts-ok/);
  });
}
