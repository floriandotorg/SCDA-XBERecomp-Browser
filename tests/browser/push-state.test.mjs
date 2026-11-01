import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/push-state-tests');
const temporary = resolve('out/test-tmp');
mkdirSync(directory, { recursive: true });
mkdirSync(temporary, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './push-state.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} strict atomic NV2A state push decoder`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'push-state.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=33554432', '-sSTACK_SIZE=2097152']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const options = { encoding: 'utf8', timeout: 120000, env: { ...process.env, TMPDIR: temporary } };
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-Isrc', 'tests/browser/push-state.cpp', 'src/browser/push-state.cpp', ...flags, '-o', output], options);
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], options);
    assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
    assert.match(run.stdout, /push-state-contracts-ok/);
  });
}
