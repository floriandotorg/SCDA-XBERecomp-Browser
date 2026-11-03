import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/delay-provider-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './delay-provider.mjs'; await create();\n");

for (const platform of ['native', 'wasm']) {
  test(`${platform} checked nonalertable kernel delay ABI and independent worker progress`, () => {
    const output = `${directory}/${platform === 'wasm' ? 'delay-provider.mjs' : 'native'}`;
    const flags = platform === 'wasm'
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sALLOW_MEMORY_GROWTH=0', '-sPTHREAD_POOL_SIZE=2', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '-sSTACK_OVERFLOW_CHECK=2', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(platform === 'wasm' ? 'em++' : 'clang++', [
      '-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc',
      'tests/browser/delay-provider.cpp', 'src/browser/delay-provider.cpp', 'src/browser/cpu_runtime.cpp',
      ...flags, '-o', output,
    ], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const execution = platform === 'wasm'
      ? spawnSync(process.execPath, [`${directory}/run.mjs`], { encoding: 'utf8', timeout: 30000 })
      : spawnSync(output, [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(execution.status, 0, execution.stderr);
    assert.match(execution.stdout, /delay-provider-contracts-ok/);
    writeFileSync(`${directory}/${platform}-results.txt`, execution.stdout);
  });
}
