import { createWorkerBroker } from './boot-worker-broker.mjs';
import { createIsoMailbox, finishIsoRead, isoMailboxHeader } from './iso-mailbox.mjs';
import { validateHostSettings } from './host-settings.mjs';

// image: the recompiled module this boot runs, story or versus; carry: the hand over from the image it replaces
export const createBootRuntime = ({ WorkerClass = Worker, createBroker = createWorkerBroker, createServices, observeIo = false, developmentStart = false, hostSettings,
  image = 'story', carry } = {}) => {
  if (typeof developmentStart !== 'boolean') { throw new TypeError('Invalid development start option.'); }
  if (!['story', 'versus'].includes(image) || (carry !== undefined && !(carry instanceof Uint8Array))) { throw new TypeError('Invalid image switch.'); }
  if (hostSettings !== undefined) { validateHostSettings(hostSettings); }
  const broker = createBroker({ WorkerClass });
  const pending = new Map();
  const ports = new Set();
  let io;
  let mailbox;
  let requestId = 0;
  let retired = false;
  let started = false;
  let progress;
  let sample;
  let ioTimer;
  let ioBytes = 0;
  let statsPending = false;
  let services;
  let initialization;
  let serviceCancelled = false;
  let cancellationError;
  let termination;
  const cancelServices = () => {
    if (!services || serviceCancelled) { return; }
    serviceCancelled = true;
    try { services.cancel?.(); }
    catch (error) { cancellationError = error; }
  };
  const cancellation = () => Object.assign(new Error('Boot generation was cancelled.'), { code: 'CANCELLED' });
  const checkpoint = (signal) => {
    if (retired || signal.aborted) { throw cancellation(); }
  };
  const request = (data, transfer = []) => new Promise((resolve, reject) => {
    if (requestId === 0x7fffffff) { reject(new Error('ISO control request budget exhausted.')); return; }
    const id = ++requestId;
    pending.set(id, { resolve, reject });
    try { io.postMessage({ ...data, id }, transfer); }
    catch (error) { pending.delete(id); reject(error); }
  });
  const cancelIo = () => {
    retired = true;
    clearInterval(ioTimer);
    cancelServices();
    progress = undefined;
    sample = undefined;
    for (const entry of pending.values()) { entry.reject(cancellation()); }
    pending.clear();
    if (mailbox) {
      const header = isoMailboxHeader(mailbox);
      finishIsoRead(mailbox, Atomics.load(header, 1), Atomics.load(header, 2), undefined, 'CANCELLED');
    }
    for (const port of ports) { port.close(); }
    ports.clear();
    if (io) { io.terminate(); io = undefined; }
  };
  return Object.freeze({
    start: async ({ source, generation, ownership, signal, report }) => {
      if (started) { throw new Error('A disposable runtime owns one boot only.'); }
      started = true;
      checkpoint(signal);
      const owned = () => !retired && !signal.aborted && Atomics.load(ownership, 0) === generation;
      const publish = (data) => {
        if (!owned() || data?.generation !== generation) { return; }
        if (data.phase === 'restarting') { progress = undefined; }
        if (data.phase === 'progress') {
          if (progress) { return; }
          if (!(data.buffer instanceof SharedArrayBuffer) || data.buffer.growable || data.buffer.byteLength > 0x80000000 ||
              !Number.isInteger(data.address) || data.address <= 0 || data.address % 4 || data.address + 4 > data.buffer.byteLength) {
            report({ generation, ok: false, code: 'PROGRESS_PROTOCOL', message: 'Invalid shared runtime progress descriptor.' });
            return;
          }
          progress = new Uint32Array(data.buffer, data.address, 1);
          return;
        }
        if (services?.handle?.(data)) { return; }
        if (data.phase === 'running') {
          report({ generation, ok: false, code: 'BOOT_PROTOCOL', message: 'Guest callbacks cannot establish a running game.' });
          return;
        }
        report(data);
      };
      sample = () => {
        if (!owned()) { return undefined; }
        if (io && !statsPending) {
          statsPending = true;
          void request({ type: 'stats' }).then(({ stats }) => {
            if (owned() && stats?.generation === generation && Number.isSafeInteger(stats.bytesRead) && stats.bytesRead >= ioBytes) {
              ioBytes = stats.bytesRead;
              if (observeIo) { publish({ generation, phase: 'io', stats }); }
            }
          }, () => {}).finally(() => { statsPending = false; });
        }
        const execution = progress ? Atomics.load(progress, 0) : 0;
        return owned() && (progress || ioBytes > 0) ? { execution, ioBytes } : undefined;
      };
      const workerError = (error) => {
        for (const entry of pending.values()) { entry.reject(Object.assign(new Error(error.message ?? 'Worker failed.'), { code: 'WORKER' })); }
        pending.clear();
        publish({ generation, ok: false, code: 'WORKER', message: error.message ?? 'Worker failed.' });
      };
      io = new WorkerClass(new URL('./iso-io-worker.mjs', import.meta.url), { type: 'module' });
      io.addEventListener('message', ({ data }) => {
        const entry = pending.get(data?.id);
        if (!entry) { return; }
        pending.delete(data.id);
        if (data.ok) { entry.resolve(data); }
        else { entry.reject(Object.assign(new Error(data.error?.message ?? 'ISO I/O failed.'), { code: data.error?.code ?? 'IO' })); }
      });
      io.addEventListener('error', workerError);
      io.addEventListener('messageerror', () => workerError({ message: 'ISO worker message could not be decoded.' }));
      await request({ type: 'mount', file: source, generation });
      checkpoint(signal);
      const channel = new MessageChannel();
      ports.add(channel.port1);
      ports.add(channel.port2);
      mailbox = createIsoMailbox();
      await request({ type: 'connect', port: channel.port1, mailbox, generation }, [channel.port1]);
      checkpoint(signal);
      if (createServices) {
        initialization = Promise.resolve().then(() => createServices({ generation, signal, owned, report: (data) => {
          if (!owned() || data.generation !== generation) { return; }
          if (data.phase === 'running') { report(data); }
          else { publish(data); }
        } })).then((value) => {
          services = value;
          if (retired) { cancelServices(); }
          return value;
        });
        await initialization;
        checkpoint(signal);
      }
      const entry = image === 'versus' ? './versus-boot-worker.mjs' : './boot-worker.mjs';
      const guest = broker.create(new URL(entry, import.meta.url), { type: 'module', name: `${image}-boot` }, publish, workerError);
      guest.postMessage({ port: channel.port2, mailbox, generation, ownership, size: source.size,
        developmentStart, ...(hostSettings ? { hostSettings } : {}), ...(carry ? { carry } : {}),
        ...(services?.webgpuMailbox ? { webgpuMailbox: services.webgpuMailbox } : {}) }, [channel.port2]);
      if (observeIo) {
        ioTimer = setInterval(() => { sample?.(); }, 1000);
        ioTimer.unref?.();
      }
    },
    cancelIo,
    sampleProgress: () => sample?.(),
    terminateAll: () => {
      termination ??= (async () => {
        cancelIo();
        let failure;
        try { await broker.terminateAll(); }
        catch (error) { failure = error; }
        await initialization?.catch(() => {});
        cancelServices();
        try { await services?.dispose?.(); }
        catch (error) { failure ??= error; }
        failure ??= cancellationError;
        if (failure) { throw failure; }
      })();
      return termination;
    },
  });
};
