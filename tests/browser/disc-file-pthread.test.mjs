import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { Worker, MessageChannel } from 'node:worker_threads';
import { createIsoMailbox, ISO_MAILBOX } from '../../src/browser/iso-mailbox.mjs';
import { ISO_LIMITS } from '../../src/browser/iso-reader.mjs';

const directory = resolve('out/disc-file-pthread-tests');
mkdirSync(directory, { recursive: true });
const moduleUrl = pathToFileURL(`${directory}/disc-file-pthread.mjs`).href;
const build = spawnSync('em++', ['-std=c++20', '-O1', '-g', '-pthread', '-fwasm-exceptions',
  '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', 'tests/browser/disc-file-pthread.cpp',
  'src/browser/disc-wasm.cpp', 'src/browser/file-provider.cpp', 'src/browser/image.cpp',
  'src/browser/raw-cache.cpp', 'src/browser/cache-fatx.cpp', 'src/browser/temporary-filesystem.cpp', 'src/browser/cpu_runtime.cpp',
  'src/kernel/disc_index.cpp', '--no-entry', '-sMODULARIZE=1', '-sEXPORT_ES6=1',
  '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=134217728',
  '-sALLOW_MEMORY_GROWTH=1', '-sMAXIMUM_MEMORY=268435456',
  '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2',
  '-sEXPORTED_RUNTIME_METHODS=HEAPU8', '-o', `${directory}/disc-file-pthread.mjs`],
{ encoding: 'utf8', timeout: 120000 });
assert.equal(build.status, 0, build.stderr);

const execute = async (context, scenario) => {
  const channel = new MessageChannel();
  const mailbox = createIsoMailbox();
  const ownership = new SharedArrayBuffer(4);
  new Int32Array(ownership)[0] = 1;
  const audit = new SharedArrayBuffer(16);
  const done = new SharedArrayBuffer(4);
  const workerUrl = new URL('./disc-file-pthread-node.mjs', import.meta.url);
  const io = new Worker(workerUrl, { workerData: {
    role: 'io', scenario, port: channel.port1, mailbox, ownership, audit,
  }, transferList: [channel.port1] });
  const loader = new Worker(workerUrl, { workerData: {
    role: 'loader', scenario, port: channel.port2, mailbox, ownership, audit, done, moduleUrl,
  }, transferList: [channel.port2] });
  context.after(async () => { await Promise.all([io.terminate(), loader.terminate()]); });
  const result = new Promise((resolve, reject) => {
    loader.once('message', resolve);
    loader.once('error', reject);
    io.once('error', reject);
  });
  const state = new Int32Array(done);
  const deadline = performance.now() + 15000;
  while (Atomics.load(state, 0) === 0 && performance.now() < deadline) {
    Atomics.wait(state, 0, 0, Math.max(1, deadline - performance.now()));
  }
  assert.equal(Atomics.load(state, 0), 1, 'Actual Runtime::Join deadlocked while UI callbacks were blocked');
  const response = await result;
  assert.equal(response.ok, true, JSON.stringify(response));
  const payload = new Uint8Array(mailbox, ISO_MAILBOX.headerBytes);
  if (scenario === 'timeout' || scenario === 'cancel') { payload.fill(0xa5); }
  const settled = new Promise((resolve, reject) => {
    io.once('message', resolve);
    io.once('error', reject);
  });
  io.postMessage({ type: 'settle' });
  const { stats } = await settled;
  if (scenario === 'timeout' || scenario === 'cancel') {
    assert.ok(payload.every((byte) => byte === 0xa5), 'Retired producer wrote the mailbox after guest completion');
  }
  assert.ok(response.calls.filter(({ joining }) => joining).every(({ nativeJoining }) => nativeJoining),
    'A pthread ISO proxy ran before the C++ Runtime::Join phase');
  assert.equal(stats.fileSize, 6 * 1024 ** 3);
  assert.equal(stats.active, 0);
  assert.equal(stats.pending, 0);
  assert.equal(stats.stagingBytes, 0);
  assert.equal(stats.cacheBytes, 0);
  assert.ok(stats.peakStagingBytes <= ISO_LIMITS.maxStagingBytes);
  assert.ok(stats.peakConcurrency <= ISO_LIMITS.maxConcurrency);
  assert.ok(stats.bytesRead < 32768);
  for (const round of response.rounds) {
    assert.equal(round.joined, 1, JSON.stringify(response));
    assert.equal(round.owned, 2);
    assert.equal(round.unbound, 2, 'The fixture must not install reader functions on pthread Modules');
    assert.equal(round.distinct, 1);
  }
  assert.equal(response.loaderEvents, 0, 'Loader event callbacks ran during the synchronous guest operation');
  return { ...response, highReads: Atomics.load(new Int32Array(audit), 0), stats };
};

