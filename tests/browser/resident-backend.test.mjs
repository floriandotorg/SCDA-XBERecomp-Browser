import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/resident-backend-tests');
const temporary = resolve('out/test-tmp');
mkdirSync(directory, { recursive: true });
mkdirSync(temporary, { recursive: true });
const environment = { ...process.env, TMPDIR: temporary };
writeFileSync(`${directory}/run.mjs`, "import create from './backend.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} resident raw buffer cache hits and invalidation`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'backend.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=33554432', '-sALLOW_MEMORY_GROWTH=1', '-sSTACK_SIZE=2097152']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/resident-backend.cpp', 'src/browser/menu-backend.cpp', 'src/browser/menu-shaders.cpp', 'src/browser/menu-pixel-shaders.cpp', 'src/browser/menu-texture.cpp', 'src/browser/render-snapshot.cpp', 'src/browser/webgpu-transport.cpp', 'src/browser/cpu_runtime.cpp', 'src/gpu/shaders.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 300000, env: environment });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000, env: environment });
    assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
    assert.match(run.stdout, /resident-backend-ok/);
  });
}
