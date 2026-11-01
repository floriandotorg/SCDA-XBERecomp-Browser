import { test } from 'node:test';
import assert from 'node:assert/strict';
import { BoundedIsoReader } from '../../src/browser/iso-reader.mjs';

const deferred = () => {
  let resolve;
  let reject;
  const promise = new Promise((success, failure) => { resolve = success; reject = failure; });
  return { promise, resolve, reject };
};

const controlledFile = (size = 6 * 1024 ** 3) => {
  const reads = [];
  return {
    size,
    reads,
    slice: (start, end) => ({ arrayBuffer: () => {
      const completion = deferred();
      reads.push({ start, end, ...completion });
      return completion.promise;
    } }),
  };
};
const readerFor = (file, options = {}) => new BoundedIsoReader(file, {
  maxReadBytes: 16, maxStagingBytes: 24, maxConcurrency: 2, maxPending: 4, ...options,
});
const flush = async () => { await new Promise((resolve) => setImmediate(resolve)); };
const code = (expected) => (error) => error.code === expected;

test('ISO reads preserve offsets above 4 GiB and reject invalid ranges before I/O', async () => {
  const file = controlledFile();
  const reader = readerFor(file);
  reader.beginGeneration(1);
  const pending = reader.readAt(2 ** 32 + 7, 12, 1, 1);
  assert.deepEqual([file.reads[0].start, file.reads[0].end], [2 ** 32 + 7, 2 ** 32 + 19]);
  file.reads[0].resolve(new Uint8Array(12).fill(37).buffer);
  assert.deepEqual(await pending, new Uint8Array(12).fill(37));
  for (const [offset, length] of [[-1, 1], [0.5, 1], [0, -1], [0, 17], [file.size, 1], [Number.MAX_SAFE_INTEGER, 2]]) {
    await assert.rejects(reader.readAt(offset, length, 2, 1), code('RANGE'));
  }
  assert.equal(file.reads.length, 1);
  assert.equal((await reader.readAt(file.size, 0, 3, 1)).length, 0);
});

test('ISO reader bounds concurrency, staging and queue while completing in reverse order', async () => {
  const file = controlledFile();
  const reader = readerFor(file);
  reader.beginGeneration(1);
  const first = reader.readAt(0, 12, 1, 1);
  const second = reader.readAt(20, 12, 2, 1);
  const third = reader.readAt(40, 16, 3, 1);
  const fourth = reader.readAt(60, 8, 4, 1);
  await assert.rejects(reader.readAt(80, 1, 5, 1), code('BUSY'));
  await assert.rejects(reader.readAt(80, 1, 1, 1), code('DUPLICATE'));
  assert.equal(file.reads.length, 2);
  assert.equal(reader.stats.stagingBytes, 24);
  file.reads[1].resolve(new Uint8Array(12).fill(2).buffer);
  assert.equal((await second)[0], 2);
  assert.equal(file.reads.length, 2);
  file.reads[0].resolve(new Uint8Array(12).fill(1).buffer);
  assert.equal((await first)[0], 1);
  await flush();
  assert.equal(file.reads.length, 4);
  file.reads[3].resolve(new Uint8Array(8).fill(4).buffer);
  file.reads[2].resolve(new Uint8Array(16).fill(3).buffer);
  assert.equal((await third)[0], 3);
  assert.equal((await fourth)[0], 4);
  assert.equal(reader.stats.peakStagingBytes, 24);
  assert.equal(reader.stats.peakConcurrency, 2);
  assert.equal(reader.stats.cacheBytes, 0);
});

test('generation changes cancel active and queued reads but retain reservations until I/O settles', async () => {
  const file = controlledFile();
  const reader = readerFor(file, { maxConcurrency: 1 });
  reader.beginGeneration(1);
  const first = reader.readAt(0, 16, 1, 1);
  const queued = reader.readAt(20, 8, 2, 1);
  const firstRejected = assert.rejects(first, code('CANCELLED'));
  const queuedRejected = assert.rejects(queued, code('CANCELLED'));
  reader.beginGeneration(2);
  await Promise.all([firstRejected, queuedRejected]);
  await assert.rejects(reader.readAt(0, 1, 3, 1), code('STALE'));
  const replacement = reader.readAt(100, 16, 1, 2);
  assert.equal(file.reads.length, 1);
  assert.equal(reader.stats.stagingBytes, 16);
  file.reads[0].resolve(new ArrayBuffer(16));
  await flush();
  assert.equal(file.reads.length, 2);
  file.reads[1].resolve(new Uint8Array(16).fill(9).buffer);
  assert.equal((await replacement)[0], 9);
  assert.equal(reader.stats.bytesRead, 32);
  assert.throws(() => reader.beginGeneration(2), code('GENERATION'));
});

test('short reads, denied access and cancellation free slots without fake success', async () => {
  const file = controlledFile(100);
  const reader = readerFor(file, { maxConcurrency: 1 });
  reader.beginGeneration(1);
  const short = reader.readAt(0, 12, 1, 1);
  const shortRejected = assert.rejects(short, code('SHORT_READ'));
  file.reads[0].resolve(new ArrayBuffer(3));
  await shortRejected;
  const failed = reader.readAt(0, 12, 2, 1);
  const failedRejected = assert.rejects(failed, code('READ_FAILED'));
  file.reads[1].reject(new Error('private path must not escape'));
  await failedRejected;
  const cancelled = reader.readAt(0, 12, 3, 1);
  const cancelledRejected = assert.rejects(cancelled, code('CANCELLED'));
  reader.cancel(3, 1);
  await cancelledRejected;
  file.reads[2].resolve(new ArrayBuffer(12));
  await flush();
  assert.equal(reader.stats.stagingBytes, 0);
  assert.equal(reader.stats.pending, 0);
  reader.close();
  await assert.rejects(reader.readAt(0, 1, 4, 1), code('CLOSED'));
});

test('budget configuration cannot create an unserviceable queue', () => {
  const file = controlledFile();
  for (const options of [{ maxConcurrency: 0 }, { maxReadBytes: 25 }, { maxPending: 1 }, { maxStagingBytes: Infinity }]) {
    assert.throws(() => readerFor(file, options), code('LIMITS'));
  }
});
