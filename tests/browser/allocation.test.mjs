import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/allocation-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './allocation.mjs'; await create();\n");
writeFileSync(`${directory}/index.html`, `<!doctype html><meta charset="utf-8"><title>Allocator contracts</title><pre id="result">Running</pre><script type="module">
import create from './allocation.mjs';
const result = document.querySelector('#result');
window.allocationEvidence = { isolated: crossOriginIsolated, lines: [], exit: null, heartbeats: 0 };
const heartbeat = setInterval(() => { ++window.allocationEvidence.heartbeats; }, 1);
const timeout = setTimeout(() => { result.textContent = 'Allocator worker timeout'; }, 30000);
const print = text => { window.allocationEvidence.lines.push(text); result.textContent = window.allocationEvidence.lines.join('\\n'); };
create({print, printErr: print, onExit: status => { clearTimeout(timeout); clearInterval(heartbeat); window.allocationEvidence.exit = status; }})
  .catch(error => { clearTimeout(timeout); clearInterval(heartbeat); result.textContent = String(error); });
</script>`);

for (const platform of ['native', 'wasm']) {
  test(`${platform} compact allocator preserves guest contracts`, () => {
    const output = `${directory}/${platform === 'wasm' ? 'allocation.mjs' : 'allocation-native'}`;
    const flags = platform === 'wasm'
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=web,node', '-sINITIAL_MEMORY=134217728', '-sPTHREAD_POOL_SIZE=1', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sSTACK_SIZE=1048576', '-sDEFAULT_PTHREAD_STACK_SIZE=262144', '-sSTACK_OVERFLOW_CHECK=2', '-sEXIT_RUNTIME=1']
      : [];
    const build = spawnSync(platform === 'wasm' ? 'em++' : 'clang++', ['-std=c++20', '-O3', '-pthread', '-fno-strict-aliasing', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/allocation.cpp', 'src/kernel/memory.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const execution = platform === 'wasm'
      ? spawnSync(process.execPath, [`${directory}/run.mjs`], { encoding: 'utf8', timeout: 30000 })
      : spawnSync(output, [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(execution.status, 0, execution.stderr);
    assert.match(execution.stdout, /allocation-contracts-ok/);
    writeFileSync(`${directory}/${platform}-results.txt`, execution.stdout);
  });
}
