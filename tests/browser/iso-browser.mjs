import { createIsoMailbox } from '../../src/browser/iso-mailbox.mjs';

const status = document.querySelector('#status');
const output = document.querySelector('#output');
const workers = new Set();
let timer;
let id = 0;
const pending = new Map();

const replyFrom = (worker) => new Promise((resolve, reject) => {
  const timeout = setTimeout(() => reject(new Error('Guest response timed out.')), 10000);
  worker.addEventListener('message', ({ data }) => { clearTimeout(timeout); resolve(data); }, { once: true });
  worker.addEventListener('error', () => { clearTimeout(timeout); reject(new Error('Guest worker failed.')); }, { once: true });
});

const run = async () => {
  if (!crossOriginIsolated || typeof SharedArrayBuffer !== 'function') {
    throw new Error('Cross origin isolation and SharedArrayBuffer are required.');
  }
  const io = new Worker(new URL('../../src/browser/iso-io-worker.mjs', import.meta.url), { type: 'module' });
  workers.add(io);
  io.addEventListener('message', ({ data }) => {
    const request = pending.get(data.id);
    if (request) {
      pending.delete(data.id);
      clearTimeout(request.timer);
      if (data.ok) {
        request.resolve(data);
      } else {
        request.reject(new Error(data.error.code));
      }
    }
  });
  const control = (message, transfer = []) => new Promise((resolve, reject) => {
    const requestId = ++id;
    const timeout = setTimeout(() => { pending.delete(requestId); reject(new Error('I/O response timed out.')); }, 5000);
    pending.set(requestId, { resolve, reject, timer: timeout });
    io.postMessage({ ...message, id: requestId }, transfer);
  });
  const bytes = Uint8Array.from({ length: 1024 * 1024 }, (_, n) => n % 251);
  const file = new File([bytes], 'invented.iso');
  await control({ type: 'mount', file, generation: 1 });
  let guestHeartbeats = 0;
  let totalRead = 0;
  status.textContent = 'Reading local synthetic File while guest workers wait';
  timer = setInterval(() => { ++guestHeartbeats; }, 1);
  const generations = [];
  for (let n = 1; n <= 3; ++n) {
    const generation = n;
    if (generation > 1) {
      await control({ type: 'generation', generation });
    }
    const runGuest = async () => {
      const guest = new Worker(new URL('./iso-guest-browser.mjs', import.meta.url), { type: 'module' });
      workers.add(guest);
      const channel = new MessageChannel();
      const mailbox = createIsoMailbox(65536);
      await control({ type: 'connect', port: channel.port1, mailbox, generation }, [channel.port1]);
      const response = replyFrom(guest);
      guest.postMessage({ port: channel.port2, mailbox, generation }, [channel.port2]);
      const result = await response;
      guest.terminate();
      workers.delete(guest);
      if (!result.ok) {
        throw new Error(result.message);
      }
      totalRead += result.bytesRead;
    };
    await Promise.all([runGuest(), runGuest()]);
    generations.push((await control({ type: 'stats' })).stats);
  }
  clearInterval(timer);
  await control({ type: 'close' });
  if (guestHeartbeats === 0 || totalRead !== 3 * 2 * 128 * 65536 || generations.at(-1).bytesRead !== totalRead ||
      generations.some((stats) => stats.peakConcurrency > 2 || stats.peakStagingBytes > 4 * 1024 * 1024 ||
        stats.stagingBytes !== 0 || stats.pending !== 0 || stats.cacheBytes !== 0)) {
    throw new Error('Independent progress or bounded reader accounting failed.');
  }
  return { ok: true, totalRead, guestHeartbeats, generations };
};

try {
  globalThis.isoResults = await run();
  status.textContent = 'ISO reader contracts passed';
  output.textContent = JSON.stringify(globalThis.isoResults, null, 2);
} catch (error) {
  globalThis.isoResults = { ok: false, message: error.message };
  status.textContent = `ISO reader error: ${error.message}`;
} finally {
  clearInterval(timer);
  for (const worker of workers) {
    worker.terminate();
  }
  for (const request of pending.values()) {
    clearTimeout(request.timer);
  }
  pending.clear();
}
