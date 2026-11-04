import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
mkdirSync('out/mission-clock-tests', { recursive: true });
test('background intervals do not become simulation time and reversal fails explicitly', () => {
  const build = spawnSync('clang++', ['-std=c++20', '-fsanitize=address,undefined', '-Isrc', 'tests/browser/mission-clock.cpp', '-o', 'out/mission-clock-tests/native'], { encoding: 'utf8' });
  assert.equal(build.status, 0, build.stderr);
  const run = spawnSync('out/mission-clock-tests/native', [], { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stderr);
});
