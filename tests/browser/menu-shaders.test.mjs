import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/menu-shaders-tests');
mkdirSync(directory, { recursive: true });
for (const platform of ['native', 'wasm']) {
  test(`${platform} strict menu WGSL and constants`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'test.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sSTACK_SIZE=1048576', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const env = { ...process.env, TMPDIR: directory };
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-Isrc', 'tests/browser/menu-shaders.cpp', 'src/browser/menu-shaders.cpp', 'src/browser/menu-pixel-shaders.cpp', 'src/gpu/shaders.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000, env });
    assert.equal(build.status, 0, build.stderr);
    if (wasm) {
      writeFileSync(`${directory}/run.mjs`, "import create from './test.mjs'; await create();\n");
    }
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [directory], { encoding: 'utf8', timeout: 30000, env });
    assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
    assert.match(run.stdout, /menu-shaders-ok/);
  });
}
