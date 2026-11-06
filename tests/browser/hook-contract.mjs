import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

export const hookDirectory = resolve('out/hook-compat-tests');
mkdirSync(hookDirectory, { recursive: true });
const wasmFlags = ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=201326592',
  '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '-sEXIT_RUNTIME=1'];

export const buildHookContract = ({ name, platform, memory, sources, definitions = [], includes = [] }) => {
  const wasm = platform === 'wasm';
  const stem = `${name}-${memory}`;
  const output = `${hookDirectory}/${stem}${wasm ? '.mjs' : ''}`;
  const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY',
    ...(memory === 'flat' ? ['-DX86_FLAT_MEMORY'] : []), ...definitions, '-Isrc/browser/hook-compat', '-Isrc', ...includes.map(path => `-I${path}`),
    ...sources, 'src/browser/hook-compat.cpp', 'src/browser/hook-log.cpp', 'src/browser/cpu_runtime.cpp',
    ...(wasm ? wasmFlags : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']), '-o', output], { encoding: 'utf8', timeout: 180000 });
  assert.equal(build.status, 0, build.stderr);
  if (wasm) { writeFileSync(`${hookDirectory}/${stem}-run.mjs`, `import create from './${stem}.mjs'; await create();\n`); }
  return spawnSync(wasm ? process.execPath : output, wasm ? [`${hookDirectory}/${stem}-run.mjs`] : [], { encoding: 'utf8', timeout: 60000 });
};
