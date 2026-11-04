export const AUDIO_FRAMES = 16384;
export const AUDIO_CHANNELS = 6;
export const AUDIO_RATE = 48000;

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
  }

  render(left, right) {
    left.fill(0);
    right.fill(0);
    const header = this.header;
    const epoch = Atomics.load(header, 2);
    const write = Atomics.load(header, 0);
    const read = Atomics.load(header, 1);
    if (!Atomics.load(header, 3)) {
      Atomics.store(header, 1, write);
      return;
    }
    const queued = (write - read) >>> 0;
    if (queued > AUDIO_FRAMES || left.length !== right.length) {
      Atomics.store(header, 7, 1);
      return;
    }
    const frames = Math.min(queued, left.length);
    for (let n = 0; n < frames; ++n) {
      const offset = ((read + n) % AUDIO_FRAMES) * AUDIO_CHANNELS;
      const centre = this.samples[offset + 2] * 0.707;
      const l = this.samples[offset] + centre + this.samples[offset + 4] * 0.707;
      const r = this.samples[offset + 1] + centre + this.samples[offset + 5] * 0.707;
      left[n] = Number.isFinite(l) ? Math.max(-1, Math.min(1, l)) : 0;
      right[n] = Number.isFinite(r) ? Math.max(-1, Math.min(1, r)) : 0;
    }
    if (Atomics.load(header, 2) !== epoch || !Atomics.load(header, 3)) {
      left.fill(0);
      right.fill(0);
      return;
    }
    Atomics.store(header, 1, (read + frames) >>> 0);
    Atomics.add(header, 5, left.length - frames);
    Atomics.add(header, 6, frames);
  }
}
