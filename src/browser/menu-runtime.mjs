import { createBootRuntime } from './boot-runtime.mjs';
import { createMenuInput } from './menu-input.mjs';
import { createGamepadInput } from './gamepad-input.mjs';
import { createMissionControls } from './mission-controls.mjs';
import { createBrowserAudio } from './browser-audio.mjs';
import { MISSION_METRICS_WORDS, readMissionMetrics } from './mission-metrics.mjs';
import { createMailbox } from './webgpu-transport.mjs';
import { startGPUWorker } from './webgpu-service.mjs';
import { createHostSettingsWriter, encodeHostSettings, readHookStatus } from './host-settings.mjs';
import { createKeyboardInput } from './keyboard-input.mjs';
import { applyHostMessage, createHostMessages } from './host-messages.mjs';

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
  gamepad = true, virtualPads, createGamepad = createGamepadInput, createInput = createMenuInput, createKeyboard = createKeyboardInput, createMotion = createMissionControls, createAudio = createBrowserAudio, onPresented = () => {}, onAdapter = () => {}, onDiagnostic = () => {},
  onKeyboard = () => {}, onHostMessage, onHostMessages = () => {}, hostComputed = () => ({}), onHostSettings = () => {},
  readyTimeoutMs = 20000, settings, ...runtimeOptions } = {}) => {
  if (typeof acquireCanvas !== 'function') { throw new TypeError('A fresh owned canvas is required for each menu runtime.'); }
  if (!Number.isInteger(readyTimeoutMs) || readyTimeoutMs < 1 || readyTimeoutMs > 30000) { throw new RangeError('Invalid GPU initialization deadline.'); }
  return createRuntime({ ...runtimeOptions, ...(settings ? { hostSettings: encodeHostSettings(settings.snapshot()) } : {}),
    observeIo: true, createServices: async ({ generation, signal, owned, report }) => {
    signal.throwIfAborted();
    const canvas = await acquireCanvas();
    signal.throwIfAborted();
    if (!owned()) { throw new DOMException('Stopped.', 'AbortError'); }
    const mailbox = createMailbox();
    let producer;
    let controller;
    let motion;
    let audio;
    let audioClosing;
    let metrics;
    let hostSettings;
    let hookStatus;
    let messages;
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
        for (const message of messages?.take() ?? []) {
          if (onHostMessage) { onHostMessage(message); } else if (settings) { applyHostMessage(settings, message); }
        }
        onPresented({ ...details, ...(sample ? { metrics: sample } : {}), ...(hookStatus ? { hookStatus: readHookStatus(hookStatus) } : {}) });
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
      audioClosing = audio?.dispose();
      hostSettings?.dispose();
      motion?.dispose();
      controller?.dispose();
      producer?.dispose();
      gpu.cancel();
    };
    const dispose = () => {
      disposed ??= (async () => { cancel(); await Promise.all([gpu.dispose(), audioClosing]); })();
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
          if (data.phase === 'hooks') {
            if (live()) { onDiagnostic({ hooks: data.hooks }); }
            return true;
          }
          if (data.phase === 'log') {
            if (live()) { console.info(data.text); }
            return true;
          }
          if (data.phase === 'restarting' && live()) {
            hostSettings?.dispose();
            hostSettings = undefined;
            hookStatus = undefined;
            messages = undefined;
            void audio?.dispose();
            audio = undefined;
            motion?.dispose();
            motion = undefined;
            metrics = undefined;
            controller?.dispose();
            controller = undefined;
            producer?.dispose();
            producer = undefined;
            return true;
          }
          if ((data.graphics || data.fileOpens || data.wasmDiagnostics) && live()) { onDiagnostic({ guest: data }); }
          if (data.phase !== 'input') { return false; }
          if (!live()) { return true; }
          try {
            if (producer || data.words !== 2) { throw new Error('Unexpected menu input storage publication.'); }
            if (gamepad && !data.gamepadOffset) { throw new Error('Missing shared gamepad input storage.'); }
            if (gamepad) { controller = createGamepad({ buffer: data.buffer, byteOffset: data.gamepadOffset, signal, ...(virtualPads ? { virtualPads } : {}) }); }
            producer = data.keyboardOffset ? createKeyboard({ buffer: data.buffer, byteOffset: data.keyboardOffset, signal }) :
              createInput({ buffer: data.buffer, byteOffset: data.byteOffset, signal });
            if (data.keyboardOffset) { onKeyboard(producer); }
            if (data.messagesOffset) {
              messages = createHostMessages({ buffer: data.buffer, byteOffset: data.messagesOffset });
              onHostMessages(messages);
            }
            if (data.audioOffset) {
              audio = createAudio({ buffer: data.buffer, byteOffset: data.audioOffset, samplesOffset: data.audioSamplesOffset, signal });
            }
            if (data.metricsOffset) { metrics = new Uint32Array(data.buffer, data.metricsOffset, MISSION_METRICS_WORDS); }
            if (data.hookStatusOffset) { hookStatus = new Uint32Array(data.buffer, data.hookStatusOffset, 8); }
            if (settings && data.settingsOffset) {
              hostSettings = createHostSettingsWriter({ buffer: data.buffer, byteOffset: data.settingsOffset, settings, computed: hostComputed,
                onWrite: revision => { if (live()) { onDiagnostic({ settings: { revision } }); } } });
              onHostSettings(hostSettings);
            }
            if (data.motionOffset) {
              motion = createMotion({ buffer: data.buffer, byteOffset: data.motionOffset, signal,
                pointerFree: () => producer?.pointerFree?.() ?? false,
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
