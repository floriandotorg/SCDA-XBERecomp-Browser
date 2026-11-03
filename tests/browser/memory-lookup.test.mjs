import test from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';

mkdirSync('out/memory-lookup-tests', { recursive: true });
for (const platform of ['native', 'wasm']) {
  test(`${platform} memory lookup stays bounded with fragmented mappings and revalidates backing`, () => {
    const wasm = platform === 'wasm';
    const output = `out/memory-lookup-tests/${platform}${wasm ? '.mjs' : ''}`;
    const flags = wasm ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sINITIAL_MEMORY=134217728', '-sEXIT_RUNTIME=1'] : [];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O3', '-pthread', '-DX86_PORTABLE_RUNTIME',
      '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/memory-lookup.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output],
    { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const runner = 'out/memory-lookup-tests/run.mjs';
    if (wasm) { writeFileSync(runner, "import create from './wasm.mjs'; await create();\n"); }
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [runner] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(run.status, 0, run.stdout + run.stderr);
    assert.match(run.stdout, /memory-lookup-contracts-ok/);
    process.stdout.write(run.stdout);
  });
}
