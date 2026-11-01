import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { inventedImage } from './image-fixture.mjs';
import { resolve } from 'node:path';

const directory = resolve('out/render-snapshot-tests');
mkdirSync(directory, { recursive: true });
writeFileSync(`${directory}/run.mjs`, "import create from './snapshot.mjs'; await create();\n");
test('default table extractor uses original file ranges and trusted hash', async () => {
  const { extractRenderState } = await import('../../tools/browser/extract-render-state.mjs');
  const image = inventedImage();
  const imagePath = `${directory}/invented.xbe`;
  const outputPath = `${directory}/invented-tables.json`;
  writeFileSync(imagePath, image);
  const expectedSha256 = createHash('sha256').update(image).digest('hex');
  const tables = [{ name: 'inventedWords', address: 0x12080, count: 1, width: 4 }];
  const record = extractRenderState({ imagePath, outputPath, expectedSha256, tables });
  assert.equal(record.tables.inventedWords.values[0], 0x44332211);
  assert.equal(JSON.parse(readFileSync(outputPath, 'utf8')).sourceSha256, expectedSha256);
  assert.throws(() => extractRenderState({ imagePath, outputPath, expectedSha256: '0'.repeat(64), tables }), /hash/);
  assert.throws(() => extractRenderState({ imagePath, outputPath, expectedSha256, tables: [{ name: 'zeroFill', address: 0x12100, count: 1, width: 4 }] }), /file-backed/);
  assert.throws(() => extractRenderState({ imagePath, outputPath: 'tests/browser/proprietary.json', expectedSha256, tables }), /out/);
});
let nativeWire;
for (const platform of ['native', 'wasm']) {
  test(`${platform} bounded owned portable render snapshot`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'snapshot.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=33554432', '-sALLOW_MEMORY_GROWTH=1', '-sSTACK_SIZE=2097152']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/render-snapshot.cpp', 'src/browser/render-snapshot.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], { encoding: 'utf8', timeout: 30000 });
    assert.equal(run.status, 0, `${run.stdout}\n${run.stderr}`);
    assert.match(run.stdout, /render-snapshot-contracts-ok/);
    const wire = run.stdout.match(/wire-\d+-[0-9a-f]+/)?.[0];
    assert.ok(wire);
    if (wasm) { assert.equal(wire, nativeWire, 'native and Wasm produce identical wire bytes'); }
    if (!wasm) { nativeWire = wire; }
  });
}
