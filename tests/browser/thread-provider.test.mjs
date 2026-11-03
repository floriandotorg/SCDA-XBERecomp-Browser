import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/thread-provider-tests');
mkdirSync(`${directory}/tmp`, { recursive: true });
const environment = { ...process.env, TMPDIR: `${directory}/tmp` };
writeFileSync(`${directory}/run.mjs`, "import create from './threads.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} bounded thread ownership preserves identity, references, priority, completion and teardown`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'threads.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=134217728', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread',
      '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/thread-provider.cpp',
      'src/browser/thread-provider.cpp', 'src/browser/image.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000, env: environment });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000, env: environment });
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /thread-provider-contracts-ok/);
  });
}
