import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, renameSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { adaptMemory } from '../../tools/browser/adapt-memory.mjs';
import { optimizerIdentity, publishWasm } from '../../tools/browser/tail-call-postlink.mjs';

test('bounded memory blocks preserve actual tail transfers and unwind faults', () => {
  const directory = resolve('out/memory-block-execution');
  mkdirSync(directory, { recursive: true });
  const operations = '\t{ uint32_t ea = 0x10000; ctx.eax = X86_MEM32(ea); }\n\t{ uint32_t ea = 0x10004; X86_MEM32(ea) = ctx.eax; }';
  const source = `#include "cpu/x86_ops.h"
#include <cstdio>
X86_EXTERN_FUNC(sub_00012000);
X86_FUNC(sub_00011000) {
${operations}
  if (ctx.edx) { --ctx.edx; sub_00012000(ctx, base); return; }
}
X86_FUNC(sub_00012000) {
${operations}
  sub_00011000(ctx, base); return;
}
X86_FUNC(Deep) {
${operations}
  if (ctx.edx) { --ctx.edx; Deep(ctx, base); }
  ++ctx.ebx;
}
int main() {
  browser::Memory memory;
  memory.Map(0x10000, 4096); memory.Commit(0x10000, 4096);
  X86Context ctx{}; ctx.edx = 100000;
  sub_00011000(ctx, memory.Base());
  if (ctx.edx || browser::Memory::HasBorrow()) { return 1; }
  ctx.edx = 4096;
  Deep(ctx, memory.Base());
  if (ctx.ebx != 4097 || browser::Memory::HasBorrow()) { return 3; }
  memory.Decommit(0x10000, 4096);
  bool rejected = false;
  try { sub_00011000(ctx, memory.Base()); } catch (const browser::Fault&) { rejected = true; }
  if (!rejected || browser::Memory::HasBorrow()) { return 2; }
  memory.Commit(0x10000, 4096);
  std::puts("memory-block-execution-ok");
}`;
  writeFileSync(`${directory}/guest.cpp`, adaptMemory(source, { blocks: true }));
  const run = (command, args) => {
    const result = spawnSync(command, args, { encoding: 'utf8', timeout: 120000 });
    assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
    return result;
  };
  run('em++', ['-std=c++20', '-O3', '-mtail-call', '-pthread', '-fwasm-exceptions', '-DX86_COMPACT_MEMORY', '-Isrc', `${directory}/guest.cpp`, 'src/browser/cpu_runtime.cpp', '-sENVIRONMENT=node', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sINITIAL_MEMORY=134217728', '-sSTACK_SIZE=1048576', '-sEXIT_RUNTIME=1', '-o', `${directory}/guest.mjs`]);
  const tool = resolve(run('em-config', ['BINARYEN_ROOT']).stdout.trim(), 'bin/wasm-opt');
  renameSync(`${directory}/guest.wasm`, `${directory}/pristine.wasm`);
  publishWasm({ input: `${directory}/pristine.wasm`, output: `${directory}/guest.wasm`, optimization: { ...optimizerIdentity(tool), tool } });
  writeFileSync(`${directory}/run.mjs`, "import create from './guest.mjs'; await create();\n");
  assert.match(run(process.execPath, [`${directory}/run.mjs`]).stdout, /memory-block-execution-ok/);
});
