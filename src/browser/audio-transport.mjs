export const AUDIO_FRAMES = 16384;
export const AUDIO_CHANNELS = 6;
export const AUDIO_RATE = 48000;
// the game feeds by its own clock and the device plays by its clock, so the fill drifts: a starved ring waits for
// AUDIO_PRIME frames before playing again rather than playing each small chunk as it lands, and one past AUDIO_HIGH
// skips back to AUDIO_TARGET before the producer has to drop whole chunks
export const AUDIO_PRIME = 2048;
export const AUDIO_TARGET = 8192;
export const AUDIO_HIGH = 12288;

export class AudioQueue {
  constructor({ buffer, byteOffset, samplesOffset }) {
    if (!(buffer instanceof SharedArrayBuffer) || buffer.growable) { throw new TypeError('Audio requires fixed shared memory.'); }
    const span = (offset, bytes) => Number.isSafeInteger(offset) && offset >= 0 && offset % 4 === 0 && offset <= buffer.byteLength - bytes;
    const sampleBytes = AUDIO_FRAMES * AUDIO_CHANNELS * 4;
    if (!span(byteOffset, 32) || !span(samplesOffset, sampleBytes) || (byteOffset < samplesOffset + sampleBytes && samplesOffset < byteOffset + 32)) {
      throw new RangeError('Audio storage is unaligned, overlapping or outside shared memory.');
    }
    this.header = new Uint32Array(buffer, byteOffset, 8);
    this.samples = new Float32Array(buffer, samplesOffset, AUDIO_FRAMES * AUDIO_CHANNELS);
    this.primed = true;
  }

  render(left, right) {
    left.fill(0);
    right.fill(0);
    const header = this.header;
    const epoch = Atomics.load(header, 2);
    const write = Atomics.load(header, 0);
    let read = Atomics.load(header, 1);
    if (!Atomics.load(header, 3)) {
      Atomics.store(header, 1, write);
      return;
    }
    let queued = (write - read) >>> 0;
    if (queued > AUDIO_FRAMES || left.length !== right.length) {
      Atomics.store(header, 7, 1);
      return;
    }
    if (queued > AUDIO_HIGH) {
      const skipped = (write - AUDIO_TARGET) >>> 0;
      if (Atomics.compareExchange(header, 1, read, skipped) !== read) { return; }
      Atomics.add(header, 4, queued - AUDIO_TARGET);
      read = skipped;
      queued = AUDIO_TARGET;
    }
    if (!this.primed && queued < AUDIO_PRIME) {
      Atomics.add(header, 5, left.length);
      return;
    }
    this.primed = queued >= left.length;
    const frames = Math.min(queued, left.length);
    for (let n = 0; n < frames; ++n) {
      const offset = ((read + n) % AUDIO_FRAMES) * AUDIO_CHANNELS;
      const centre = this.samples[offset + 2] * 0.707;
      const l = this.samples[offset] + centre + this.samples[offset + 4] * 0.707;
      const r = this.samples[offset + 1] + centre + this.samples[offset + 5] * 0.707;
      left[n] = Number.isFinite(l) ? Math.max(-1, Math.min(1, l)) : 0;
      right[n] = Number.isFinite(r) ? Math.max(-1, Math.min(1, r)) : 0;
    }
    // the page flushes the ring by moving the read position; a flush during this render wins and its frames are dropped
    if (Atomics.load(header, 2) !== epoch || !Atomics.load(header, 3) || Atomics.compareExchange(header, 1, read, (read + frames) >>> 0) !== read) {
      left.fill(0);
      right.fill(0);
      return;
    }
    Atomics.add(header, 5, left.length - frames);
    Atomics.add(header, 6, frames);
  }
}
