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
X86_FUNC(Leaf) {
${operations}
  if (ctx.ecx) { goto loc_00013010; }
  ctx.eax = 19;
  ctx.esp += 4; return;
loc_00013010:
  ctx.eax = 37;
  ctx.esp += 4; return;
}
X86_FUNC(VectorLeaf) {
  { uint32_t ea = 0x10000; ctx.xmm[0] = *X86_PTR(X86Xmm, ea); }
  { uint32_t ea = 0x10010; *X86_PTR(X86Xmm, ea) = ctx.xmm[0]; }
  ctx.esp += 4; return;
}
X86_FUNC(StackLeaf) {
  { X86_PUSH32(ctx.eax); }
  { uint32_t value = X86_MEM32(ctx.esp); ctx.edx = value; ctx.esp += 4; }
  ctx.esp += 4; return;
}
int main() {
  browser::Memory memory;
  memory.Map(0x10000, 4096); memory.Commit(0x10000, 4096);
  X86Context ctx{}; ctx.edx = 100000;
  for (uint32_t n = 0; n < 2; ++n) {
    ctx.ecx = n;
    Leaf(ctx, memory.Base());
    if (ctx.eax != (n ? 37 : 19) || browser::Memory::HasBorrow()) { return 4; }
  }
  VectorLeaf(ctx, memory.Base());
  const auto vector = memory.Load<X86Xmm>(ctx, 0x10010);
  for (uint32_t n = 0; n < 4; ++n) { if (vector.u[n] != ctx.xmm[0].u[n]) { return 5; } }
  ctx.esp = 0x10f00; ctx.eax = 0x12345678;
  StackLeaf(ctx, memory.Base());
  if (ctx.edx != ctx.eax || ctx.esp != 0x10f04 || browser::Memory::HasBorrow()) { return 6; }
  ctx.edx = 100000;
  sub_00011000(ctx, memory.Base());
  if (ctx.edx || browser::Memory::HasBorrow()) { return 1; }
  ctx.edx = 4096;
  Deep(ctx, memory.Base());
  if (ctx.ebx != 4097 || browser::Memory::HasBorrow()) { return 3; }
  memory.Decommit(0x10000, 4096);
  for (const auto function : {Leaf, sub_00011000}) {
    bool rejected = false;
    try { function(ctx, memory.Base()); } catch (const browser::Fault&) { rejected = true; }
    if (!rejected || browser::Memory::HasBorrow()) { return 2; }
  }
  memory.Commit(0x10000, 4096);
  std::puts("memory-block-execution-ok");
}`;
  const adapted = adaptMemory(source, { blocks: true });
  assert.match(adapted, /X86_FUNC\(Leaf\) \{\n\s*browser::Memory::BlockScope guestMemoryBlock/);
  assert.match(adapted, /browser::BlockPointer<X86Xmm>\(guestMemoryBlock, ctx,/);
  assert.match(adapted, /browser::BlockPush32\(guestMemoryBlock, ctx,/);
  writeFileSync(`${directory}/guest.cpp`, adapted);
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
