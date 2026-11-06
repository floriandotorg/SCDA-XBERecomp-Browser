import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';

test('render scale sizes the canvas and screen sized targets as native RenderScale', () => {
  mkdirSync('out/render-scale-tests', { recursive: true });
  const output = 'out/render-scale-tests/render-scale';
  const build = spawnSync('clang++', ['-std=c++20', '-O1', '-g', '-fsanitize=address,undefined', '-Isrc', 'tests/browser/render-scale.cpp', '-o', output], { encoding: 'utf8' });
  assert.equal(build.status, 0, build.stderr);
  const run = spawnSync(output, { encoding: 'utf8' });
  assert.equal(run.status, 0, run.stdout + run.stderr);
  assert.match(run.stdout, /render-scale-contracts-ok/);
});
