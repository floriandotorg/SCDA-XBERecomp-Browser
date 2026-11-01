import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/image-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './image.mjs'; await create();\n");

for (const platform of ['native', 'wasm']) {
  test(`${platform} image preparation, import binding and isolated thread TLS preserve checked startup state`, () => {
    const output = `${directory}/${platform === 'wasm' ? 'image.mjs' : 'native'}`;
    const flags = platform === 'wasm'
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=web,node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=134217728', '-sALLOW_MEMORY_GROWTH=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(platform === 'wasm' ? 'em++' : 'clang++', [
      '-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc',
      'tests/browser/image.cpp', 'src/browser/image.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output,
    ], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const execution = platform === 'wasm'
      ? spawnSync(process.execPath, [`${directory}/run.mjs`], { encoding: 'utf8', timeout: 30000 })
      : spawnSync(output, [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(execution.status, 0, execution.stderr);
    assert.match(execution.stdout, /image-contracts-ok/);
    writeFileSync(`${directory}/${platform}-results.txt`, execution.stdout);
  });
}
