import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
mkdirSync('out/shader-inventory-tests', { recursive: true });
for (const platform of ['native', 'wasm']) {
  test(`${platform} observed shader inventory respects end tokens and bound texture modes`, () => {
    const wasm = platform === 'wasm';
    const output = `out/shader-inventory-tests/${wasm ? 'inventory.mjs' : 'native'}`;
    const flags = wasm ? ['-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sEXIT_RUNTIME=1'] : ['-fsanitize=address,undefined'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-Isrc', 'tests/browser/shader-inventory.cpp', ...flags, '-o', output], { encoding: 'utf8' });
    assert.equal(build.status, 0, build.stderr);
    writeFileSync('out/shader-inventory-tests/run.mjs', "import create from './inventory.mjs'; await create();\n");
    const run = spawnSync(wasm ? process.execPath : output, wasm ? ['out/shader-inventory-tests/run.mjs'] : [], { encoding: 'utf8' });
    assert.equal(run.status, 0, run.stderr);
  });
}
