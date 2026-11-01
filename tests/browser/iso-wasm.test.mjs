import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { Worker, MessageChannel } from 'node:worker_threads';
import { createIsoMailbox, finishIsoRead } from '../../src/browser/iso-mailbox.mjs';
import { createWasmIsoReader } from '../../src/browser/iso-wasm.mjs';
import { installIsoIoService } from '../../src/browser/iso-io-service.mjs';
import { syntheticDisc } from './iso-wasm-fixture.mjs';
import { inventedImage } from './image-fixture.mjs';
import { createHash } from 'node:crypto';

const ownership = () => new Int32Array(new SharedArrayBuffer(4));
const adapter = (complete) => {
  const owner = ownership();
  owner[0] = 1;
  let heap = new Uint8Array(128).fill(0xaa);
  let requests = 0;
  const mailbox = createIsoMailbox(32);
  const read = createWasmIsoReader({
    port: { postMessage: (message) => {
      ++requests;
      complete?.({ message, owner, replaceHeap: () => { heap = new Uint8Array(256).fill(0xbb); } });
      finishIsoRead(mailbox, message.requestId, message.generation, new Uint8Array(message.length).fill(7));
    } }, mailbox, generation: 1, ownership: owner, getHeap: () => heap,
  });
  return { read, owner, heap: () => heap, requests: () => requests };
};

test('Wasm ISO adapter checks destinations before I/O and copies only the requested bytes', () => {
  const source = adapter(({ message }) => assert.equal(message.offset, 2 ** 32 + 13));
  for (const destination of [-1, 0.5, 127, 2 ** 32]) {
    assert.throws(() => source.read(0, destination, 4), (error) => error.code === 'DESTINATION');
  }
  assert.equal(source.requests(), 0);
  assert.equal(source.read(2 ** 32 + 13, 16, 4), 4);
  assert.deepEqual([...source.heap().slice(15, 21)], [0xaa, 7, 7, 7, 7, 0xaa]);
});

test('Wasm ISO adapter reacquires the heap after waiting and rejects replaced runtime ownership', () => {
  const grown = adapter(({ replaceHeap }) => replaceHeap());
  assert.equal(grown.read(0, 16, 4), 4);
  assert.deepEqual([...grown.heap().slice(15, 21)], [0xbb, 7, 7, 7, 7, 0xbb]);
  const stale = adapter(({ owner }) => Atomics.store(owner, 0, 2));
  assert.throws(() => stale.read(0, 16, 4), (error) => error.code === 'STALE');
  assert.ok(stale.heap().every((byte) => byte === 0xaa));
  assert.throws(() => stale.read(0, 16, 4), (error) => error.code === 'STALE');
  assert.equal(stale.requests(), 1);
});

test('Wasm ISO adapter leaves destination unchanged on short reads and completion errors', () => {
  for (const outcome of ['SHORT_READ', 'READ_FAILED', 'CANCELLED']) {
    const mailbox = createIsoMailbox(32);
    const owner = ownership();
    owner[0] = 1;
    const heap = new Uint8Array(32).fill(0xaa);
    const read = createWasmIsoReader({
      mailbox, generation: 1, ownership: owner, getHeap: () => heap,
      port: { postMessage: ({ requestId, generation }) => {
        finishIsoRead(mailbox, requestId, generation,
          outcome === 'SHORT_READ' ? new Uint8Array(1) : undefined, outcome);
      } },
    });
    assert.throws(() => read(0, 8, 4), (error) => error.code === outcome);
    assert.ok(heap.every((byte) => byte === 0xaa));
  }
});

mkdirSync('out/iso-wasm-tests', { recursive: true });
test('native guest disc copies release leases and revalidate memory and runtime after I/O', () => {
  const output = 'out/iso-wasm-tests/native';
  const build = spawnSync('clang++', ['-std=c++20', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
    '-fno-omit-frame-pointer', '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc',
    'tests/browser/iso-disc.cpp', 'src/browser/iso-disc.cpp', 'src/browser/cpu_runtime.cpp',
    'src/kernel/disc_index.cpp', '-o', output], { encoding: 'utf8', timeout: 120000 });
  assert.equal(build.status, 0, build.stderr);
  const execution = spawnSync(output, [], { encoding: 'utf8', timeout: 30000 });
  assert.equal(execution.status, 0, execution.stderr);
  assert.match(execution.stdout, /iso-guest-contracts-ok/);
});

