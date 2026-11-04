import { test } from 'node:test';
import assert from 'node:assert/strict';
import { adaptMemory as adapt } from '../../tools/browser/adapt-memory.mjs';
import { adaptArithmetic } from '../../tools/browser/adapt-arithmetic.mjs';
import { readFileSync } from 'node:fs';

const instruction = '\t{ uint32_t ea = ctx.eax; ctx.ecx = X86_MEM32(ea); }';
const adaptMemory = source => adapt(source, { blocks: true });
const blocks = source => [...source.matchAll(/\.BeginBlock\(\);/g)].length;

test('straight guest instructions share bounded backing leases', () => {
  const source = Array.from({ length: 131 }, () => instruction).join('\n');
  const adapted = adaptMemory(source);
  assert.equal(blocks(adapted), 3);
  assert.equal(blocks(adapt(source)), 0);
  assert.equal(adaptMemory(adapted), adapted);
  const groups = adapted.split('.BeginBlock();').slice(1);
  assert.deepEqual(groups.map(group => [...group.matchAll(/ctx.ecx =/g)].length), [64, 64, 3]);
});

test('actual guest tail transfers are explicit and genuine calls retain their continuations', () => {
  const tail = '\t{ X86_SAVE_FLAGS(); sub_00011000(ctx, base); return; }';
  assert.equal(adaptMemory(tail), '\t{ X86_SAVE_FLAGS(); BROWSER_GUEST_TAIL sub_00011000(ctx, base); }');
  assert.equal(adapt(tail), tail);
  const call = '\tX86_PUSH32(0x1234); sub_00011000(ctx, base);\n\tctx.eax += 1;';
  assert.equal(adaptMemory(call), call);
});

test('pure context assignments and local arithmetic conditionals retain one bounded instruction block', () => {
  const source = `${instruction}\n\tctx.ebp = (uint32_t)(ctx.esp + 8u);\n\t{ uint32_t count = ctx.ecx & 31; if (count) { ctx.edx <<= count; } }\n${instruction}`;
  const adapted = adaptMemory(source);
  assert.equal(blocks(adapted), 1);
  assert.ok(adapted.includes(source));
  const unknown = `${instruction}\n\tctx.eax = unknown_helper(ctx);\n${instruction}`;
  assert.equal(blocks(adaptMemory(unknown)), 0);
});

test('original exception handlers retain their call and unwind boundaries', () => {
  const body = `X86_FUNC(__imp__sub_00011000) {\n\ttry {\n${instruction}\n${instruction}\n\t{ X86_SAVE_FLAGS(); sub_00012000(ctx, base); return; }\n\t} catch (const X86Resume&) { throw; }\n}`;
  assert.equal(adaptMemory(body), body);
});

test('leases end before calls, labels, waits, atomics, loops and unknown helpers', () => {
  for (const boundary of [
    '\t{ x86_icall(ctx, base, ctx.eax); }',
    '\t{ sub_00011000(ctx, base); }',
    '\t{ ctx.eax = x86_rdtsc(); }',
    '\t{ while (ctx.ecx) { --ctx.ecx; } }',
    '\t{ for (uint32_t n = 0; n < 2; ++n) { ctx.eax += n; } }',
    '\t{ uint32_t ea = ctx.eax; __atomic_exchange_n(X86_PTR(uint32_t, ea), 1, __ATOMIC_SEQ_CST); }',
    '\t{ unknown_helper(ctx); }',
    'loc_00011000:',
    '\tif (ctx.eax) { X86_BARRIER(); goto loc_00011000; }',
    '\treturn;',
  ]) {
    const source = `${instruction}\n${instruction}\n${boundary}\n${instruction}\n${instruction}`;
    const adapted = adaptMemory(source);
    assert.equal(blocks(adapted), 2, boundary);
    assert.ok(adapted.includes(`.EndBlock(); }\n${adaptMemory(boundary)}\n\t{ browser::Memory::From(base).BeginBlock();`), boundary);
  }
});

test('instruction comments remain adjacent for arithmetic adaptation', () => {
  const source = `\t// 00011000 mov eax, [eax]\n${instruction}\n\t// 00011002 mov ecx, [eax]\n${instruction}`;
  const adapted = adaptMemory(source);
  assert.equal(blocks(adapted), 1);
  assert.ok(adapted.includes(`\t// 00011000 mov eax, [eax]\n${instruction}`));
  assert.ok(adapted.includes(`\t// 00011002 mov ecx, [eax]\n${instruction}`));
  const boundary = '\t{ ctx.eax = x86_rdtsc(); }';
  const combined = adaptMemory(`${source}\n\t// 00011004 rdtsc\n${boundary}`);
  assert.ok(combined.includes(`\t// 00011004 rdtsc\n${boundary}`));
});

test('all retained generated partitions still compose with arithmetic adaptation', () => {
  for (let n = 0; n < 128; ++n) {
    const source = readFileSync(`generated/game/x86_recomp.${n}.cpp`, 'utf8');
    assert.ok(adaptArithmetic(adaptMemory(source)).length >= source.length);
  }
});
