import { AudioQueue, AUDIO_RATE } from './audio-transport.mjs';

const gestures = ['pointerdown', 'keydown', 'touchend'];
const clampVolume = value => Number.isFinite(value) ? Math.max(0, Math.min(1, value)) : 1;

export const createBrowserAudio = ({ buffer, byteOffset, samplesOffset, signal, volume = 1, output = 'stereo',
  document = globalThis.document, window = globalThis.window,
  AudioContext = globalThis.AudioContext,
  createNode = (context, options) => new AudioWorkletNode(context, 'mission-audio', options),
  now = () => performance.now(), schedule = setInterval, cancel = clearInterval,
} = {}) => {
  const queue = new AudioQueue({ buffer, byteOffset, samplesOffset });
  if (!document?.addEventListener || !window?.addEventListener) { throw new Error('Audio requires document and window events.'); }
  signal?.throwIfAborted();
  const header = queue.header;
  let context, node, gain, pending, timer;
  let level = clampVolume(volume);
  let disposed = false;
  let focused = document.hasFocus?.() ?? true;
  const flush = () => {
    Atomics.store(header, 3, 0);
    Atomics.store(header, 1, Atomics.load(header, 0));
  };
  const snapshot = () => ({ mode: 'web-audio', sampleRate: AUDIO_RATE, channels: 2,
    queuedFrames: (Atomics.load(header, 0) - Atomics.load(header, 1)) >>> 0,
    droppedFrames: Atomics.load(header, 4), underrunFrames: Atomics.load(header, 5),
    playedFrames: Atomics.load(header, 6), fault: Atomics.load(header, 7), state: context?.state ?? 'disabled', volume: level });
  const playing = () => context?.state === 'running' && node !== undefined;
  const sync = () => {
    flush();
    if (!disposed && focused && !document.hidden && playing()) { Atomics.store(header, 3, 1); }
  };
  // a disconnected worklet node keeps running and reading the ring until its processor stops
  const fail = () => {
    flush();
    node?.port?.postMessage('stop');
    node?.disconnect();
    node?.removeEventListener('processorerror', fail);
    node = undefined;
    cancel(timer);
  };
  const enable = () => {
    if (disposed || pending) { return pending ?? Promise.resolve(); }
    pending = (async () => {
      try {
        if (typeof AudioContext !== 'function') { return; }
        if (!context || context.state === 'closed') {
          context = new AudioContext({ sampleRate: AUDIO_RATE, latencyHint: 'interactive' });
          context.addEventListener('statechange', sync);
        }
        await context.resume();
        if (disposed || context.sampleRate !== AUDIO_RATE || context.state !== 'running') { return; }
        if (!node) {
          await context.audioWorklet.addModule(new URL('./audio-worklet.mjs', import.meta.url));
          if (disposed) { return; }
          node = createNode(context, { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2],
            processorOptions: { buffer, byteOffset, samplesOffset } });
          node.addEventListener('processorerror', fail);
          gain ??= context.createGain();
          gain.gain.value = level;
          applyOutput();
          node.connect(gain);
          gain.connect(context.destination);
        }
        focused = document.hasFocus?.() ?? focused;
        sync();
        cancel(timer);
        let lastClock = context.currentTime, lastAdvance = now();
        timer = schedule(() => {
          if (disposed || !Atomics.load(header, 3)) { return; }
          if (context.currentTime > lastClock) { lastClock = context.currentTime; lastAdvance = now(); }
          else if (now() - lastAdvance > 2000) { fail(); return; }
          if (Atomics.load(header, 7)) {
            Atomics.store(header, 7, 0);
            sync();
          }
        }, 1000);
      } catch { if (!disposed) { fail(); } }
      finally { pending = undefined; }
    })();
    return pending;
  };
  const gesture = () => { if (!playing()) { void enable(); } };
  const blur = () => { focused = false; sync(); };
  const focus = () => { focused = true; sync(); };
  const setVolume = value => {
    level = clampVolume(value);
    if (gain) { gain.gain.value = level; }
  };
  // mono: the gain node takes one channel, which Web Audio's speakers down-mix makes (L + R) / 2 and the destination
  // plays on both sides, as native's mono output
  let mono = output === 'mono';
  const applyOutput = () => {
    if (!gain) { return; }
    gain.channelCount = mono ? 1 : 2;
    gain.channelCountMode = mono ? 'explicit' : 'max';
    gain.channelInterpretation = 'speakers';
  };
  const setOutput = value => {
    mono = value === 'mono';
    applyOutput();
  };
  let closing;
  const dispose = () => {
    if (disposed) { return closing; }
    disposed = true;
    flush();
    cancel(timer);
    for (const type of gestures) { window.removeEventListener(type, gesture, true); }
    document.removeEventListener('visibilitychange', sync);
    window.removeEventListener('blur', blur);
    window.removeEventListener('focus', focus);
    signal?.removeEventListener('abort', dispose);
    node?.removeEventListener('processorerror', fail);
    node?.port?.postMessage('stop');
    node?.disconnect();
    gain?.disconnect();
    context?.removeEventListener('statechange', sync);
    closing = context?.close().catch(() => {});
    return closing;
  };
  flush();
  for (const type of gestures) { window.addEventListener(type, gesture, true); }
  document.addEventListener('visibilitychange', sync);
  window.addEventListener('blur', blur);
  window.addEventListener('focus', focus);
  signal?.addEventListener('abort', dispose, { once: true });
  void enable();
  return Object.freeze({ enable, dispose, snapshot, setVolume, setOutput });
};
