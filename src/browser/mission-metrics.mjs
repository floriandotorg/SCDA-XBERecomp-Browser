export const MISSION_METRICS_WORDS = 42;
export const MISSION_RENDERER_BYTES = 128 * 1024 * 1024;
export const createMissionMetrics = () => {
  const histogram = new Uint32Array(60001);
  let frames, intervals, total, maximum, previous, peakResources, latest, peaks;
  const begin = () => {
    histogram.fill(0);
    frames = intervals = total = maximum = peakResources = 0;
    previous = latest = undefined;
    peaks = {};
  };
  begin();
  const percentile = fraction => {
    if (!intervals) { return null; }
    const rank = Math.ceil(intervals * fraction);
    let count = 0;
    for (let n = 0; n < histogram.length; ++n) {
      count += histogram[n];
      if (count >= rank) { return n; }
    }
    return null;
  };
  return Object.freeze({
    begin,
    suspend: () => { previous = undefined; },
    present: (time, details = {}) => {
      if (!Number.isFinite(time) || time < 0 || (previous !== undefined && time < previous)) { throw new RangeError('Invalid presentation time.'); }
      ++frames;
      if (previous !== undefined) {
        const duration = time - previous;
        ++intervals;
        total += duration;
        maximum = Math.max(maximum, duration);
        ++histogram[Math.min(60000, Math.ceil(duration))];
      }
      previous = time;
      if (Number.isInteger(details.resources) && details.resources >= 0) { peakResources = Math.max(peakResources, details.resources); }
      if (Number.isFinite(details.serviceHeapBytes)) { peaks.serviceHeapBytes = Math.max(peaks.serviceHeapBytes ?? 0, details.serviceHeapBytes); }
      if (details.metrics) {
        latest = details.metrics;
        for (const key of ['wasmHeapBytes', 'translatedBackingBytes', 'activeWorkers', 'rendererResidentBytes', 'rendererResources', 'pipelines', 'childThreadHandles', 'objectReferences', 'audioVoices', 'simulationBacklogMicroseconds']) {
          if (Number.isFinite(latest[key])) { peaks[key] = Math.max(peaks[key] ?? 0, latest[key]); }
        }
      }
    },
    snapshot: () => ({ frames, intervals, meanMilliseconds: intervals ? total / intervals : null,
      p50Milliseconds: percentile(0.5), p95Milliseconds: percentile(0.95), p99Milliseconds: percentile(0.99),
      maxMilliseconds: intervals ? maximum : null, histogramBucketMilliseconds: 1, histogramOverflowMilliseconds: 60000,
      peakResources, peaks: { ...peaks }, ...(latest ? { runtime: latest } : {}) }),
  });
};

export const readMissionMetrics = words => {
  if (!(words instanceof Uint32Array) || words.length !== MISSION_METRICS_WORDS || !(words.buffer instanceof SharedArrayBuffer)) { throw new TypeError('Invalid mission metrics storage.'); }
  const version = Atomics.load(words, 0);
  if (!version || version % 2) { return undefined; }
  const sample = Array.from({ length: words.length }, (_, n) => Atomics.load(words, n));
  if (Atomics.load(words, 0) !== version) { return undefined; }
  if (sample[1] > 1024 * 1024 * 1024 || sample[2] > 192 * 1024 * 1024 || sample[3] > 128 || sample[4] > MISSION_RENDERER_BYTES || sample[5] > 4096 || sample[6] > 256) { throw new RangeError('Mission metrics exceed memory, worker or resource bounds.'); }
  if (sample[41] > 2000000) { throw new RangeError('Mission simulation backlog exceeds its bound.'); }
  if (sample[35] > 256) { throw new RangeError('Mission audio occupancy exceeds its voice object bound.'); }
  const floats = new Float32Array(Uint32Array.of(sample[32], sample[33]).buffer);
  if (sample[31] && (!sample[34] || sample[34] > sample[31] || !Number.isFinite(floats[0]) || floats[0] < 0 || !Number.isFinite(floats[1]) || floats[1] < 0 || floats[1] > 10)) { throw new RangeError('Invalid simulation time sample.'); }
  const simulation = sample[31] ? { samples: sample[31], seconds: floats[0], deltaSeconds: floats[1], epoch: sample[34] } : undefined;
  const names = ['wasmHeapBytes', 'translatedBackingBytes', 'activeWorkers', 'rendererResidentBytes', 'rendererResources', 'pipelines',
    'frames', 'commandWaitMicroseconds', 'readbackWaitMicroseconds', 'uploadWaitMicroseconds', 'drawWaitMicroseconds',
    'assetBytes', 'assetOperations', 'assetWaitMicroseconds', 'inactiveMilliseconds', 'restarts', 'childThreadHandles', 'objectReferences', 'vertexMacMask', 'vertexIluMask', 'pixelTextureModeMask', 'shaderPaths'];
  const result = Object.fromEntries(names.map((name, n) => [name, sample[n + 1]]));
  const bits = (mask, limit = 32) => Array.from({ length: limit }, (_, n) => n).filter(n => (mask >>> n) & 1);
  return { ...result, audioVoices: sample[35], audioVoiceRejections: sample[36], simulationSteps: sample[37], skippedViewportDraws: sample[38],
    updateMicroseconds: sample[39], renderMicroseconds: sample[40], simulationBacklogMicroseconds: sample[41], ...(simulation ? { simulation } : {}), shaderInventory: {
    vertexMac: bits(result.vertexMacMask, 16), vertexIlu: bits(result.vertexIluMask, 8), pixelTextureModes: bits(result.pixelTextureModeMask),
    paths: ['pretransformed', 'fixed-transform', 'vertex-program', 'pixel-combiners'].filter((name, n) => (result.shaderPaths >>> n) & 1),
    textureFormats: Array.from({ length: 256 }, (_, n) => n).filter(n => (sample[23 + Math.floor(n / 32)] >>> (n % 32)) & 1),
  } };

};
