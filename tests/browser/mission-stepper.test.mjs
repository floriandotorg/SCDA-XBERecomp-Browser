import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
const directory = resolve('out/mission-stepper-tests');
mkdirSync(directory, { recursive: true });
for (const platform of ['native', 'wasm']) {
  test(`${platform} bounded simulation steps account for elapsed time, pause and scene replacement`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'stepper.mjs' : 'native'}`;
    const flags = wasm ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sEXIT_RUNTIME=1'] : [];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O2', '-Isrc', 'tests/browser/mission-stepper.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    writeFileSync(`${directory}/run.mjs`, "import create from './stepper.mjs'; await create();\n");
    const execution = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(execution.status, 0, execution.stderr);
    assert.match(execution.stdout, /mission-stepper-contracts-ok/);
  });
}
