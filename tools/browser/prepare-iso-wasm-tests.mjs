import { copyFile, mkdir } from 'node:fs/promises';
import { spawnSync } from 'node:child_process';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../../', import.meta.url));
const destination = resolve(process.argv[2] ?? resolve(root, 'out/iso-wasm-tests'));
await mkdir(destination, { recursive: true });
const build = spawnSync('em++', ['-std=c++20', '-O1', '-pthread', '-fwasm-exceptions',
  '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc',
  'tests/browser/iso-wasm.cpp', 'src/browser/iso-disc.cpp', 'src/browser/cpu_runtime.cpp', 'src/kernel/disc_index.cpp',
  'src/browser/image.cpp', 'src/browser/image-wasm.cpp',
  '--no-entry', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=web,node', '-sFILESYSTEM=0',
  '-sINITIAL_MEMORY=134217728', '-sALLOW_MEMORY_GROWTH=1', '-sMAXIMUM_MEMORY=268435456',
  '-sEXPORTED_RUNTIME_METHODS=HEAPU8', '-o', resolve(destination, 'iso-disc.mjs')],
{ cwd: root, encoding: 'utf8', timeout: 120000 });
if (build.status !== 0) {
  throw new Error(build.stderr);
}
const files = [
  'src/browser/iso-reader.mjs', 'src/browser/iso-mailbox.mjs', 'src/browser/iso-wasm.mjs',
  'src/browser/iso-io-service.mjs', 'src/browser/iso-io-worker.mjs', 'src/browser/iso-validation.mjs',
  'src/browser/iso-image.mjs',
  'tests/browser/iso-local.html', 'tests/browser/iso-local-browser.mjs', 'tests/browser/iso-local-worker.mjs',
  'tests/browser/iso-local-node.mjs',
  'tests/browser/iso-wasm-fixture.mjs', 'tests/browser/iso-wasm-guest.mjs', 'tests/browser/iso-wasm-node.mjs',
  'tests/browser/iso-wasm-worker.mjs', 'tests/browser/iso-wasm-browser.mjs', 'tests/browser/iso-wasm.html',
];
for (const file of files) {
  const path = resolve(destination, file);
  await mkdir(dirname(path), { recursive: true });
  await copyFile(resolve(root, file), path);
}
console.log(`Wasm ISO fixtures: ${destination}/tests/browser/iso-wasm.html`);
