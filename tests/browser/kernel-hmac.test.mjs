import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/kernel-hmac-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './hmac.mjs'; await create();\n");
const lengths = [0, 1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129, 257, 4097];
const vectors = lengths.map((length, n) => {
  const key = Buffer.from(Array.from({ length: n * 7 }, (_, n) => n * 13 & 255));
  const input = Buffer.from(Array.from({ length }, (_, n) => n * 17 & 255));
  const digest = createHmac('sha1', key.subarray(0, 64)).update(input).digest();
  return `{${key.length}, ${length}, {${[...digest].join(',')}}}`;
});
writeFileSync(`${directory}/vectors.h`, `struct Vector { uint32_t keyBytes, inputBytes; std::array<uint8_t, 20> digest; };\nconst std::array<Vector, ${vectors.length}> vectors = {{${vectors.join(',\n')}}};\n`);

for (const platform of ['native', 'wasm']) {
  test(`${platform} session Xbox HMAC truncates keys and publishes only a checked complete digest`, () => {
    const output = `${directory}/${platform === 'wasm' ? 'hmac.mjs' : 'native'}`;
    const flags = platform === 'wasm'
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=134217728', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=1048576', '-sSTACK_OVERFLOW_CHECK=2', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(platform === 'wasm' ? 'em++' : 'clang++', [
      '-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', `-I${directory}`,
      'tests/browser/kernel-hmac.cpp', 'src/browser/kernel-provider.cpp', 'src/browser/cpu_runtime.cpp', 'src/kernel/crypto.cpp',
      ...flags, '-o', output,
    ], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const execution = platform === 'wasm'
      ? spawnSync(process.execPath, [`${directory}/run.mjs`], { encoding: 'utf8', timeout: 30000 })
      : spawnSync(output, [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(execution.status, 0, execution.stderr);
    assert.match(execution.stdout, /kernel-hmac-contracts-ok/);
  });
}
