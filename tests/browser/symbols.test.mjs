import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/browser-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/index.html`, readFileSync(new URL('./symbols.html', import.meta.url)));
const compile = (name, sources) => {
  const paths = sources.map((source, n) => {
    const path = `${directory}/${name}-${n}.cpp`;
    writeFileSync(path, source);
    return path;
  });
  return spawnSync('em++', ['-std=c++20', '-O2', '-fwasm-exceptions', '-pthread', '-DX86_GAME_CODE', '-Isrc', ...paths, '-o', `${directory}/${name}.mjs`], { encoding: 'utf8' });
};
const bodies = `#include "cpu/x86_ops.h"
X86_FUNC(__imp__ordinary) { ctx.eax += 3; }
X86_WEAK_FUNC(ordinary, __imp__ordinary)
X86_FUNC(__imp__replaced) { ctx.eax += 7; }
X86_WEAK_FUNC(replaced, __imp__replaced)
X86Func* mapping[] = {ordinary, replaced};
X86_FUNC(generated_caller) { replaced(ctx, base); }
X86_FUNC(mapped_caller) { mapping[1](ctx, base); }
`;

test('Wasm direct calls, map calls and original body dispatch survive a strong override', () => {
  const result = compile('dispatch', [bodies, `#include "cpu/x86_ops.h"
#include <cstdio>
X86_EXTERN_FUNC(ordinary);
X86_EXTERN_FUNC(replaced);
X86_EXTERN_FUNC(__imp__replaced);
X86_EXTERN_FUNC(generated_caller);
X86_EXTERN_FUNC(mapped_caller);
X86_FUNC(replaced) { __imp__replaced(ctx, base); ctx.eax *= 2; }
int main() {
  X86Context ctx{};
  ordinary(ctx, nullptr);
  if (ctx.eax != 3) { return 1; }
  generated_caller(ctx, nullptr);
  if (ctx.eax != 20) { return 2; }
  mapped_caller(ctx, nullptr);
  if (ctx.eax != 54) { return 3; }
  __imp__replaced(ctx, nullptr);
  if (ctx.eax != 61) { return 4; }
  try { throw X86Resume{123, 456}; }
  catch (const X86Resume& resume) {
    if (resume.target != 123 || resume.esp != 456) { return 5; }
    std::puts("symbol-dispatch-ok");
    return 0;
  }
}`]);
  assert.equal(result.status, 0, result.stderr);
  const execution = spawnSync(process.execPath, ['--input-type=module', '-e', `import create from '${directory}/dispatch.mjs'; await create();`], { encoding: 'utf8' });
  assert.equal(execution.status, 0, execution.stderr);
  assert.match(execution.stdout, /symbol-dispatch-ok/);
});

test('missing and duplicate strong symbols fail with their names', () => {
  const missing = compile('missing', ['extern "C" void missing_guest_service(); int main() { missing_guest_service(); }']);
  assert.notEqual(missing.status, 0);
  assert.match(missing.stderr, /undefined symbol: missing_guest_service/);
  const duplicate = compile('duplicate', ['extern "C" void duplicate_guest() {} int main() {}', 'extern "C" void duplicate_guest() {}']);
  assert.notEqual(duplicate.status, 0);
  assert.match(duplicate.stderr, /duplicate symbol: duplicate_guest/);
});
