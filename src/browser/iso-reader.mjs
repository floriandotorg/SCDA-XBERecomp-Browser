export const ISO_LIMITS = Object.freeze({
  maxReadBytes: 1024 * 1024,
  maxStagingBytes: 4 * 1024 * 1024,
  maxConcurrency: 2,
  maxPending: 16,
});

export class IsoReadError extends Error {
  constructor(code, message) {
    super(message);
    this.name = 'IsoReadError';
    this.code = code;
  }
}

const fail = (code, message) => new IsoReadError(code, message);
const positiveInteger = (value) => Number.isSafeInteger(value) && value > 0;

export class BoundedIsoReader {
  #file;
  #limits;
  #generation = 0;
  #closed = false;
  #jobs = new Map();
  #queue = [];
  #active = 0;
  #stagingBytes = 0;
  #peakStagingBytes = 0;
  #peakConcurrency = 0;
  #bytesRead = 0;

  constructor(file, options = {}) {
    const limits = { ...ISO_LIMITS, ...options };
    if (!file || !Number.isSafeInteger(file.size) || file.size < 0 || typeof file.slice !== 'function') {
      throw fail('FILE', 'A local File with a safe byte size is required.');
    }
    if (!Object.values(limits).every(positiveInteger) || limits.maxReadBytes > limits.maxStagingBytes ||
        limits.maxPending < limits.maxConcurrency || limits.maxReadBytes > ISO_LIMITS.maxReadBytes ||
        limits.maxStagingBytes > ISO_LIMITS.maxStagingBytes || limits.maxConcurrency > ISO_LIMITS.maxConcurrency ||
        limits.maxPending > ISO_LIMITS.maxPending) {
      throw fail('LIMITS', 'Invalid ISO reader budgets.');
    }
    this.#file = file;
    this.#limits = Object.freeze(limits);
  }

  get stats() {
    return {
      generation: this.#generation,
      fileSize: this.#file.size,
      active: this.#active,
      pending: this.#active + this.#queue.length,
      stagingBytes: this.#stagingBytes,
      peakStagingBytes: this.#peakStagingBytes,
      peakConcurrency: this.#peakConcurrency,
      bytesRead: this.#bytesRead,
      cacheBytes: 0,
    };
  }

  beginGeneration = (generation) => {
    if (this.#closed) {
      throw fail('CLOSED', 'ISO reader is closed.');
    }
    if (!positiveInteger(generation) || generation <= this.#generation || generation > 0x7fffffff) {
      throw fail('GENERATION', 'Runtime generations must increase within the signed 32 bit range.');
    }
    this.#generation = generation;
    for (const job of this.#jobs.values()) {
      this.#reject(job, fail('CANCELLED', 'Runtime generation ended.'));
    }
    this.#queue = [];
  };

  readAt = (offset, length, requestId, generation) => {
    return new Promise((resolve, reject) => {
      if (this.#closed) {
        throw fail('CLOSED', 'ISO reader is closed.');
      }
      if (generation !== this.#generation || generation === 0) {
        throw fail('STALE', 'ISO request belongs to an inactive runtime.');
      }
      if (!positiveInteger(requestId) || requestId > 0x7fffffff) {
        throw fail('REQUEST', 'Invalid ISO request identifier.');
      }
      if (!Number.isSafeInteger(offset) || offset < 0 || !Number.isSafeInteger(length) || length < 0 ||
          length > this.#limits.maxReadBytes || offset > this.#file.size || length > this.#file.size - offset) {
        throw fail('RANGE', 'ISO byte range is outside the file or request budget.');
      }
      if (this.#jobs.has(requestId)) {
        throw fail('DUPLICATE', 'ISO request identifier is already pending.');
      }
      if (this.#active + this.#queue.length >= this.#limits.maxPending) {
        throw fail('BUSY', 'ISO request queue is full.');
      }
      if (length === 0) {
        resolve(new Uint8Array());
        return;
      }
      const job = { offset, length, requestId, generation, resolve, reject, settled: false };
      this.#jobs.set(requestId, job);
      this.#queue.push(job);
      this.#pump();
    });
  };

  cancel = (requestId, generation) => {
    const job = this.#jobs.get(requestId);
    if (!job || job.generation !== generation) {
      return;
    }
    this.#reject(job, fail('CANCELLED', 'ISO request was cancelled.'));
    this.#queue = this.#queue.filter((queued) => queued !== job);
    this.#pump();
  };

  close = () => {
    this.#closed = true;
    for (const job of this.#jobs.values()) {
      this.#reject(job, fail('CANCELLED', 'ISO reader was closed.'));
    }
    this.#queue = [];
  };

  #reject = (job, error) => {
    if (!job.settled) {
      job.settled = true;
      job.reject(error);
      this.#jobs.delete(job.requestId);
    }
  };

  #pump = () => {
    while (!this.#closed && this.#queue.length > 0 && this.#active < this.#limits.maxConcurrency) {
      const job = this.#queue[0];
      if (job.length > this.#limits.maxStagingBytes - this.#stagingBytes) {
        return;
      }
      this.#queue.shift();
      ++this.#active;
      this.#stagingBytes += job.length;
      this.#peakStagingBytes = Math.max(this.#peakStagingBytes, this.#stagingBytes);
      this.#peakConcurrency = Math.max(this.#peakConcurrency, this.#active);
      void this.#read(job);
    }
  };

  #read = async (job) => {
    try {
      const buffer = await this.#file.slice(job.offset, job.offset + job.length).arrayBuffer();
      this.#bytesRead += buffer.byteLength;
      if (!job.settled && job.generation === this.#generation && !this.#closed) {
        if (buffer.byteLength !== job.length) {
          this.#reject(job, fail('SHORT_READ', 'ISO range returned fewer bytes than requested.'));
        } else {
          job.settled = true;
          this.#jobs.delete(job.requestId);
          job.resolve(new Uint8Array(buffer));
        }
      }
    } catch {
      this.#reject(job, fail('READ_FAILED', 'The selected ISO could not be read.'));
    } finally {
      --this.#active;
      this.#stagingBytes -= job.length;
      this.#pump();
    }
  };
}
