import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
const load = async () => existsSync('src/browser/mission-metrics.mjs') ? import('../../src/browser/mission-metrics.mjs') : {};
test('frame distribution is bounded and resource peaks survive replacement', async () => {
  const { createMissionMetrics } = await load();
  assert.equal(typeof createMissionMetrics, 'function');
  const metrics = createMissionMetrics();
  for (const [time, resources] of [[100, 3], [116, 4], [150, 2], [220, 3]]) { metrics.present(time, { resources }); }
  const result = metrics.snapshot();
  assert.equal(result.frames, 4);
  assert.equal(result.intervals, 3);
  assert.equal(result.meanMilliseconds, 40);
  assert.equal(result.p50Milliseconds, 34);
  assert.equal(result.p95Milliseconds, 70);
  assert.equal(result.peakResources, 4);
  metrics.begin();
  metrics.present(300, { resources: 1 });
  assert.equal(metrics.snapshot().frames, 1);
  assert.equal(metrics.snapshot().peakResources, 1);
});
test('inactive windows and invalid samples do not become frame intervals', async () => {
  const { createMissionMetrics } = await load();
  const metrics = createMissionMetrics();
  metrics.present(0, { resources: 1 });
  metrics.present(40, { resources: 1 });
  metrics.suspend();
  metrics.present(600040, { resources: 1 });
  metrics.present(600080, { resources: 1 });
  assert.equal(metrics.snapshot().p95Milliseconds, 40);
  assert.equal(metrics.snapshot().intervals, 2);
  assert.throws(() => metrics.present(NaN, { resources: 1 }), /time/i);
});
test('shared metrics reject torn or unbounded snapshots', async () => {
  const { readMissionMetrics } = await load();
  const words = new Uint32Array(new SharedArrayBuffer(168));
  assert.equal(readMissionMetrics(words), undefined);
  words[0] = 1;
  assert.equal(readMissionMetrics(words), undefined);
  words[0] = 2;
  words[1] = 384 * 1024 * 1024;
  words[3] = 128;
  assert.equal(readMissionMetrics(words).activeWorkers, 128);
  words[3] = 129;
  assert.throws(() => readMissionMetrics(words), /worker|bound/i);
});
test('measurement retains peak heaps while latest counters advance', async () => {
  const { createMissionMetrics } = await load();
  const metrics = createMissionMetrics();
  metrics.present(0, { serviceHeapBytes: 16, metrics: { wasmHeapBytes: 32, activeWorkers: 2 } });
  metrics.present(40, { serviceHeapBytes: 8, metrics: { wasmHeapBytes: 24, activeWorkers: 1 } });
  assert.equal(metrics.snapshot().peaks.wasmHeapBytes, 32);
  assert.equal(metrics.snapshot().peaks.serviceHeapBytes, 16);
  assert.equal(metrics.snapshot().runtime.activeWorkers, 1);
});

test('simulation samples retain independent game time and reject invalid floats', async () => {
  const { readMissionMetrics } = await load();
  const words = new Uint32Array(new SharedArrayBuffer(168));
  const floats = new Float32Array(words.buffer);
  words[0] = 2;
  words[31] = 30;
  floats[32] = 12.5;
  floats[33] = 1 / 30;
  words[34] = 1;
  const snapshot = readMissionMetrics(words);
  assert.deepEqual(snapshot.simulation, { samples: 30, seconds: 12.5, deltaSeconds: Math.fround(1 / 30), epoch: 1 });
  floats[32] = NaN;
  assert.throws(() => readMissionMetrics(words), /simulation/i);
});

test('mission renderer residency accepts its bounded scene budget and rejects overflow', async () => {
  const { readMissionMetrics } = await load();
  const words = new Uint32Array(new SharedArrayBuffer(168));
  words[0] = 2;
  words[4] = 128 * 1024 * 1024;
  assert.equal(readMissionMetrics(words).rendererResidentBytes, words[4]);
  ++words[4];
  assert.throws(() => readMissionMetrics(words), /bound/i);
});

test('audio occupancy and rejected creations remain observable and bounded', async () => {
  const { readMissionMetrics, createMissionMetrics } = await load();
  const words = new Uint32Array(new SharedArrayBuffer(168));
  words[0] = 2;
  words[35] = 256;
  words[36] = 7;
  const sample = readMissionMetrics(words);
  assert.equal(sample.audioVoices, 256);
  assert.equal(sample.audioVoiceRejections, 7);
  const metrics = createMissionMetrics();
  metrics.present(0, { metrics: sample });
  assert.equal(metrics.snapshot().peaks.audioVoices, 256);
  ++words[35];
  assert.throws(() => readMissionMetrics(words), /audio|bound/i);
});

test('simulation update, render work and bounded backlog have independent telemetry', async () => {
  const { readMissionMetrics, createMissionMetrics } = await load();
  const words = new Uint32Array(new SharedArrayBuffer(168));
  words[0] = 2;
  words[37] = 10;
  words[38] = 7;
  words[39] = 1500;
  words[40] = 4000;
  words[41] = 20000;
  const sample = readMissionMetrics(words);
  assert.equal(sample.simulationSteps, 10);
  assert.equal(sample.skippedViewportDraws, 7);
  assert.equal(sample.updateMicroseconds, 1500);
  assert.equal(sample.renderMicroseconds, 4000);
  const metrics = createMissionMetrics();
  metrics.present(0, { metrics: sample });
  metrics.present(40, { metrics: { ...sample, simulationBacklogMicroseconds: 0 } });
  assert.equal(metrics.snapshot().peaks.simulationBacklogMicroseconds, 20000);
  words[41] = 2000001;
  assert.throws(() => readMissionMetrics(words), /backlog|bound/i);
});

test('story cache heap has a checked one GiB telemetry ceiling', async () => {
  const { readMissionMetrics } = await load();
  const words = new Uint32Array(new SharedArrayBuffer(168));
  words[0] = 2;
  words[1] = 1024 * 1024 * 1024;
  assert.equal(readMissionMetrics(words).wasmHeapBytes, words[1]);
  ++words[1];
  assert.throws(() => readMissionMetrics(words), /memory|bound/i);
});
