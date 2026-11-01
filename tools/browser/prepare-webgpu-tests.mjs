import { copyFile, mkdir } from 'node:fs/promises';
import { spawnSync } from 'node:child_process';
import { dirname, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '../..');
const output = resolve(root, 'out/browser-webgpu-tests');
await mkdir(output, { recursive: true });
const temporary = resolve(root, 'out/test-tmp');
await mkdir(temporary, { recursive: true });
const env = { ...process.env, TMPDIR: temporary };
const compiler = '/opt/homebrew/opt/emscripten/bin/em++';
const args = [
  '-std=c++20', '-O1', '-fexceptions', '-DWEBGPU_SERVICE_TESTING=1',
  '--use-port=emdawnwebgpu',
  resolve(root, 'src/browser/webgpu-service.cpp'),
  '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=web,worker', '-sALLOW_MEMORY_GROWTH=1',
  '-sEXPORTED_FUNCTIONS=["_webgpu_start","_webgpu_submit","_webgpu_resources","_webgpu_shutdown","_webgpu_test_loss","_malloc","_free"]',
  '-sEXPORTED_RUNTIME_METHODS=["HEAPU8"]', '-sASSERTIONS=1', '-o', resolve(output, 'webgpu-service.mjs')
];
const result = spawnSync(compiler, args, { stdio: 'inherit', cwd: root, env });
if (result.error) { throw result.error; }
if (result.status !== 0) { process.exit(result.status ?? 1); }
const guest = spawnSync(compiler, [
  '-std=c++20', '-O1', '-pthread', '-sPTHREAD_POOL_SIZE=1', '-sINITIAL_MEMORY=33554432', '-I', resolve(root, 'src'),
  resolve(root, 'tests/browser/webgpu-service-guest.cpp'), resolve(root, 'src/browser/webgpu-transport.cpp'),
  '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=web,worker', '-sALLOW_MEMORY_GROWTH=1',
  '-sEXPORTED_FUNCTIONS=["_gpu_execute","_gpu_reply","_gpu_reply_size","_gpu_command","_gpu_pthread_unbound","_malloc","_free"]', '-sEXPORTED_RUNTIME_METHODS=["HEAPU8"]',
  '-o', resolve(output, 'webgpu-service-guest.mjs')
], { stdio: 'inherit', cwd: root, env });
if (guest.error) { throw guest.error; }
if (guest.status !== 0) { process.exit(guest.status ?? 1); }
const publicRoot = resolve(output, 'public');
const fixtures = ['src/browser/webgpu-service.mjs', 'src/browser/webgpu-transport.mjs',
  'tests/browser/webgpu-service.html', 'tests/browser/webgpu-service-guest.mjs',
  'tests/browser/webgpu-service-lifecycle.html', 'tests/browser/webgpu-service-loss.mjs', 'tests/browser/webgpu-service-loss-module.mjs',
  'out/browser-webgpu-tests/webgpu-service.mjs', 'out/browser-webgpu-tests/webgpu-service.wasm',
  'out/browser-webgpu-tests/webgpu-service-guest.mjs', 'out/browser-webgpu-tests/webgpu-service-guest.wasm'];
for (const path of fixtures) {
  const destination = resolve(publicRoot, path);
  await mkdir(dirname(destination), { recursive: true });
  await copyFile(resolve(root, path), destination);
}
console.log(publicRoot);
