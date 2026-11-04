import { AudioQueue, AUDIO_RATE } from './audio-transport.mjs';

export const createBrowserAudio = ({ buffer, byteOffset, samplesOffset, signal,
  document = globalThis.document, window = globalThis.window,
  button = document?.querySelector('#enable-audio'), status = document?.querySelector('#audio-status'),
  AudioContext = globalThis.AudioContext,
  createNode = (context, options) => new AudioWorkletNode(context, 'mission-audio', options),
  now = () => performance.now(), schedule = setInterval, cancel = clearInterval,
} = {}) => {
  const queue = new AudioQueue({ buffer, byteOffset, samplesOffset });
  if (!button || !status || !document?.addEventListener || !window?.addEventListener) { throw new Error('Audio controls are required.'); }
  signal?.throwIfAborted();
  const header = queue.header;
  let context, node, pending, timer;
  let disposed = false;
  let focused = document.hasFocus?.() ?? true;
  const flush = () => {
    Atomics.store(header, 3, 0);
    Atomics.store(header, 1, Atomics.load(header, 0));
  };
  const snapshot = () => ({ mode: 'web-audio', sampleRate: AUDIO_RATE, channels: 2,
    queuedFrames: (Atomics.load(header, 0) - Atomics.load(header, 1)) >>> 0,
    droppedFrames: Atomics.load(header, 4), underrunFrames: Atomics.load(header, 5),
    playedFrames: Atomics.load(header, 6), fault: Atomics.load(header, 7), state: context?.state ?? 'disabled' });
  const sync = () => {
    flush();
    if (!disposed && focused && !document.hidden && context?.state === 'running' && node) {
      Atomics.store(header, 3, 1);
      status.textContent = 'Audio enabled.';
    } else if (!disposed) { status.textContent = 'Audio paused. Select Enable audio to resume if needed.'; }
  };
  const fail = reason => {
    flush();
    node?.disconnect();
    node?.removeEventListener('processorerror', fail);
    node = undefined;
    cancel(timer);
    if (!disposed) { button.disabled = false; status.textContent = typeof reason === 'string' ? reason : 'Audio unavailable. Select Enable audio to retry.'; }
  };
  const enable = () => {
    if (disposed || pending) { return pending; }
    pending = (async () => {
      button.disabled = true;
      try {
        if (typeof AudioContext !== 'function') { throw new Error('Web Audio is unavailable.'); }
        if (!context || context.state === 'closed') {
          context = new AudioContext({ sampleRate: AUDIO_RATE, latencyHint: 'interactive' });
          context.addEventListener('statechange', sync);
        }
        await context.resume();
        if (disposed) { return; }
        if (context.sampleRate !== AUDIO_RATE || context.state !== 'running') { throw new Error('Audio activation was denied.'); }
        if (!node) {
          await context.audioWorklet.addModule(new URL('./audio-worklet.mjs', import.meta.url));
          if (disposed) { return; }
          node = createNode(context, { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
            processorOptions: { buffer, byteOffset, samplesOffset } });
          node.addEventListener('processorerror', fail);
          node.connect(context.destination);
        }
        focused = document.hasFocus?.() ?? focused;
        sync();
        cancel(timer);
        let lastClock = context.currentTime, lastAdvance = now();
        timer = schedule(() => {
          if (disposed || !Atomics.load(header, 3)) { return; }
          if (context.currentTime > lastClock) { lastClock = context.currentTime; lastAdvance = now(); }
          else if (now() - lastAdvance > 2000) {
            fail('The browser audio clock stopped. Check the output device, then select Enable audio to retry.');
            return;
          }
          const sample = snapshot();
          status.textContent = `Audio enabled. ${(sample.queuedFrames / 48).toFixed(0)} ms queued; ${sample.underrunFrames} underrun frames.`;
          if (sample.fault) { fail(); }
        }, 1000);
        button.disabled = false;
      } catch { if (!disposed) { fail(); } }
      finally { pending = undefined; }
    })();
    return pending;
  };
  const click = () => { void enable(); };
  const blur = () => { focused = false; sync(); };
  const focus = () => { focused = true; sync(); };
  let closing;
  const dispose = () => {
    if (disposed) { return closing; }
    disposed = true;
    flush();
    cancel(timer);
    button.disabled = true;
    button.removeEventListener('click', click);
    document.removeEventListener('visibilitychange', sync);
    window.removeEventListener('blur', blur);
    window.removeEventListener('focus', focus);
    signal?.removeEventListener('abort', dispose);
    node?.removeEventListener('processorerror', fail);
    node?.disconnect();
    context?.removeEventListener('statechange', sync);
    closing = context?.close().catch(() => {});
    return closing;
  };
  flush();
  button.disabled = false;
  status.textContent = 'Select Enable audio to hear speech, music and effects.';
  button.addEventListener('click', click);
  document.addEventListener('visibilitychange', sync);
  window.addEventListener('blur', blur);
  window.addEventListener('focus', focus);
  signal?.addEventListener('abort', dispose, { once: true });
  return Object.freeze({ enable, dispose, snapshot });
};
