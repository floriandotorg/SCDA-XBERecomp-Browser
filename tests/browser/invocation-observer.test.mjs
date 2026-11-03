import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/m4-current/invocation-observer-tests');
mkdirSync(directory, { recursive: true });
for (const platform of ['native', 'wasm', 'wasm-tail']) {
  test(`${platform} cold image invocation evidence preserves resolved dispatch, exceptions and retirement`, () => {
    const wasm = platform !== 'native';
    const output = `${directory}/${platform}${wasm ? '.mjs' : ''}`;
    const flags = wasm ? ['-fwasm-exceptions', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=67108864',
      '-sMAXIMUM_MEMORY=268435456', '-sALLOW_MEMORY_GROWTH=1', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2',
      '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1'] : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    if (platform === 'wasm-tail') { flags.push('-mtail-call', '-sBINARYEN_EXTRA_PASSES=tail-call'); }
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME',
      '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/invocation-observer.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output],
    { encoding: 'utf8', timeout: 120000 });
    writeFileSync(`${directory}/${platform}-build.log`, `${build.stdout ?? ''}${build.stderr ?? ''}`);
    assert.equal(build.status, 0, build.stderr);
    writeFileSync(`${directory}/${platform}-run.mjs`, `import create from './${platform}.mjs'; await create();\n`);
    const execution = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/${platform}-run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    writeFileSync(`${directory}/${platform}-run.log`, `${execution.stdout ?? ''}${execution.stderr ?? ''}`);
    assert.equal(execution.status, 0, execution.stderr);
    assert.match(execution.stdout, /invocation-observer-ok/);
  });
}
