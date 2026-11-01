import { createIsoMailbox } from '../../src/browser/iso-mailbox.mjs';

const workers = new Set();
const pending = new Set();
const run = document.querySelector('#run');
const stop = document.querySelector('#stop');
const output = document.querySelector('#output');
let cancelled = false;
const request = (worker, message, transfer = []) => new Promise((resolve, reject) => {
  const fail = (error) => { clearTimeout(timeout); pending.delete(fail); reject(error); };
  const timeout = setTimeout(() => fail(new Error('Independent watchdog expired.')), 30000);
  pending.add(fail);
  worker.addEventListener('message', ({ data }) => {
    clearTimeout(timeout);
    pending.delete(fail);
    if (data.ok) { resolve(data); } else { reject(new Error(data.message ?? data.error?.code)); }
  }, { once: true });
  worker.addEventListener('error', () => fail(new Error('Worker failed.')), { once: true });
  worker.postMessage(message, transfer);
});
const terminate = () => {
  for (const worker of workers) { worker.terminate(); }
  workers.clear();
  for (const fail of [...pending]) { fail(new Error('Stopped.')); }
};
stop.onclick = () => { cancelled = true; terminate(); };
run.onclick = async () => {
  cancelled = false;
  run.disabled = true;
  stop.disabled = false;
  globalThis.isoLocalResults = undefined;
  let heartbeats = 0;
  const timer = setInterval(() => { ++heartbeats; }, 1);
  try {
    if (!crossOriginIsolated) { throw new Error('Cross origin isolation is required.'); }
    const manifestFile = document.querySelector('#manifest').files[0];
    const file = document.querySelector('#iso').files[0];
    if (!file || !manifestFile || manifestFile.size > 65536) { throw new Error('Select an ISO and a manifest of at most 64 KiB.'); }
    const manifest = JSON.parse(await manifestFile.text());
    if (cancelled) { throw new Error('Stopped.'); }
    output.textContent = 'Preparing original image. Guest execution is disabled.';
    const io = new Worker(new URL('../../src/browser/iso-io-worker.mjs', import.meta.url), { type: 'module' });
    workers.add(io);
    await request(io, { type: 'mount', file, generation: 1 });
    const generations = [];
    for (let n = 1; n <= 3; ++n) {
      if (n > 1) { await request(io, { type: 'generation', generation: n }); }
      const channel = new MessageChannel();
      const mailbox = createIsoMailbox();
      const ownership = new Int32Array(new SharedArrayBuffer(4));
      Atomics.store(ownership, 0, n);
      await request(io, { type: 'connect', port: channel.port1, mailbox, generation: n }, [channel.port1]);
      const guest = new Worker(new URL('./iso-local-worker.mjs', import.meta.url), { type: 'module' });
      workers.add(guest);
      const result = await request(guest, { port: channel.port2, mailbox, generation: n, ownership, size: file.size, manifest }, [channel.port2]);
      Atomics.store(ownership, 0, n + 1);
      guest.terminate();
      workers.delete(guest);
      const { stats } = await request(io, { type: 'stats' });
      generations.push({ ...result, stats });
    }
    await request(io, { type: 'close' });
    globalThis.isoLocalResults = { ok: true, fileBytes: file.size, heartbeats, generations };
  } catch (error) {
    globalThis.isoLocalResults = { ok: false, cancelled, message: error.message };
  } finally {
    clearInterval(timer);
    terminate();
    run.disabled = false;
    stop.disabled = true;
    output.textContent = JSON.stringify(globalThis.isoLocalResults, null, 2);
  }
};