test('Wasm disc parser mounts through independent I/O and copies bounded subfiles into compact guest memory', async (context) => {
  const build = spawnSync(process.execPath, ['tools/browser/prepare-iso-wasm-tests.mjs'], { encoding: 'utf8', timeout: 120000 });
  assert.equal(build.status, 0, build.stderr);
  const control = new EventTarget();
  let reply;
  control.postMessage = (message) => { reply = message; };
  installIsoIoService(control);
  const request = (data) => {
    control.dispatchEvent(new MessageEvent('message', { data }));
    assert.equal(reply.ok, true, JSON.stringify(reply));
    return reply;
  };
  const fixture = syntheticDisc({ large: true });
  let highReads = 0;
  request({ type: 'mount', file: { size: fixture.size, slice: (start, end) => {
    if (start > 2 ** 32) { ++highReads; }
    return fixture.slice(start, end);
  } }, generation: 1 });
  context.after(() => request({ type: 'close' }));
  let heartbeats = 0;
  const timer = setInterval(() => { ++heartbeats; }, 1);
  context.after(() => clearInterval(timer));
  for (let n = 1; n <= 3; ++n) {
    if (n > 1) {
      request({ type: 'generation', generation: n });
    }
    const channel = new MessageChannel();
    const mailbox = createIsoMailbox();
    const owner = ownership();
    owner[0] = n;
    request({ type: 'connect', port: channel.port1, mailbox, generation: n });
    const guest = new Worker(new URL('../../out/iso-wasm-tests/tests/browser/iso-wasm-node.mjs', import.meta.url));
    context.after(() => guest.terminate());
    const result = await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('Wasm guest timed out')), 15000);
      guest.once('error', reject);
      guest.once('message', (message) => { clearTimeout(timeout); resolve(message); });
      guest.postMessage({ port: channel.port2, mailbox, generation: n, ownership: owner, size: fixture.size }, [channel.port2]);
    });
    assert.equal(result.ok, true, JSON.stringify(result));
    assert.equal(result.bytes, 7);
    assert.equal(result.validated, true);
    assert.equal(result.rejectedMismatch, true);
    assert.equal(result.partition, 0);
    assert.equal(result.invalidDestination, true);
    assert.equal(result.stale, true);
    await guest.terminate();
    const stats = request({ type: 'stats' }).stats;
    assert.equal(stats.pending, 0);
    assert.equal(stats.stagingBytes, 0);
    assert.ok(stats.bytesRead < 32768);
    assert.equal(stats.cacheBytes, 0);
  }
  assert.ok(heartbeats > 0);
  assert.equal(highReads, 12);
});

test('authenticated mounted XBE bytes reach the Wasm parser and private image backing', async (context) => {
  const build = spawnSync(process.execPath, ['tools/browser/prepare-iso-wasm-tests.mjs'], { encoding: 'utf8', timeout: 120000 });
  assert.equal(build.status, 0, build.stderr);
  const imageBytes = inventedImage();
  const file = syntheticDisc({ imageBytes });
  const control = new EventTarget();
  let reply;
  control.postMessage = (message) => { reply = message; };
  installIsoIoService(control);
  const request = (data) => {
    control.dispatchEvent(new MessageEvent('message', { data }));
    assert.equal(reply.ok, true, JSON.stringify(reply));
    return reply;
  };
  request({ type: 'mount', file, generation: 1 });
  context.after(() => request({ type: 'close' }));
  const manifest = {
    schema: 1, image: 'default.xbe', xbeSize: imageBytes.length,
    xbeSha256: createHash('sha256').update(imageBytes).digest('hex'),
    requiredStoryFiles: [{ path: 'story.dat', minBytes: 3 }],
    ranges: { X86ImageBase: 0x10000, X86ImageEnd: 0x13000, X86EntryPoint: 0x11000 },
  };
  for (let n = 1; n <= 3; ++n) {
    if (n > 1) { request({ type: 'generation', generation: n }); }
    const channel = new MessageChannel();
    const mailbox = createIsoMailbox();
    const owner = ownership();
    owner[0] = n;
    request({ type: 'connect', port: channel.port1, mailbox, generation: n });
    const worker = new Worker(new URL('../../out/iso-wasm-tests/tests/browser/iso-local-node.mjs', import.meta.url));
    context.after(() => worker.terminate());
    const result = await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('Original image preparation timed out')), 15000);
      worker.once('error', (error) => { clearTimeout(timeout); reject(error); });
      worker.once('message', (message) => { clearTimeout(timeout); resolve(message); });
      const expected = n === 2 ? { ...manifest, xbeSha256: '0'.repeat(64) } : manifest;
      worker.postMessage({ port: channel.port2, mailbox, generation: n, ownership: owner, size: file.size, manifest: expected }, [channel.port2]);
    });
    assert.equal(result.ok, n !== 2, JSON.stringify(result));
    if (n !== 2) {
      assert.equal(result.sections, 2);
      assert.equal(result.imports, 1);
      assert.equal(result.tlsBytes, 20);
      assert.ok(result.backingBytes > 64 * 1024 ** 2);
    }
    await worker.terminate();
    const { stats } = request({ type: 'stats' });
    assert.equal(stats.pending, 0);
    assert.equal(stats.stagingBytes, 0);
    assert.ok(stats.bytesRead < 32768);
  }
});
