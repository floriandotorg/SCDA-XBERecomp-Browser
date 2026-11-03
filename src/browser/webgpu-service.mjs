import { waitForMailboxRequest, serveMailbox, closeMailbox, Status, MAX_BYTES, Opcode } from './webgpu-transport.mjs';

export const createGPUService = async ({ canvas, moduleURL, diagnostic = () => {}, onPresented = () => {}, onAdapter = () => {} }) => {
  const { default: createModule } = await import(moduleURL);
  let resolveReady;
  let pending;
  let terminal;
  const readiness = new Promise(resolve => { resolveReady = resolve; });
  const module = await createModule({
    canvas,
    onGPUReady: (status, message) => resolveReady({ status, message }),
    onGPUAdapter: onAdapter,
    onGPUComplete: (status, bytes, message) => {
      if (pending) {
        const { resolve, command, resource, opcode } = pending;
        pending = undefined;
        if (status !== Status.Ok) { diagnostic({ command, resource, status, message }); }
        if (status === Status.Ok && opcode === Opcode.Present) { onPresented({ command, resource, resources: module._webgpu_resources() }); }
        resolve({ status, bytes: message ? new TextEncoder().encode(message) : bytes });
      }
    },
    onGPUError: message => {
      terminal = Status.Error;
      diagnostic({ command: pending?.command ?? 0, resource: pending?.resource ?? 0, status: Status.Error, message });
      if (pending) { pending.resolve({ status: Status.Error, bytes: new TextEncoder().encode(message) }); pending = undefined; }
    },
    onGPULost: message => {
      terminal = Status.Lost;
      diagnostic({ command: pending?.command ?? 0, resource: pending?.resource ?? 0, status: Status.Lost, message });
      if (pending) { pending.resolve({ status: Status.Lost, bytes: new TextEncoder().encode(message) }); pending = undefined; }
    },
    printErr: message => diagnostic({ status: Status.Error, message })
  });
  module._webgpu_start();
  const initialized = await readiness;
  if (initialized.status !== Status.Ok) { module._webgpu_shutdown(); throw new Error(initialized.message); }
  return {
    async execute(packet) {
      if (terminal !== undefined) { return { status: terminal }; }
      if (pending) { throw new Error('GPU service busy'); }
      if (packet.bytes.byteLength > MAX_BYTES) { throw new Error('GPU request exceeds bounded capacity'); }
      const pointer = module._malloc(packet.bytes.byteLength || 1);
      module.HEAPU8.set(packet.bytes, pointer);
      try {
        return await new Promise(resolve => {
          pending = { resolve, command: packet.command, resource: packet.resource, opcode: packet.opcode };
          try {
            module._webgpu_submit(packet.opcode, packet.resource, pointer, packet.bytes.byteLength);
          } catch (error) {
            pending = undefined;
            terminal = Status.Error;
            diagnostic({ command: packet.command, resource: packet.resource, status: Status.Error, message: String(error) });
            module._webgpu_shutdown();
            resolve({ status: Status.Error, bytes: new TextEncoder().encode(String(error)) });
          }
        });
      } finally { module._free(pointer); }
    },
    resources: () => module._webgpu_resources(),
    shutdown() {
      terminal = Status.Closed;
      module._webgpu_shutdown();
      if (pending) { pending.resolve({ status: Status.Closed }); pending = undefined; }
    }
  };
};

