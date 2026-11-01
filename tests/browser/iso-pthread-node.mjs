import { parentPort, workerData } from 'node:worker_threads';
import { installIsoIoService } from '../../src/browser/iso-io-service.mjs';
import { createWasmIsoReader } from '../../src/browser/iso-wasm.mjs';
import { syntheticDisc } from './iso-wasm-fixture.mjs';

const { role, port, mailbox, ownership, scenario, audit } = workerData;
const counts = new Int32Array(audit);
const owner = new Int32Array(ownership);

if (role === 'io') {
  const control = new EventTarget();
  let response;
  control.postMessage = (message) => { response = message; };
  installIsoIoService(control);
  const request = (data) => {
    control.dispatchEvent(new MessageEvent('message', { data }));
    if (!response.ok) { throw new Error(JSON.stringify(response)); }
    return response;
  };
  const fixture = syntheticDisc({ large: true });
  const file = {
    size: fixture.size,
    slice: (start, end) => {
      if (start < 2 ** 32) { return fixture.slice(start, end); }
      Atomics.add(counts, 0, 1);
      return { arrayBuffer: async () => {
        if (scenario === 'denied') { throw new Error('Invented denied ISO read'); }
        if (scenario === 'stale-after') { Atomics.store(owner, 0, 2); }
        if (scenario === 'cancel') {
          setTimeout(() => {
            Atomics.store(owner, 0, 0);
            request({ type: 'generation', generation: 2 });
          }, 10);
        }
        await new Promise((resolve) => setTimeout(resolve,
          ['timeout', 'cancel', 'stop'].includes(scenario) ? 150 : 15));
        return fixture.slice(start, scenario === 'short' ? end - 1 : end).arrayBuffer();
      } };
    },
  };
  request({ type: 'mount', file, generation: 1 });
  request({ type: 'connect', port, mailbox, generation: 1 });
  parentPort.on('message', async ({ type }) => {
    if (type === 'settle') {
      await new Promise((resolve) => setTimeout(resolve, 200));
      parentPort.postMessage(request({ type: 'stats' }));
    }
    if (type === 'close') {
      parentPort.postMessage(request({ type: 'close' }));
    }
  });
} else {
  const done = new Int32Array(workerData.done);
  try {
    const { default: create } = await import(workerData.moduleUrl);
    let module;
    let joining = false;
    let duringJoin = 0;
    let loaderEvents = 0;
    let read;
    const calls = [];
    module = await create({ readIsoBytes: (offset, destination, length) => {
      if (joining) { ++duringJoin; }
      const call = { offset, length, joining, nativeJoining: module.isoJoining === true };
      calls.push(call);
      if (joining && offset > 2 ** 32) { module._pthread_io_started(); }
      try {
        return read(offset, destination, length);
      } catch (error) {
        call.error = error.code;
        throw error;
      }
    } });
    read = createWasmIsoReader({ port, mailbox, generation: 1, ownership: owner,
      getHeap: () => module.HEAPU8, timeoutMs: scenario === 'timeout' ? 35 : 2000 });
    if (module._mount_disc(6 * 1024 ** 3) !== 1) { throw new Error(`Mount failed: ${module.isoError}`); }
    if (scenario === 'stale-before') { Atomics.store(owner, 0, 2); }
    const interval = setInterval(() => { ++loaderEvents; }, 1);
    queueMicrotask(() => { ++loaderEvents; });
    const rounds = [];
    const modes = scenario === 'bounds' ? [2, 3, 4] : scenario === 'stop' ? [1] :
      scenario === 'success' ? [0, 0, 0] : [0];
    for (const mode of modes) {
      joining = true;
      const joined = module._pthread_reads(mode);
      joining = false;
      rounds.push({ joined, workers: [0, 1].map((n) => ({
        completed: module._pthread_result(0, n), unchanged: module._pthread_result(1, n),
        matched: module._pthread_result(2, n),
      })), owned: module._pthread_result(3, 0), unbound: module._pthread_result(4, 0),
      rejected: module._pthread_result(5, 0), distinct: module._pthread_result(6, 0),
      stoppedDuringIo: module._pthread_result(7, 0) });
    }
    clearInterval(interval);
    parentPort.postMessage({ ok: true, rounds, duringJoin, loaderEvents, calls, error: module.isoError });
  } catch (error) {
    parentPort.postMessage({ ok: false, error: error.stack ?? String(error) });
  } finally {
    Atomics.store(done, 0, 1);
    Atomics.notify(done, 0);
  }
}
