import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, renameSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { optimizerIdentity, publishWasm } from '../../tools/browser/tail-call-postlink.mjs';

const directory = resolve('out/m4-resume/synchronization/provider-tests');
mkdirSync(directory, { recursive: true });
const sources = ['tests/browser/synchronization-provider.cpp', 'src/browser/synchronization-provider.cpp',
  'src/browser/dispatcher.cpp', 'src/browser/dispatcher-handles.cpp', 'src/browser/dispatcher-object-abi.cpp',
  'src/browser/guest-wait.cpp', 'src/browser/thread-provider.cpp', 'src/browser/cpu_runtime.cpp', 'src/browser/image.cpp',
  'src/browser/file-provider.cpp', 'src/browser/raw-cache.cpp', 'src/browser/cache-fatx.cpp',
  'src/browser/temporary-filesystem.cpp', 'src/kernel/disc_index.cpp'];
for (const platform of ['native', 'wasm', 'wasm-tail']) {
  test(`${platform} installed production synchronization owns handles, completion, real waits and cancellation`, () => {
    const wasm = platform !== 'native';
    const output = `${directory}/${platform}${wasm ? '.mjs' : ''}`;
    const flags = wasm ? ['-fwasm-exceptions', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456',
      '-sMAXIMUM_MEMORY=268435456', '-sALLOW_MEMORY_GROWTH=0', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2',
      '-sPROXY_TO_PTHREAD=1', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '-sEXIT_RUNTIME=1'] :
      ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME',
      '-DX86_COMPACT_MEMORY', '-Isrc', ...sources, ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    writeFileSync(`${directory}/${platform}-build.log`, `${build.stdout ?? ''}${build.stderr ?? ''}`);
    assert.equal(build.status, 0, build.stderr);
    if (platform === 'wasm-tail') {
      const tool = resolve(spawnSync('em-config', ['BINARYEN_ROOT'], { encoding: 'utf8' }).stdout.trim(), 'bin/wasm-opt');
      const input = `${directory}/wasm-tail-pristine.wasm`;
      renameSync(`${directory}/wasm-tail.wasm`, input);
      publishWasm({ input, output: `${directory}/wasm-tail.wasm`, optimization: { ...optimizerIdentity(tool), tool } });
    }
    writeFileSync(`${directory}/${platform}-run.mjs`, `import create from './${platform}.mjs'; await create();\n`);
    const result = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/${platform}-run.mjs`] : [],
      { encoding: 'utf8', timeout: 30000 });
    writeFileSync(`${directory}/${platform}-run.log`, `${result.stdout ?? ''}${result.stderr ?? ''}`);
    assert.equal(result.status, 0, result.stderr);
    assert.match(result.stdout, /synchronization-provider-contracts-ok/);
  });
}
