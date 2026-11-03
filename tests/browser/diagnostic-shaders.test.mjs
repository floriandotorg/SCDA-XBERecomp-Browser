import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/diagnostic-shaders-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './test.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} bounded portable guest shader parsing`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'test.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=134217728', '-sSTACK_SIZE=2097152']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const options = { encoding: 'utf8', timeout: 120000, env: { ...process.env, TMPDIR: directory } };
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/diagnostic-shaders.cpp', 'src/browser/diagnostic-shaders.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], options);
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], options);
    assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
    assert.match(run.stdout, /diagnostic-shaders-contracts-ok/);
  });
}
