import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/temporary-filesystem-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './temporary-filesystem.mjs'; await create({ arguments: process.argv.slice(2) });\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} temporary filesystem preserves atomic writes under a bounded copy peak`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'temporary-filesystem.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sMAXIMUM_MEMORY=268435456', '-sALLOW_MEMORY_GROWTH=0', '-sABORTING_MALLOC=0', '-sSTACK_SIZE=1048576', '-sPTHREAD_POOL_SIZE=1', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-Isrc',
      'tests/browser/temporary-filesystem.cpp', 'src/browser/temporary-filesystem.cpp', ...flags, '-o', output],
    { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    for (const mode of ['contracts', 'peak']) {
      const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`, mode] : [mode],
        { encoding: 'utf8', timeout: 120000 });
      writeFileSync(`${directory}/${platform}-${mode}-results.txt`, `${run.stdout}${run.stderr}`);
      assert.equal(run.status, 0, `${mode}: ${run.stdout}${run.stderr}`);
      assert.match(run.stdout, new RegExp(`temporary-filesystem-${mode}-ok`));
    }
  });
}
