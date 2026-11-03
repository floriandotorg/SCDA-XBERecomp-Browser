import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, renameSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { adaptMemory } from '../../tools/browser/adapt-memory.mjs';
import { adaptArithmetic } from '../../tools/browser/adapt-arithmetic.mjs';
import { optimizerIdentity, publishWasm } from '../../tools/browser/tail-call-postlink.mjs';

const directory = resolve('out/cpu-tests');
mkdirSync(directory, { recursive: true });
const fixtures = [
  ['generated/game/x86_recomp.0.cpp', 'sub_00011000'],
  ['generated/game/x86_recomp.98.cpp', 'sub_003E0540'],
  ['generated/game/x86_recomp.38.cpp', 'sub_0019B695'],
  ['generated/game/x86_recomp.34.cpp', 'sub_0016E504'],
  ['generated/game/x86_recomp.0.cpp', 'sub_00011013'],
  ['generated/game/x86_recomp.69.cpp', 'sub_002CDA10'],
  ['generated/game/x86_recomp.69.cpp', 'sub_002CDA25'],
  ['generated/game/x86_recomp.109.cpp', 'sub_00452604'],
  ['generated/game/x86_recomp.109.cpp', 'sub_00452449'],
  ['generated/game/x86_recomp.84.cpp', 'sub_00365CA1'],
  ['generated/game/x86_recomp.75.cpp', 'sub_00309660'],
];
const header = '#include "cpu/x86_ops.h"\nX86_EXTERN_FUNC(sub_002CDA25);\nX86_EXTERN_FUNC(sub_002CD410);\nX86_EXTERN_FUNC(sub_002CD860);\n';
const bodies = fixtures.map(([path, name]) => {
  const source = readFileSync(path, 'utf8');
  const start = source.indexOf(`X86_FUNC(__imp__${name})`);
  const marker = `X86_WEAK_FUNC(${name}, __imp__${name})`;
  const end = source.indexOf(marker, start);
  assert.ok(start >= 0 && end > start, `Missing local fixture ${name}`);
  return adaptArithmetic(adaptMemory(source.slice(start, end + marker.length)));
});
writeFileSync(`${directory}/fixtures.cpp`, `${header}${bodies.join('\n')}`);
writeFileSync(`${directory}/run.mjs`, "import create from './cpu.mjs'; await create();\n");
writeFileSync(`${directory}/index.html`, readFileSync('tests/browser/full-cpu.html', 'utf8').replaceAll('cpu_contracts.mjs', 'cpu.mjs').replaceAll('fullCpuEvidence', 'cpuEvidence').replaceAll('Full CPU contracts', 'CPU runtime tests').replaceAll('Running complete story metadata CPU contracts', 'Running CPU contracts'));

for (const [name, fixture, expected] of [
  ['bad-memory', 0, '00011004..0001100F sparse stores'],
  ['bad-arithmetic', 3, '0016E504 signed multiply wraps at 32 bits'],
]) {
  test(`independent oracle rejects ${name} with the guest operation`, () => {
    const mutated = [...bodies];
    mutated[fixture] = fixture === 0
      ? mutated[fixture].replace('X86_MEM32(ea) = (uint32_t)(ctx.ecx);', 'X86_MEM32(ea) = 1;')
      : mutated[fixture].replace('0x1Cu); uint32_t r', '0x18u); uint32_t r');
    assert.notEqual(mutated[fixture], bodies[fixture]);
    const source = `${directory}/${name}.cpp`;
    writeFileSync(source, `${header}${mutated.join('\n')}`);
    const output = `${directory}/${name}`;
    const build = spawnSync('clang++', ['-std=c++20', '-O3', '-pthread', '-fno-strict-aliasing', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/cpu.cpp', 'src/browser/cpu_runtime.cpp', source, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const execution = spawnSync(output, [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(execution.status, 1, execution.stderr);
    assert.ok(execution.stderr.includes(expected), execution.stderr);
  });
}

for (const platform of ['native', 'wasm', 'wasm-tail']) {
  test(`${platform} CPU contracts and real generated fixtures`, () => {
    const wasm = platform !== 'native';
    const output = `${directory}/${wasm ? platform === 'wasm-tail' ? 'cpu-tail.mjs' : 'cpu.mjs' : 'cpu-native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=web,node', '-sINITIAL_MEMORY=134217728', '-sALLOW_MEMORY_GROWTH=0', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '-sSTACK_OVERFLOW_CHECK=2', '-sEXIT_RUNTIME=1']
      : [];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O3', '-pthread', '-fno-strict-aliasing', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/cpu.cpp', 'src/browser/cpu_runtime.cpp', `${directory}/fixtures.cpp`, ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    if (platform === 'wasm-tail') {
      const tool = resolve(spawnSync('em-config', ['BINARYEN_ROOT'], { encoding: 'utf8' }).stdout.trim(), 'bin/wasm-opt');
      const input = output.replace(/\.mjs$/, '-pristine.wasm');
      const final = output.replace(/\.mjs$/, '.wasm');
      renameSync(final, input);
      publishWasm({ input, output: final, optimization: { ...optimizerIdentity(tool), tool } });
    }
    if (wasm) { writeFileSync(`${directory}/${platform}-run.mjs`, `import create from 'file://${output}'; await create();\n`); }
    const execution = wasm
      ? spawnSync(process.execPath, [`${directory}/${platform}-run.mjs`], { encoding: 'utf8', timeout: 30000 })
      : spawnSync(output, [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(execution.status, 0, execution.stderr);
    assert.match(execution.stdout, /cpu-runtime-ok/);
    writeFileSync(`${directory}/${platform}-results.txt`, execution.stdout);
  });
}
