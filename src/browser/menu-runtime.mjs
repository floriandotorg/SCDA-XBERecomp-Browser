import { createBootRuntime } from './boot-runtime.mjs';
import { createMenuInput } from './menu-input.mjs';
import { createMailbox } from './webgpu-transport.mjs';
import { startGPUWorker } from './webgpu-service.mjs';

const waitReady = async (ready, signal, timeoutMs) => {
  signal.throwIfAborted();
  let timer;
  let cancel;
  const cancelled = new Promise((resolve, reject) => {
    cancel = () => reject(signal.reason ?? new DOMException('Stopped.', 'AbortError'));
    signal.addEventListener('abort', cancel, { once: true });
    timer = setTimeout(() => reject(new Error('WebGPU service initialization timed out.')), timeoutMs);
  });
  try { await Promise.race([ready, cancelled]); }
  finally {
    clearTimeout(timer);
    signal.removeEventListener('abort', cancel);
  }
};

export const createMenuRuntime = ({ acquireCanvas, createRuntime = createBootRuntime, createGPU = startGPUWorker,
  createInput = createMenuInput, onPresented = () => {}, onAdapter = () => {}, onDiagnostic = () => {},
  readyTimeoutMs = 20000, ...runtimeOptions } = {}) => {
  if (typeof acquireCanvas !== 'function') { throw new TypeError('A fresh owned canvas is required for each menu runtime.'); }
  if (!Number.isInteger(readyTimeoutMs) || readyTimeoutMs < 1 || readyTimeoutMs > 30000) { throw new RangeError('Invalid GPU initialization deadline.'); }
  return createRuntime({ ...runtimeOptions, createServices: async ({ generation, signal, owned, report }) => {
    signal.throwIfAborted();
    const canvas = await acquireCanvas();
    signal.throwIfAborted();
    if (!owned()) { throw new DOMException('Stopped.', 'AbortError'); }
    const mailbox = createMailbox();
    let producer;
    let cancelled = false;
    let disposed;
    let presenting = false;
    const live = () => !cancelled && owned() && !signal.aborted;
    const gpu = createGPU({
      canvas, mailbox, moduleURL: new URL('./menu_gpu.mjs', import.meta.url),
      onAdapter: (details) => { if (live()) { onAdapter(details); } },
      onPresented: (details) => {
        if (!live()) { return; }
        onPresented(details);
        if (!presenting) {
          presenting = true;
          report({ generation, phase: 'running', executing: true });
        }
      },
      onDiagnostic: (details) => {
        if (!live()) { return; }
        onDiagnostic(details);
        report({ generation, ok: false, code: details.status === 3 ? 'WEBGPU_DEVICE_LOST' : 'WEBGPU_VALIDATION',
          message: `WebGPU command ${details.command ?? 0}, resource ${details.resource ?? 0}: ${details.message ?? 'GPU operation failed.'}` });
      },
    });
    const cancel = () => {
      if (cancelled) { return; }
      cancelled = true;
      producer?.dispose();
      gpu.cancel();
    };
    const dispose = () => {
      disposed ??= (async () => { cancel(); await gpu.dispose(); })();
      return disposed;
    };
    try {
      await waitReady(gpu.ready, signal, readyTimeoutMs);
      if (!live()) { throw new DOMException('Stopped.', 'AbortError'); }
      return {
        webgpuMailbox: mailbox, cancel, dispose,
        handle: (data) => {
          if (data.phase !== 'input') { return false; }
          if (!live()) { return true; }
          try {
            if (producer || data.words !== 2) { throw new Error('Unexpected menu input storage publication.'); }
            producer = createInput({ buffer: data.buffer, byteOffset: data.byteOffset, signal });
          } catch (error) {
            report({ generation, ok: false, code: 'INPUT_PROTOCOL', message: error.message });
          }
          return true;
        },
      };
    } catch (error) {
      await dispose();
      throw error;
    }
  } });
};