export const startGPUWorker = ({ canvas, mailbox, moduleURL, onDiagnostic = () => {}, onPresented = () => {}, onAdapter = () => {}, signal, initTimeoutMs = 20000, shutdownTimeoutMs = 5000 }) => {
  const worker = new Worker(new URL('./webgpu-service.mjs', import.meta.url), { type: 'module', name: 'yielding-webgpu-service' });
  let resolveReady;
  let rejectReady;
  let resolveStopped;
  let initialized = false;
  let disposal;
  const ready = new Promise((resolve, reject) => { resolveReady = resolve; rejectReady = reject; });
  const stopped = new Promise(resolve => { resolveStopped = resolve; });
  const cancel = () => {
    closeMailbox(mailbox);
    if (!initialized) { rejectReady(new Error('GPU initialization cancelled')); }
  };
  const dispose = () => {
    if (disposal) { return disposal; }
    disposal = (async () => {
      clearTimeout(initTimer);
      cancel();
      signal?.removeEventListener('abort', abort);
      let timer;
      try {
        worker.postMessage({ type: 'shutdown' });
        const result = await Promise.race([stopped, new Promise((resolve, reject) => { timer = setTimeout(() => reject(new Error('GPU shutdown timed out')), shutdownTimeoutMs); })]);
        if (result.resources !== 0) { throw new Error('GPU shutdown did not release all resources'); }
        return { resources: 0, forced: false };
      } catch (error) {
        onDiagnostic({ status: Status.Error, message: String(error), forcedTermination: true });
        throw error;
      } finally {
        clearTimeout(timer);
        worker.terminate();
      }
    })();
    return disposal;
  };
  const abort = () => { cancel(); void dispose().catch(() => {}); };
  const fail = message => {
    clearTimeout(initTimer);
    closeMailbox(mailbox, Status.Error);
    rejectReady(new Error(message));
    onDiagnostic({ status: Status.Error, message });
    void dispose().catch(() => {});
  };
  const initTimer = setTimeout(() => fail('GPU initialization timed out'), initTimeoutMs);
  worker.onmessage = event => {
    if (event.data.type === 'ready') { clearTimeout(initTimer); initialized = true; resolveReady(); }
    else if (event.data.type === 'stopped') { resolveStopped(event.data); }
    else if (event.data.type === 'error') { fail(event.data.message); }
    else if (event.data.type === 'diagnostic') { onDiagnostic(event.data.diagnostic); }
    else if (event.data.type === 'presented') { onPresented(event.data.details); }
    else if (event.data.type === 'adapter') { onAdapter(event.data.details); }
  };
  worker.onerror = event => fail(event.message);
  worker.postMessage({ type: 'start', canvas, mailbox, moduleURL: new URL(moduleURL, location.href).href }, [canvas]);
  signal?.addEventListener('abort', abort, { once: true });
  if (signal?.aborted) { abort(); }
  return { ready, worker, webgpuMailbox: mailbox, cancel, dispose, shutdown: dispose };
};

if (typeof WorkerGlobalScope !== 'undefined' && globalThis instanceof WorkerGlobalScope) {
  let service;
  let mailbox;
  let stopped = false;
  globalThis.onmessage = async event => {
    if (event.data.type === 'shutdown') {
      stopped = true;
      if (mailbox) { closeMailbox(mailbox); }
      if (service) { service.shutdown(); globalThis.postMessage({ type: 'stopped', resources: service.resources() }); }
      return;
    }
    if (event.data.type !== 'start' || service) { return; }
    mailbox = event.data.mailbox;
    try {
      service = await createGPUService({ ...event.data, diagnostic: diagnostic => globalThis.postMessage({ type: 'diagnostic', diagnostic }), onPresented: details => globalThis.postMessage({ type: 'presented', details }), onAdapter: details => globalThis.postMessage({ type: 'adapter', details }) });
      if (stopped) { service.shutdown(); globalThis.postMessage({ type: 'stopped', resources: service.resources() }); return; }
      globalThis.postMessage({ type: 'ready' });
      while (!stopped && await waitForMailboxRequest(mailbox)) {
        await serveMailbox(mailbox, async packet => {
          const result = await service.execute(packet);
          if (packet.opcode === Opcode.Shutdown) {
            service.shutdown();
            stopped = true;
          }
          return result;
        });
      }
    } catch (error) {
      closeMailbox(mailbox, Status.Error);
      service?.shutdown();
      globalThis.postMessage({ type: 'error', message: String(error) });
      if (stopped) { globalThis.postMessage({ type: 'stopped', resources: service?.resources() ?? 0 }); }
    }
  };
}
