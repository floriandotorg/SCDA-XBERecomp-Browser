import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/worker-capacity-tests');
mkdirSync(directory, { recursive: true });
for (const platform of ['native', 'wasm']) {
  test(`${platform} finite worker admission cancels and joins every blocked worker`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'workers.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sINITIAL_MEMORY=134217728', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '-sPTHREAD_POOL_SIZE=129', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-pthread', '-Isrc', '-DX86_PORTABLE_RUNTIME',
      'tests/browser/worker-capacity.cpp', 'src/browser/cpu_runtime.cpp', 'src/browser/dispatcher.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    writeFileSync(`${directory}/run.mjs`, "import create from './workers.mjs'; await create();\n");
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /worker-capacity-contracts-ok/);
  });
}
