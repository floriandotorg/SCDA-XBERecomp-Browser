import { createBootRuntime } from './boot-runtime.mjs';
import { createMenuInput } from './menu-input.mjs';
import { createMissionControls } from './mission-controls.mjs';
import { MISSION_METRICS_WORDS, readMissionMetrics } from './mission-metrics.mjs';
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
  createInput = createMenuInput, createMotion = createMissionControls, onPresented = () => {}, onAdapter = () => {}, onDiagnostic = () => {},
  readyTimeoutMs = 20000, ...runtimeOptions } = {}) => {
  if (typeof acquireCanvas !== 'function') { throw new TypeError('A fresh owned canvas is required for each menu runtime.'); }
  if (!Number.isInteger(readyTimeoutMs) || readyTimeoutMs < 1 || readyTimeoutMs > 30000) { throw new RangeError('Invalid GPU initialization deadline.'); }
  return createRuntime({ ...runtimeOptions, observeIo: true, createServices: async ({ generation, signal, owned, report }) => {
    signal.throwIfAborted();
    const canvas = await acquireCanvas();
    signal.throwIfAborted();
    if (!owned()) { throw new DOMException('Stopped.', 'AbortError'); }
    const mailbox = createMailbox();
    let producer;
    let motion;
    let metrics;
    let cancelled = false;
    let disposed;
    let presenting = false;
    const fileOperations = [];
    const live = () => !cancelled && owned() && !signal.aborted;
    const gpu = createGPU({
      canvas, mailbox, moduleURL: new URL('./menu_gpu.mjs', import.meta.url),
      onAdapter: (details) => { if (live()) { onAdapter(details); } },
      onPresented: (details) => {
        if (!live()) { return; }
        let sample;
        try { sample = metrics ? readMissionMetrics(metrics) : undefined; }
        catch (error) { report({ generation, ok: false, code: 'METRICS_PROTOCOL', message: error.message }); return; }
        onPresented({ ...details, ...(sample ? { metrics: sample } : {}) });
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
      motion?.dispose();
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
          if (data.phase === 'io' && live()) { onDiagnostic({ io: data.stats }); return true; }
          if (data.phase === 'file-operation') {
            if (live()) {
              fileOperations.push(data.fileOperation);
              if (fileOperations.length > 32) { fileOperations.shift(); }
              onDiagnostic({ guest: { fileOperations: [...fileOperations] } });
            }
            return true;
          }
          if (data.phase === 'runtime' && live()) {
            if (!Number.isInteger(data.restarts) || data.restarts < 0 || data.restarts > 0xffffffff) {
              report({ generation, ok: false, code: 'RESTART_PROTOCOL', message: 'Invalid story runtime restart count.' });
            } else { onDiagnostic({ runtime: { restarts: data.restarts } }); }
            return true;
          }
          if (data.phase === 'restarting' && live()) {
            motion?.dispose();
            motion = undefined;
            metrics = undefined;
            producer?.dispose();
            producer = undefined;
            return true;
          }
          if ((data.graphics || data.fileOpens || data.wasmDiagnostics) && live()) { onDiagnostic({ guest: data }); }
          if (data.phase !== 'input') { return false; }
          if (!live()) { return true; }
          try {
            if (producer || data.words !== 2) { throw new Error('Unexpected menu input storage publication.'); }
            producer = createInput({ buffer: data.buffer, byteOffset: data.byteOffset, signal });
            if (data.metricsOffset) { metrics = new Uint32Array(data.buffer, data.metricsOffset, MISSION_METRICS_WORDS); }
            if (data.motionOffset) {
              motion = createMotion({ buffer: data.buffer, byteOffset: data.motionOffset, signal,
                clear: () => { producer?.setActive(false); producer?.setActive(true); } });
            }
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
