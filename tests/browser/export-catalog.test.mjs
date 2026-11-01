import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { exportCatalog } from '../../tools/browser/export-catalog.mjs';

test('catalog records declarations, not callable browser implementations', () => {
  assert.deepEqual(exportCatalog([{ path: 'src/kernel/example.cpp', text: `
KERNEL_EXPORT(277, RtlEnterCriticalSection, 1) {
KERNEL_FASTCALL(160, KfRaiseIrql) {
KERNEL_DATA(156, KeTickCount, 4);
` }]), [
    { ordinal: 156, name: 'KeTickCount', kind: 'data', size: 4, source: 'src/kernel/example.cpp' },
    { ordinal: 160, name: 'KfRaiseIrql', kind: 'function', abi: 'fastcall', source: 'src/kernel/example.cpp' },
    { ordinal: 277, name: 'RtlEnterCriticalSection', kind: 'function', abi: 'stdcall', arguments: 1, source: 'src/kernel/example.cpp' },
  ]);
  for (const text of [
    'KERNEL_EXPORT(277, First, 1) {\nKERNEL_DATA(277, Second, 4);',
    'KERNEL_EXPORT(400, Invalid, 1) {',
    'KERNEL_DATA(1, Invalid, unknown);',
  ]) { assert.throws(() => exportCatalog([{ path: 'example.cpp', text }]), /export catalog/); }
});

test('current desktop declarations identify the first real import without authorizing it', () => {
  const files = readdirSync('src/kernel').filter((name) => name.endsWith('.cpp')).map((name) => ({
    path: `src/kernel/${name}`, text: readFileSync(`src/kernel/${name}`, 'utf8'),
  }));
  const catalog = exportCatalog(files);
  assert.equal(catalog.find(({ ordinal }) => ordinal === 277)?.name, 'RtlEnterCriticalSection');
  assert.equal(catalog.find(({ ordinal }) => ordinal === 156)?.kind, 'data');
  assert.deepEqual(catalog.find(({ ordinal }) => ordinal === 8), {
    ordinal: 8, name: 'DbgPrint', kind: 'function', abi: 'custom', entry: 'DbgPrintEntry', source: 'src/kernel/xboxkrnl.cpp',
  });
  assert.ok(catalog.every((entry) => !('target' in entry)));
});
