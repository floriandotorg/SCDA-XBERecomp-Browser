import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/arithmetic-tests');
const instructions = new Map([
  ['shl eax, cl', [32, 'eax', 0]], ['shl al, cl', [8, 'eax', 0]], ['shl bx, cl', [16, 'ebx', 0]],
  ['shr eax, cl', [32, 'eax', 0]], ['shr al, cl', [8, 'eax', 0]], ['shr ax, cl', [16, 'eax', 0]],
  ['sar eax, cl', [32, 'eax', 0]], ['sar ax, cl', [16, 'eax', 0]],
  ['shld edx, eax, cl', [32, 'edx', 0]], ['shrd eax, edx, cl', [32, 'eax', 0]],
  ['rol cl, 0x01', [8, 'ecx', 0]], ['rol ch, 0x01', [8, 'ecx', 8]],
  ['ror ecx, 0x10', [32, 'ecx', 0]], ['ror ax, 0x08', [16, 'eax', 0]],
  ['rcr eax, 0x01', [32, 'eax', 0]], ['rcl ah, 0x9B', [8, 'eax', 8]],
  ['add eax, ecx', [32, 'eax', 0]], ['add al, cl', [8, 'eax', 0]], ['add ax, cx', [16, 'eax', 0]],
  ['sub eax, ecx', [32, 'eax', 0]], ['sbb eax, ecx', [32, 'eax', 0]], ['sbb eax, 0xFFFFFFFF', [32, 'eax', 0]],
  ['adc al, 0x03', [8, 'eax', 0]], ['adc eax, 0x80000000', [32, 'eax', 0]],
  ['neg al', [8, 'eax', 0]], ['neg ax', [16, 'eax', 0]], ['neg eax', [32, 'eax', 0]],
  ['inc al', [8, 'eax', 0]], ['inc ax', [16, 'eax', 0]], ['inc eax', [32, 'eax', 0]],
  ['dec al', [8, 'eax', 0]], ['dec ax', [16, 'eax', 0]], ['dec eax', [32, 'eax', 0]],
]);
const selected = new Map();
for (let n = 0; n < 128; ++n) {
  const lines = readFileSync(`generated/game/x86_recomp.${n}.cpp`, 'utf8').split('\n');
  for (let n = 0; n < lines.length - 1; ++n) {
    const instruction = /\/\/ [0-9A-F]{8} (.*)/.exec(lines[n])?.[1];
    if (instructions.has(instruction) && !selected.has(instruction)) {
      selected.set(instruction, `${lines[n]}\n${lines[n + 1]}`);
    }
  }
}
assert.equal(selected.size, instructions.size);
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/index.html`, readFileSync('tests/browser/full-cpu.html', 'utf8').replaceAll('cpu_contracts.mjs', 'arithmetic.mjs').replaceAll('fullCpuEvidence', 'arithmeticEvidence').replaceAll('Full CPU contracts', 'Emitted arithmetic contracts').replaceAll('Running complete story metadata CPU contracts', 'Running emitted arithmetic contracts'));
const fixtures = [...instructions].map(([instruction, [width, destination, shift]], n) => {
  const count = instruction.includes(', ') ? instruction.split(', ').at(-1) : '0';
  return { n, operation: instruction.split(' ')[0], width, destination, shift, count: ['cl', 'cx', 'ecx'].includes(count) ? -1 : Number(count), body: selected.get(instruction) };
});
const source = `#include "arithmetic.h"\n${fixtures.map(({ n, body }) => `X86_FUNC(arithmetic_${n}) { X86_LOAD_FLAGS();\n${body}\nX86_SAVE_FLAGS(); }`).join('\n')}\nstd::span<const ArithmeticFixture> ArithmeticFixtures() {\nstatic const ArithmeticFixture fixtures[] = {${fixtures.map(({ n, operation, width, shift, destination, count }) => `{"${operation}", ${width}, ${shift}, &X86Context::${destination}, ${count}, arithmetic_${n}}`).join(',')}};\nreturn fixtures;\n}\n`;

for (const platform of ['native', 'wasm']) {
  test(`${platform} emitted arithmetic matches independent instruction oracle`, async () => {
    const { adaptArithmetic } = await import('../../tools/browser/adapt-arithmetic.mjs');
    const fixturePath = `${directory}/fixtures.cpp`;
    writeFileSync(fixturePath, adaptArithmetic(source));
    const output = `${directory}/${platform === 'wasm' ? 'arithmetic.mjs' : 'native'}`;
    const flags = platform === 'wasm' ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=web,node', '-sEXIT_RUNTIME=1'] : [];
    const build = spawnSync(platform === 'wasm' ? 'em++' : 'clang++', ['-std=c++20', '-O3', '-DX86_PORTABLE_RUNTIME', '-Isrc', '-Itests/browser', 'tests/browser/arithmetic.cpp', fixturePath, ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    writeFileSync(`${directory}/run.mjs`, "import create from './arithmetic.mjs'; await create();\n");
    const execution = platform === 'wasm' ? spawnSync(process.execPath, [`${directory}/run.mjs`], { encoding: 'utf8', timeout: 30000 }) : spawnSync(output, [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(execution.status, 0, execution.stderr);
    assert.match(execution.stdout, new RegExp(`arithmetic-contracts-ok fixtures=${instructions.size}`));
    writeFileSync(`${directory}/${platform}-results.txt`, execution.stdout);
  });
}
writeFileSync(`${directory}/unadapted.cpp`, source);

test('independent oracle rejects the original missing carry rotation overflow', () => {
  const output = `${directory}/unadapted`;
  const build = spawnSync('clang++', ['-std=c++20', '-O3', '-DX86_PORTABLE_RUNTIME', '-Isrc', '-Itests/browser', 'tests/browser/arithmetic.cpp', `${directory}/unadapted.cpp`, '-o', output], { encoding: 'utf8', timeout: 120000 });
  assert.equal(build.status, 0, build.stderr);
  const result = spawnSync(output, [], { encoding: 'utf8', timeout: 30000 });
  assert.equal(result.status, 1);
  assert.match(result.stderr, /rcr32 value=00000000 count=1 overflow/);
});

test('carry rotation adapter is idempotent and rejects unknown generated forms', async () => {
  const { adaptArithmetic } = await import('../../tools/browser/adapt-arithmetic.mjs');
  const adapted = adaptArithmetic(source);
  assert.equal(adaptArithmetic(adapted), adapted);
  assert.throws(() => adaptArithmetic('// 00000001 rcr eax, 0x01\n{ invalid; }'), /Unsupported generated carry rotation/);
});
