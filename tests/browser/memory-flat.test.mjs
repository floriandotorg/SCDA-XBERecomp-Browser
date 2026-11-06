import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
const directory = 'out/memory-flat-tests';
mkdirSync(directory, { recursive: true });
for (const platform of ['native', 'wasm']) {
  test(`${platform} flat page translation tracks mapping, aliases, repeats and physical writes`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'pages.mjs' : 'native'}`;
    const flags = wasm ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sINITIAL_MEMORY=201326592', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '-sEXIT_RUNTIME=1'] : ['-fsanitize=address,undefined'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O2', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-DX86_FLAT_MEMORY', '-Isrc', 'tests/browser/memory-flat.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    writeFileSync(`${directory}/run.mjs`, "import create from './pages.mjs'; await create();\n");
    const result = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout, /memory-flat-contracts-ok/);
  });
}
