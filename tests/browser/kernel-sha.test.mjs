import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash, createHmac } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/kernel-sha-tests');
mkdirSync(`${directory}/tmp`, { recursive: true });
const env = { ...process.env, TMPDIR: `${directory}/tmp` };
writeFileSync(`${directory}/run.mjs`, "import create from './sha.mjs'; await create();\n");
const vectors = ['', 'a', 'ab'].map(input => `{${[...createHash('sha1').update(input).digest()].join(',')}}`);
const lengths = [0, 1, 55, 56, 63, 64, 65, 119, 120, 127, 128, 129, 257, 4097];
const coreVectors = lengths.map((length, n) => {
  const key = Buffer.from(Array.from({ length: n * 7 }, (_, n) => n * 13 & 255));
  const input = Buffer.from(Array.from({ length }, (_, n) => n * 17 & 255));
  const sha = createHash('sha1').update(input).digest();
  const hmac = createHmac('sha1', key).update(input).digest();
  return `{${key.length}, ${length}, {${[...sha].join(',')}}, {${[...hmac].join(',')}}}`;
});
writeFileSync(`${directory}/vectors.h`, `const std::array<std::array<uint8_t, 20>, 3> vectors = {{${vectors.join(',')}}};\nstruct CoreVector { uint32_t keyBytes, inputBytes; std::array<uint8_t, 20> sha, hmac; };\nconst std::array<CoreVector, ${coreVectors.length}> coreVectors = {{${coreVectors.join(',')}}};\n`);

for (const platform of ['native', 'wasm']) {
  test(`${platform} SHA init, update and final preserve checked opaque guest state`, () => {
    const output = `${directory}/${platform === 'wasm' ? 'sha.mjs' : 'native'}`;
    const flags = platform === 'wasm'
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=1048576', '-sSTACK_OVERFLOW_CHECK=2', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(platform === 'wasm' ? 'em++' : 'clang++', [
      '-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', `-I${directory}`,
      'tests/browser/kernel-sha.cpp', 'src/browser/kernel-provider.cpp', 'src/browser/cpu_runtime.cpp', 'src/kernel/crypto.cpp',
      ...flags, '-o', output,
    ], { encoding: 'utf8', env, timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const execution = platform === 'wasm'
      ? spawnSync(process.execPath, [`${directory}/run.mjs`], { encoding: 'utf8', env, timeout: 30000 })
      : spawnSync(output, [], { encoding: 'utf8', env, timeout: 30000 });
    assert.equal(execution.status, 0, execution.stderr);
    assert.match(execution.stdout, /kernel-sha-contracts-ok/);
  });
}
