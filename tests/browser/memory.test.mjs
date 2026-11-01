import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/memory-concurrency');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './memory.mjs'; await create();\n");
writeFileSync(`${directory}/index.html`, `<!doctype html><meta charset="utf-8"><title>Memory concurrency</title><pre id="result">Running</pre><script type="module">
import create from './memory.mjs';
const result = document.querySelector('#result');
window.memoryEvidence = { isolated: crossOriginIsolated, lines: [], heartbeats: 0, exit: null };
const heartbeat = setInterval(() => { ++window.memoryEvidence.heartbeats; }, 10);
if (!crossOriginIsolated || typeof SharedArrayBuffer === 'undefined') {
  clearInterval(heartbeat);
  result.textContent = 'Memory error: cross origin isolation required';
} else {
  const timeout = setTimeout(() => { result.textContent = 'Memory error: worker completion timeout'; }, 30000);
  const print = text => { window.memoryEvidence.lines.push(text); result.textContent = window.memoryEvidence.lines.join('\\n'); };
  create({ print, printErr: print, onExit: status => { clearTimeout(timeout); clearInterval(heartbeat); window.memoryEvidence.exit = status; } })
    .catch(error => { clearTimeout(timeout); clearInterval(heartbeat); result.textContent = 'Memory error: ' + error; });
}
</script>`);

for (const platform of ['native', 'wasm']) {
  test(`${platform} concurrent backing and host span lifetime contracts`, () => {
    const output = `${directory}/${platform === 'wasm' ? 'memory.mjs' : 'native'}`;
    const flags = platform === 'wasm'
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=web,node', '-sINITIAL_MEMORY=134217728', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '-sSTACK_OVERFLOW_CHECK=2', '-sEXIT_RUNTIME=1']
      : [];
    const build = spawnSync(platform === 'wasm' ? 'em++' : 'clang++', ['-std=c++20', '-O3', '-pthread', '-fno-strict-aliasing', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/memory-concurrency.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const execution = platform === 'wasm'
      ? spawnSync(process.execPath, [`${directory}/run.mjs`], { encoding: 'utf8', timeout: 30000 })
      : spawnSync(output, [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(execution.status, 0, execution.stderr);
    assert.match(execution.stdout, /memory-concurrency-ok total=20000/);
    writeFileSync(`${directory}/${platform}-results.txt`, execution.stdout);
  });
}

test('generated raw pointers receive bounded backing leases without changing inputs', async () => {
  const { adaptMemory } = await import('../../tools/browser/adapt-memory.mjs');
  const source = '{ uint32_t ea = 4; uint32_t* p = X86_PTR(uint32_t, ea); ++*p; }\n{ uint32_t ea = 8; ctx.xmm[0] = *X86_PTR(X86Xmm, ea); }';
  const adapted = adaptMemory(source);
  assert.match(adapted, /\{ auto guestMemoryLease = browser::Memory::From\(base\).Borrow\(\); uint32_t ea = 4;/);
  assert.ok(adapted.endsWith(source.split('\n')[1]));
  assert.throws(() => adaptMemory('uint32_t* escaped = X86_PTR(uint32_t, 4);'), /Unscoped generated host pointer/);
  assert.throws(() => adaptMemory('{ uint32_t ea = 4; auto* p = X86_PTR(Unknown, ea); }'), /Unsupported generated host pointer type/);
  assert.equal(adaptMemory(adapted), adapted);
  let pointers = 0;
  for (let n = 0; n < 128; ++n) {
    const original = readFileSync(`generated/game/x86_recomp.${n}.cpp`, 'utf8');
    const translated = adaptMemory(original);
    const expected = [...original.matchAll(/X86_PTR\((?:uint8_t|uint32_t),/g)].length;
    assert.equal([...translated.matchAll(/auto guestMemoryLease =/g)].length, expected);
    pointers += expected;
  }
  assert.ok(pointers > 0);
});
