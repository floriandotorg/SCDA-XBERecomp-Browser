import { waitForMailboxRequest, serveMailbox, closeMailbox, createCommandRing, Status, MAX_BYTES, Opcode } from './webgpu-transport.mjs';

export const createGPUService = async ({ canvas, moduleURL, mailbox, diagnostic = () => {}, onPresented = () => {}, onAdapter = () => {} }) => {
  const { default: createModule } = await import(moduleURL);
  let resolveReady;
  let pending;
  let terminal;
  const readiness = new Promise(resolve => { resolveReady = resolve; });
  const module = await createModule({
    canvas,
    readbackMailbox: mailbox,
    onGPUReady: (status, message) => resolveReady({ status, message }),
    onGPUAdapter: onAdapter,
    onGPUComplete: (status, bytes, message) => {
      if (pending) {
        const { resolve, command, resource, opcode } = pending;
        pending = undefined;
        if (status !== Status.Ok) { diagnostic({ command, resource, status, message }); }
        if (status === Status.Ok && opcode === Opcode.Present) { onPresented({ command, resource, resources: module._webgpu_resources(), serviceHeapBytes: module.HEAPU8.byteLength }); }
        resolve({ status, bytes: message ? new TextEncoder().encode(message) : bytes });
      }
    },
    onGPUError: message => {
      terminal = Status.Error;
      diagnostic({ command: pending?.command ?? 0, resource: pending?.resource ?? 0, status: Status.Error, message });
      if (pending) { pending.resolve({ status: Status.Error, bytes: new TextEncoder().encode(message) }); pending = undefined; }
    },
    onGPUDeferredError: details => {
      if (terminal !== undefined) { return; }
      terminal = Status.Error;
      diagnostic(details);
      if (pending) { pending.resolve({ status: Status.Error, bytes: new TextEncoder().encode(details.message) }); pending = undefined; }
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
            module._webgpu_submit(packet.opcode, packet.resource, pointer, packet.bytes.byteLength, packet.command);
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

export const createGPUCommandQueue = (service, dimensions) => {
  let tail = Promise.resolve();
  let retired = false;
  let presented = false;
  let capturing = false;
  const enqueue = operation => {
    const result = tail.then(() => {
      if (retired) { throw new Error('GPU command queue is retired.'); }
      return operation();
    });
    tail = result.catch(() => {});
    return result;
  };
  return {
    execute: packet => enqueue(async () => {
      const result = await service.execute(packet);
      if (packet.opcode === Opcode.Configure && result.status === Status.Ok) { presented = false; }
      if (packet.opcode === Opcode.Present && result.status === Status.Ok) { presented = true; }
      return result;
    }),
    async capture() {
      if (retired) { throw new Error('GPU command queue is retired.'); }
      if (capturing) { throw new Error('A frame capture is already pending.'); }
      capturing = true;
      try {
        return await enqueue(async () => {
          if (!presented) { throw new Error('Frame capture requires a completed presentation.'); }
          const { width, height } = dimensions();
          if (!Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1 || width * height * 4 > MAX_BYTES) {
            throw new Error('Frame capture dimensions exceed the readback budget.');
          }
          const result = await service.execute({ opcode: Opcode.Readback, resource: 0, command: 0, bytes: new Uint8Array() });
          if (retired || result.status !== Status.Ok || result.bytes?.byteLength !== width * height * 4) {
            throw new Error('Frame capture readback failed or was retired.');
          }
          return { width, height, pixels: result.bytes.slice() };
        });
      } finally { capturing = false; }
    },
    shutdown() {
      if (retired) { return; }
      retired = true;
      service.shutdown();
    },
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
  let commands;
  let mailbox;
  let stopped = false;
  globalThis.onmessage = async event => {
    if (event.data.type === 'capture') {
      const id = event.data.id;
      if (!Number.isInteger(id) || id < 1 || id > 0xffffffff) { return; }
      try {
        if (!commands || stopped) { throw new Error('GPU frame capture is unavailable.'); }
        const frame = await commands.capture();
        if (!stopped) { globalThis.postMessage({ type: 'captured', id, ...frame }, [frame.pixels.buffer]); }
      } catch (error) {
        if (!stopped) { globalThis.postMessage({ type: 'capture-error', id, message: String(error) }); }
      }
      return;
    }
    if (event.data.type === 'shutdown') {
      stopped = true;
      if (mailbox) { closeMailbox(mailbox); }
      if (service) { if (commands) { commands.shutdown(); } else { service.shutdown(); } globalThis.postMessage({ type: 'stopped', resources: service.resources() }); }
      return;
    }
    if (event.data.type !== 'start' || service) { return; }
    mailbox = event.data.mailbox;
    try {
      service = await createGPUService({ ...event.data, diagnostic: diagnostic => globalThis.postMessage({ type: 'diagnostic', diagnostic }), onPresented: details => globalThis.postMessage({ type: 'presented', details }), onAdapter: details => globalThis.postMessage({ type: 'adapter', details }) });
      if (stopped) { service.shutdown(); globalThis.postMessage({ type: 'stopped', resources: service.resources() }); return; }
      commands = createGPUCommandQueue(service, () => ({ width: event.data.canvas.width, height: event.data.canvas.height }));
      globalThis.postMessage({ type: 'ready' });
      const header = new Int32Array(mailbox, 0, 16);
      const ring = createCommandRing(mailbox);
      const execute = packet => commands.execute(packet);
      while (!stopped) {
        const doorbell = ring?.doorbell();
        const state = Atomics.load(header, 0);
        if (state === 4) { break; }
        if (state === 1) {
          if (ring) { await ring.drain(ring.bound(), execute); }
          await serveMailbox(mailbox, async packet => {
            const result = await commands.execute(packet);
            if (packet.opcode === Opcode.Shutdown) {
              commands.shutdown();
              stopped = true;
            }
            return result;
          });
          continue;
        }
        if (ring?.pending()) { await ring.drain(ring.bound(), execute); continue; }
        if (ring) { await ring.wait(doorbell); }
        else if (!await waitForMailboxRequest(mailbox)) { break; }
      }
    } catch (error) {
      closeMailbox(mailbox, Status.Error);
      if (commands) { commands.shutdown(); } else { service?.shutdown(); }
      globalThis.postMessage({ type: 'error', message: String(error) });
      if (stopped) { globalThis.postMessage({ type: 'stopped', resources: service?.resources() ?? 0 }); }
    }
  };
}
