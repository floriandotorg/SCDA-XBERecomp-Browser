import { test } from 'node:test';
import assert from 'node:assert/strict';
import { adaptMemory as adapt } from '../../tools/browser/adapt-memory.mjs';
import { adaptArithmetic } from '../../tools/browser/adapt-arithmetic.mjs';
import { readFileSync } from 'node:fs';

const instruction = '\t{ uint32_t ea = ctx.eax; ctx.ecx = X86_MEM32(ea); }';
const adaptMemory = source => adapt(source, { blocks: true });
const blocks = source => [...source.matchAll(/BlockScope guestInstructionBlock/g)].length;
const instructions = source => source.replace(/browser::BlockAccess<uint32_t>\(guestInstructionBlock, ctx, /g, 'X86_MEM32(').replace(/browser::BlockPointer<X86Xmm>\(guestInstructionBlock, ctx, /g, 'X86_PTR(X86Xmm,');

test('bounded acyclic leaf functions hold one exception-safe lease across their branches', () => {
  const leaf = `X86_FUNC(__imp__sub_00011000) {\n\tX86_LOAD_FLAGS();\n${instruction}\n\tif (ctx.ecx) { goto loc_00011010; }\n${instruction}\n\tctx.esp += 4; return;\nloc_00011010:\n${instruction}\n\tctx.esp += 4; return;\n}`;
  const adapted = adaptMemory(leaf);
  assert.ok(adapted.includes('browser::Memory::BlockScope guestMemoryBlock{browser::Memory::From(base)};'));
  assert.ok(adapted.includes('browser::BlockAccess<uint32_t>(guestMemoryBlock, ctx, ea)'));
  assert.equal(blocks(adapted), 0);
  assert.equal(adaptMemory(adapted), adapted);
  assert.equal(adapt(leaf), leaf);
  for (const boundary of ['unknown_helper(ctx);', 'sub_00012000(ctx, base);', 'X86_BARRIER();', 'goto loc_00011010;', '{ uint32_t ea = ctx.esp; ctx.eax = *X86_PTR(uint32_t, ea); }']) {
    const unsafe = leaf.replace('ctx.esp += 4; return;\n}', `${boundary}\n\tctx.esp += 4; return;\n}`);
    assert.equal(adaptMemory(unsafe).includes('BlockScope guestMemoryBlock'), false, boundary);
  }
  const large = leaf.replace(instruction, Array.from({ length: 70 }, () => instruction).join('\n'));
  assert.equal(adaptMemory(large).includes('BlockScope guestMemoryBlock'), false);
});

test('straight guest instructions share bounded backing leases', () => {
  const source = Array.from({ length: 131 }, () => instruction).join('\n');
  const adapted = adaptMemory(source);
  assert.equal(blocks(adapted), 3);
  assert.equal(blocks(adapt(source)), 0);
  assert.equal(adaptMemory(adapted), adapted);
  const groups = adapted.split('BlockScope guestInstructionBlock').slice(1);
  assert.deepEqual(groups.map(group => [...group.matchAll(/ctx.ecx =/g)].length), [64, 64, 3]);
});

test('actual guest tail transfers are explicit and genuine calls retain their continuations', () => {
  const tail = '\t{ X86_SAVE_FLAGS(); sub_00011000(ctx, base); return; }';
  assert.equal(adaptMemory(tail), '\t{ X86_SAVE_FLAGS(); BROWSER_GUEST_TAIL sub_00011000(ctx, base); }');
  assert.equal(adapt(tail), tail);
  const call = '\tX86_PUSH32(0x1234); sub_00011000(ctx, base);\n\tctx.eax += 1;';
  assert.equal(adaptMemory(call), call);
});

test('mission repeated memory operations use checked bounded helpers outside instruction leases', () => {
  for (const [name, replacement] of [['movs', 'MoveRepeated'], ['stos', 'StoreRepeated']]) {
    for (const width of [1, 2, 4]) {
      const call = `\t{ x86_rep_${name}(ctx, base, ${width}); }`;
      const translated = `\t{ browser::Memory::From(base).${replacement}(ctx, ${width}); }`;
      assert.equal(adapt(call), call);
      assert.equal(adaptMemory(call), translated);
      const source = `${instruction}\n${instruction}\n${call}\n${instruction}\n${instruction}`;
      const adapted = adaptMemory(source);
      assert.equal(blocks(adapted), 2);
      assert.ok(adapted.includes(`}\n${translated}\n\t{ browser::Memory::BlockScope guestInstructionBlock`));
      assert.equal(adaptMemory(adapted), adapted);
      const exception = `X86_FUNC(__imp__sub_00011000) {\n\ttry {\n${call}\n\t} catch (const X86Resume&) { throw; }\n}`;
      assert.equal(adaptMemory(exception), exception);
    }
  }
});

test('pure context assignments and local arithmetic conditionals retain one bounded instruction block', () => {
  const source = `${instruction}\n\tctx.ebp = (uint32_t)(ctx.esp + 8u);\n\t{ uint32_t count = ctx.ecx & 31; if (count) { ctx.edx <<= count; } }\n${instruction}`;
  const adapted = adaptMemory(source);
  assert.equal(blocks(adapted), 1);
  assert.ok(instructions(adapted).includes(source));
  const unknown = `${instruction}\n\tctx.eax = unknown_helper(ctx);\n${instruction}`;
  assert.equal(blocks(adaptMemory(unknown)), 0);
});

test('SIMD value loads and stores share leases without admitting raw pointers or atomics', () => {
  const load = '\t{ uint32_t ea = ctx.ecx; ctx.xmm[0] = *X86_PTR(X86Xmm, ea); }';
  const store = '\t{ uint32_t ea = ctx.eax; *X86_PTR(X86Xmm, ea) = ctx.xmm[0]; }';
  const source = `${load}\n${store}\n${load}\n${store}`;
  const adapted = adaptMemory(source);
  assert.equal(blocks(adapted), 1);
  assert.ok(instructions(adapted).includes(source));
  assert.equal(adaptMemory(adapted), adapted);
  const lanes = '\t{ X86Xmm value = ctx.xmm[1]; for (int n = 0; n < 4; ++n) { ctx.xmm[0].f[n] += value.f[n]; } }';
  assert.equal(blocks(adaptMemory(`${load}\n${lanes}\n${store}`)), 1);
  for (const boundary of [
    '\t{ uint32_t ea = ctx.ecx; __atomic_exchange_n(X86_PTR(uint32_t, ea), 1, __ATOMIC_SEQ_CST); }',
    '\t{ uint32_t ea = ctx.ecx; std::memcpy(X86_PTR(uint8_t, ea), source, 4); }',
    '\t{ for (int n = 0; n < 4; ++n) { n = 0; ctx.eax += n; } }',
    '\t{ for (int n = 0; n < 4; ++n) { --n; ctx.eax += n; } }',
    '\t{ for (int n = 0; n < 4; ++n) { n >>= 1; ctx.eax += n; } }',
    '\t{ for (int n = 0; n < 4; ++n) { std::memcpy(&n, source, 4); } }',
  ]) {
    assert.equal(blocks(adaptMemory(`${source}\n${boundary}\n${source}`)), 2);
  }
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
    assert.ok(adapted.includes(`}\n${adaptMemory(boundary)}\n\t{ browser::Memory::BlockScope guestInstructionBlock`), boundary);
  }
});

test('instruction comments remain adjacent for arithmetic adaptation', () => {
  const source = `\t// 00011000 mov eax, [eax]\n${instruction}\n\t// 00011002 mov ecx, [eax]\n${instruction}`;
  const adapted = adaptMemory(source);
  assert.equal(blocks(adapted), 1);
  assert.ok(instructions(adapted).includes(`\t// 00011000 mov eax, [eax]\n${instruction}`));
  assert.ok(instructions(adapted).includes(`\t// 00011002 mov ecx, [eax]\n${instruction}`));
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

test('non-leaf blocks use scoped page access and release before guest calls', () => {
  const source = `X86_FUNC(__imp__sub_00011000) {\n${instruction}\n${instruction}\n\tsub_00012000(ctx, base);\n${instruction}\n${instruction}\n}`;
  const adapted = adaptMemory(source);
  assert.equal(blocks(adapted), 2);
  assert.equal([...adapted.matchAll(/BlockAccess<uint32_t>/g)].length, 4);
  assert.equal(adapted.includes('.BeginBlock();'), false);
  assert.equal(adapted.includes('catch (...)'), false);
  assert.equal(adaptMemory(adapted), adapted);
});
