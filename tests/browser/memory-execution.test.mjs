import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, renameSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { optimizerIdentity, publishWasm } from '../../tools/browser/tail-call-postlink.mjs';
import { resolve } from 'node:path';
const directory = 'out/memory-execution-tests';
mkdirSync(directory, { recursive: true });
for (const platform of ['native', 'wasm', 'wasm-tail', 'wasm-lto-tail']) {
  test(`${platform} guest execution releases protection at host boundaries and restores it after callbacks`, () => {
    const wasm = platform !== 'native';
    const output = `${directory}/${wasm ? `${platform}.mjs` : 'native'}`;
    const flags = wasm ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sINITIAL_MEMORY=201326592', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '-sEXIT_RUNTIME=1'] : ['-fsanitize=address,undefined'];
    if (platform.endsWith('-tail')) { flags.push('-mtail-call'); }
    if (platform.includes('lto')) { flags.push('-flto', '-msimd128'); }
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O3', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/memory-execution.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    if (platform.endsWith('-tail')) {
      const tool = resolve(spawnSync('em-config', ['BINARYEN_ROOT'], { encoding: 'utf8' }).stdout.trim(), 'bin/wasm-opt');
      const input = output.replace(/\.mjs$/, '-pristine.wasm');
      const final = output.replace(/\.mjs$/, '.wasm');
      renameSync(final, input);
      publishWasm({ input, output: final, optimization: { ...optimizerIdentity(tool, { simd: platform.includes('lto') }), tool } });
    }
    writeFileSync(`${directory}/run.mjs`, `import create from './${platform}.mjs'; await create();\n`);
    const result = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(result.status, 0, result.stdout + result.stderr);
    assert.match(result.stdout, /memory-execution-contracts-ok/);
  });
}
