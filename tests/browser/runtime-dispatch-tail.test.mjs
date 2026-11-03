import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, renameSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { optimizerIdentity, publishWasm } from '../../tools/browser/tail-call-postlink.mjs';

const directory = resolve('out/m4-current/runtime-dispatch-tail-tests');
mkdirSync(directory, { recursive: true });
const tool = resolve(spawnSync('em-config', ['BINARYEN_ROOT'], { encoding: 'utf8' }).stdout.trim(), 'bin/wasm-opt');
const optimization = { ...optimizerIdentity(tool), tool };
const postlink = (output) => {
  const input = output.replace(/\.wasm$/, '-pristine.wasm');
  renameSync(output, input);
  publishWasm({ input, output, optimization });
};
const run = (command, arguments_, name) => {
  const result = spawnSync(command, arguments_, { encoding: 'utf8', timeout: 120000 });
  writeFileSync(`${directory}/${name}.log`, `${result.stdout ?? ''}${result.stderr ?? ''}`);
  assert.equal(result.status, 0, result.stderr);
  return result.stdout;
};

test('deep genuine calls complete without enlarged heap, guest stack or host stack limits', () => {
  const flags = ['-std=c++20', '-O3', '-pthread', '-fwasm-exceptions', '-DX86_COMPACT_MEMORY', '-Isrc'];
  run('em++', [...flags, '-fno-optimize-sibling-calls', '-DPUBLIC_GUEST_DEPTH=4096', '-c', 'tests/browser/runtime-dispatch-tail.cpp', '-o', `${directory}/deep-guest.o`], 'deep-guest-build');
  for (const enabled of [false, true]) {
    const name = enabled ? 'deep-tail' : 'deep-ordinary';
    run('em++', [...flags, 'src/browser/cpu_runtime.cpp', `${directory}/deep-guest.o`, '-sENVIRONMENT=node', '-sEXIT_RUNTIME=1',
      '-sINITIAL_MEMORY=67108864', '-sMAXIMUM_MEMORY=268435456', '-sALLOW_MEMORY_GROWTH=1',
      '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '--profiling-funcs', '-o', `${directory}/${name}.mjs`], `${name}-build`);
    if (enabled) { postlink(`${directory}/${name}.wasm`); }
    writeFileSync(`${directory}/${name}-run.mjs`, `import create from './${name}.mjs'; await create();\n`);
    const result = spawnSync(process.execPath, [`${directory}/${name}-run.mjs`], { encoding: 'utf8', timeout: 30000 });
    writeFileSync(`${directory}/${name}-run.log`, `${result.stdout ?? ''}${result.stderr ?? ''}`);
    if (enabled) {
      assert.equal(result.status, 0, result.stderr);
      assert.deepEqual(JSON.parse(result.stdout.trim()), { invoke: 0, indirect: 0, guest: 4097 });
    } else {
      assert.notEqual(result.status, 0);
      assert.match(result.stderr, /Maximum call stack size exceeded/);
    }
  }
});

test('host dispatch tail calls retire only redundant wrappers and preserve real guest calls and post-call work', () => {
  const flags = ['-std=c++20', '-O3', '-pthread', '-fwasm-exceptions', '-DX86_COMPACT_MEMORY', '-Isrc'];
  run('em++', [...flags, '-fno-optimize-sibling-calls', '-c', 'tests/browser/runtime-dispatch-tail.cpp', '-o', `${directory}/guest.o`], 'guest-build');
  for (const enabled of [false, true]) {
    const name = enabled ? 'tail' : 'ordinary';
    run('em++', [...flags, '-c', 'src/browser/cpu_runtime.cpp', '-o', `${directory}/${name}.o`], `${name}-runtime-build`);
    run('em++', [...flags, `${directory}/guest.o`, `${directory}/${name}.o`, '-sENVIRONMENT=node', '-sEXIT_RUNTIME=1',
      '-sINITIAL_MEMORY=67108864', '-sMAXIMUM_MEMORY=268435456', '-sALLOW_MEMORY_GROWTH=1',
      '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '--profiling-funcs', '-o', `${directory}/${name}.mjs`], `${name}-link`);
    if (enabled) { postlink(`${directory}/${name}.wasm`); }
    const output = run(process.execPath, ['--input-type=module', '--eval', `const { default: create } = await import('file://${directory}/${name}.mjs'); await create();`], `${name}-run`);
    const counts = JSON.parse(output.trim());
    assert.equal(counts.guest, 33);
    if (enabled) {
      assert.equal(counts.invoke, 0);
      assert.equal(counts.indirect, 0);
    } else {
      assert.ok(counts.invoke + counts.indirect >= 32);
    }
  }
});
