import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';

test('ramp, anisotropy rule, frame pacer and language ids follow native', () => {
  mkdirSync('out/settings-pack-tests', { recursive: true });
  const output = 'out/settings-pack-tests/settings-pack';
  const build = spawnSync('clang++', ['-std=c++20', '-O1', '-g', '-fsanitize=address,undefined', '-Isrc', 'tests/browser/settings-pack.cpp', '-o', output], { encoding: 'utf8' });
  assert.equal(build.status, 0, build.stderr);
  const run = spawnSync(output, { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stdout + run.stderr);
  assert.match(run.stdout, /settings-pack-contracts-ok/);
});
