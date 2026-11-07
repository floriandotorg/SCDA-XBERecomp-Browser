import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';

const directory = 'out/gyro-tests';
mkdirSync(directory, { recursive: true });
const wasmFlags = ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=67108864',
  '-sPTHREAD_POOL_SIZE=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1'];
const sources = ['tests/browser/gyro.cpp', 'src/browser/gyro.cpp', 'src/browser/input-core.cpp', 'src/browser/host-settings.cpp',
  'src/browser/host-messages.cpp', 'src/browser/hook-log.cpp'];

for (const platform of ['native', 'wasm']) {
  test(`${platform} gyro turns the view from the page's motion readings as src/core/gyro.cpp does from SDL's`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/gyro${wasm ? '.mjs' : ''}`;
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-Isrc', ...sources,
      ...(wasm ? wasmFlags : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']), '-o', output], { encoding: 'utf8', timeout: 180000 });
    assert.equal(build.status, 0, build.stderr);
    if (wasm) { writeFileSync(`${directory}/run.mjs`, "import create from './gyro.mjs'; await create();\n"); }
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 60000 });
    assert.equal(run.status, 0, run.stdout + run.stderr);
    assert.match(run.stdout, /gyro-contracts-ok/);
  });
}
