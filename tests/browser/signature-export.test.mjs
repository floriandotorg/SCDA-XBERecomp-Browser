import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/signature-export-tests');
mkdirSync(`${directory}/tmp`, { recursive: true });
const environment = { ...process.env, TMPDIR: `${directory}/tmp` };
const functions = [
  [7, '000531AC'], [7, '00053213'], [8, '00059F2A'],
  [8, '00059F30'], [8, '00059F36'], [10, '000677A4'],
];
const declarations = functions.map(([, address]) => `X86_EXTERN_FUNC(sub_${address});`).join('\n');
const bodies = functions.map(([unit, address]) => {
  const source = readFileSync(`generated/game/x86_recomp.${unit}.cpp`, 'utf8');
  const begin = source.indexOf(`X86_FUNC(__imp__sub_${address}) {`);
  const suffix = `X86_WEAK_FUNC(sub_${address}, __imp__sub_${address})`;
  const end = source.indexOf(suffix, begin);
  assert.ok(begin >= 0 && end > begin, `audited generated function ${address} is absent`);
  return source.slice(begin, end + suffix.length);
});
writeFileSync(`${directory}/generated-signature.cpp`, `#include "cpu/x86_ops.h"\n${declarations}\n${bodies.join('\n')}\n`);
writeFileSync(`${directory}/run.mjs`, "import create from './signature.mjs'; await create();\n");

for (const platform of ['native', 'wasm']) {
  test(`${platform} owned export325, isolation, rejection and unchanged public HMAC/comparison contracts`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'signature.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread',
      '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/signature-export.cpp',
      `${directory}/generated-signature.cpp`, 'src/browser/platform-profile.cpp', 'src/browser/image.cpp',
      'src/browser/kernel-provider.cpp', 'src/browser/cpu_runtime.cpp', 'src/kernel/crypto.cpp', ...flags, '-o', output],
    { encoding: 'utf8', timeout: 120000, env: environment });
    writeFileSync(`${directory}/${platform}-build.log`, `${build.stdout ?? ''}${build.stderr ?? ''}`);
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [],
      { encoding: 'utf8', timeout: 30000, env: environment });
    writeFileSync(`${directory}/${platform}-run.log`, `${run.stdout ?? ''}${run.stderr ?? ''}`);
    assert.equal(run.status, 0, run.stderr);
    assert.match(run.stdout, /signature-export-contracts-ok/);
  });
}