test('owned guest file handles read executable and non-XBE disc bytes above 4 GiB through independent I/O while loader joins', async (context) => {
  const result = await execute(context, 'success');
  for (const round of result.rounds) {
    assert.deepEqual(round.workers.map(({ completed }) => completed), [7, 6], JSON.stringify(result));
    assert.ok(round.workers.every(({ matched, status }) => matched === 1 && status === 0));
    assert.deepEqual(round.counters, { assetBytes: 6, assetOperations: 1, executableBytes: 7, executableOperations: 1 });
  }
  assert.equal(result.highReads, 6);
  assert.equal(result.duringJoin, 6);
  assert.equal(result.calls.filter(({ joining }) => joining).length, 6);
  assert.ok(result.calls.filter(({ joining }) => joining).every(({ offset }) => offset > 2 ** 32));
});

for (const scenario of ['short', 'denied', 'timeout', 'stale-before', 'stale-after', 'cancel']) {
  test(`actual guest pthread ${scenario} failures preserve both guest destinations`, async (context) => {
    const result = await execute(context, scenario);
    assert.ok(result.rounds[0].workers.every(({ completed, unchanged }) => completed === 0 && unchanged === 1),
      JSON.stringify(result));
    assert.equal(result.error, {
      short: 'SHORT_READ', denied: 'READ_FAILED', timeout: 'TIMEOUT',
      'stale-before': 'STALE', 'stale-after': 'STALE', cancel: 'STALE',
    }[scenario], JSON.stringify(result));
    assert.equal(result.duringJoin, 2);
    assert.deepEqual(result.rounds[0].counters, { assetBytes: 0, assetOperations: 0, executableBytes: 0, executableOperations: 0 });
    assert.ok(result.rounds[0].workers.every(({ status }) => status === 0xc0000185));
    if (scenario === 'cancel') {
      assert.deepEqual(result.calls.filter(({ joining }) => joining).map(({ error }) => error), ['CANCELLED', 'STALE']);
    }
    assert.equal(result.highReads, scenario === 'stale-before' ? 0 : scenario === 'stale-after' || scenario === 'cancel' ? 1 : 2);
  });
}

test('actual guest bounds reject invalid destinations, budgets and source extents before I/O', async (context) => {
  const result = await execute(context, 'bounds');
  for (const round of result.rounds) {
    assert.ok(round.workers.every(({ completed, unchanged }) => completed === 0 && unchanged === 1));
  }
  assert.deepEqual(result.rounds.map(({ rejected }) => rejected), [2, 2, 0]);
  assert.equal(result.duringJoin, 0);
  assert.equal(result.highReads, 0);
  for (const round of result.rounds) {
    assert.deepEqual(round.counters, { assetBytes: 0, assetOperations: 0, executableBytes: 0, executableOperations: 0 });
  }
  assert.ok(result.rounds[2].workers.every(({ status }) => status === 0xc000000d));
});

test('Stop during independent I/O cancels guest copy and joins both owned workers', async (context) => {
  const result = await execute(context, 'stop');
  assert.equal(result.rounds[0].workers[0].completed, 0, JSON.stringify(result));
  assert.equal(result.rounds[0].workers[0].unchanged, 1);
  assert.equal(result.rounds[0].rejected, 1);
  assert.equal(result.rounds[0].stoppedDuringIo, 1);
  assert.equal(result.highReads, 1);
  assert.equal(result.duringJoin, 1);
  assert.deepEqual(result.rounds[0].counters, { assetBytes: 0, assetOperations: 0, executableBytes: 0, executableOperations: 0 });
});
