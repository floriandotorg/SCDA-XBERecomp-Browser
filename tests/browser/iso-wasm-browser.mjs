import { createIsoMailbox } from '../../src/browser/iso-mailbox.mjs';
import { syntheticDisc } from './iso-wasm-fixture.mjs';

const workers = new Set();
let heartbeats = 0;
const timer = setInterval(() => { ++heartbeats; }, 1);
const request = (worker, message, transfer = []) => new Promise((resolve, reject) => {
  const timeout = setTimeout(() => reject(new Error('Worker response timed out.')), 15000);
  worker.addEventListener('message', ({ data }) => {
    clearTimeout(timeout);
    if (data.ok) { resolve(data); } else { reject(new Error(data.message ?? data.error?.code)); }
  }, { once: true });
  worker.addEventListener('error', () => {
    clearTimeout(timeout);
    reject(new Error('Worker failed.'));
  }, { once: true });
  worker.postMessage(message, transfer);
});

try {
  if (!crossOriginIsolated) { throw new Error('Cross origin isolation is required.'); }
  const io = new Worker(new URL('../../src/browser/iso-io-worker.mjs', import.meta.url), { type: 'module' });
  workers.add(io);
  const file = syntheticDisc();
  await request(io, { type: 'mount', file, generation: 1 });
  const generations = [];
  for (let n = 1; n <= 3; ++n) {
    if (n > 1) { await request(io, { type: 'generation', generation: n }); }
    const channel = new MessageChannel();
    const mailbox = createIsoMailbox();
    const ownership = new Int32Array(new SharedArrayBuffer(4));
    Atomics.store(ownership, 0, n);
    await request(io, { type: 'connect', port: channel.port1, mailbox, generation: n }, [channel.port1]);
    const guest = new Worker(new URL('./iso-wasm-worker.mjs', import.meta.url), { type: 'module' });
    workers.add(guest);
    const result = await request(guest, { port: channel.port2, mailbox, generation: n, ownership }, [channel.port2]);
    guest.terminate();
    workers.delete(guest);
    const { stats } = await request(io, { type: 'stats' });
    if (result.bytes !== 7 || result.partition !== 0 || !result.invalidDestination || !result.stale ||
        !result.validated || !result.rejectedMismatch ||
        stats.pending !== 0 || stats.stagingBytes !== 0 || stats.cacheBytes !== 0 || stats.bytesRead >= 32768) {
      throw new Error('Wasm ISO contract or resource bounds failed.');
    }
    generations.push({ ...result, stats });
  }
  await request(io, { type: 'close' });
  if (heartbeats === 0) { throw new Error('UI made no progress.'); }
  globalThis.isoWasmResults = { ok: true, heartbeats, generations };
} catch (error) {
  globalThis.isoWasmResults = { ok: false, message: error.message };
} finally {
  clearInterval(timer);
  for (const worker of workers) { worker.terminate(); }
  document.querySelector('#output').textContent = JSON.stringify(globalThis.isoWasmResults, null, 2);
}
