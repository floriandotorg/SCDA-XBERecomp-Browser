import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdirSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/image-carry-tests');
mkdirSync(directory, { recursive: true });
test('an image switch carries the launch page, the kept display and the profile filesystem exactly', () => {
  const sources = ['src/browser/image-carry.cpp'].filter(existsSync);
  const build = spawnSync('clang++', ['-std=c++20', '-O1', '-g', '-Isrc', 'tests/browser/image-carry.cpp', ...sources, 'src/browser/temporary-filesystem.cpp',
    '-fsanitize=address,undefined', '-o', `${directory}/native`], { encoding: 'utf8', timeout: 120000 });
  assert.equal(build.status, 0, build.stderr);
  const run = spawnSync(`${directory}/native`, [], { encoding: 'utf8', timeout: 30000 });
  assert.equal(run.status, 0, run.stdout + run.stderr);
  assert.match(run.stdout, /image-carry-ok/);
});
