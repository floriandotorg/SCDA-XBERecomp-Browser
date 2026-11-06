import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/hook-d3d-tests');
mkdirSync(directory, { recursive: true });
test('d3d shim latches fog, scales the view and forwards or counts optional effects', () => {
  const sources = ['src/browser/hook-d3d.cpp', 'src/browser/hook-graphics.cpp'].filter(existsSync);
  const build = spawnSync('clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc/browser/hook-compat', '-Isrc',
    'tests/browser/hook-d3d.cpp', ...sources, 'src/browser/cpu_runtime.cpp', '-fsanitize=address,undefined', '-o', `${directory}/native`], { encoding: 'utf8', timeout: 120000 });
  assert.equal(build.status, 0, build.stderr);
  const run = spawnSync(`${directory}/native`, [], { encoding: 'utf8', timeout: 30000 });
  assert.equal(run.status, 0, run.stdout + run.stderr);
  assert.match(run.stdout, /hook-d3d-contracts-ok/);
});
