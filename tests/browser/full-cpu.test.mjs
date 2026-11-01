import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/full-cpu');
const run = (command, arguments_, timeout) => {
  const result = spawnSync(command, arguments_, { encoding: 'utf8', timeout, maxBuffer: 16 << 20 });
  assert.equal(result.status, 0, result.stderr + result.stdout);
  return result.stdout;
};

test('complete retained story metadata executes CPU contracts with compact backing', { timeout: 1200000 }, () => {
  mkdirSync(directory, { recursive: true });
  run('emcmake', ['cmake', '-S', '.', '-B', directory, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DBROWSER_BUILD=ON', '-DBROWSER_FULL_CPU=ON'], 120000);
  const build = run('cmake', ['--build', directory, '--target', 'cpu_contracts', '--parallel', '6'], 1000000);
  writeFileSync(`${directory}/build-results.txt`, build);
  writeFileSync(`${directory}/cpu-web/run.mjs`, "import create from './cpu_contracts.mjs'; await create();\n");
  const output = run(process.execPath, [`${directory}/cpu-web/run.mjs`], 30000);
  const manifest = JSON.parse(readFileSync(`${directory}/tools/browser/input-manifest.json`, 'utf8'));
  const metadata = /full-metadata-ok mappings=(\d+) continuations=(\d+)/.exec(output);
  assert.ok(metadata, output);
  assert.equal(Number(metadata[1]), manifest.functionCount + 4);
  assert.ok(Number(metadata[2]) > 1);
  assert.match(output, /cpu-runtime-ok/);
  writeFileSync(`${directory}/node-results.txt`, output);
});
