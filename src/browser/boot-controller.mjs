const outcome = (code, message, phase = 'error', diagnostic) => ({ code, message, phase, diagnostic });
const guestDiagnostic = (message) => {
  if (message.code === 'CANCELLED' || typeof message.entryEntered !== 'boolean') { return undefined; }
  return Object.freeze({
    entryEntered: message.entryEntered, entryReturned: message.entryReturned === true,
    threadId: Number.isInteger(message.threadId) && message.threadId > 0 && message.threadId <= 0xffffffff ? message.threadId : 0,
    ordinal: Number.isInteger(message.ordinal) && message.ordinal > 0 && message.ordinal <= 0xffffffff ? message.ordinal : 0,
    exportName: typeof message.exportName === 'string' && message.exportName.length <= 128 ? message.exportName : undefined,
  });
};
const cancelled = () => outcome('CANCELLED', 'Boot stopped.', 'stopped');
const failure = (error) => outcome(error?.code ?? 'BOOT', error?.message ?? 'Boot failed.');

export const createBootController = ({
  createRuntime,
  validateSelection = async (source) => {
    if (!source || !Number.isSafeInteger(source.size) || source.size <= 0 || typeof source.slice !== 'function') {
      throw Object.assign(new Error('Select a readable local ISO.'), { code: 'FILE' });
    }
  },
  watchdogMs = 30000,
  idleWatchdogMs = 10000,
  now = () => performance.now(),
  setTimer = setTimeout,
  clearTimer = clearTimeout,
  onChange = () => {},
} = {}) => {
  if (!Number.isFinite(watchdogMs) || watchdogMs <= 0 || watchdogMs > 30000 ||
      !Number.isFinite(idleWatchdogMs) || idleWatchdogMs <= 0 || idleWatchdogMs > 30000) {
    throw new Error('Positive boot watchdogs bounded to 30 seconds are required.');
  }
  let state = Object.freeze({ phase: 'selection', source: undefined, stopping: false });
  let source;
  let active;
  let generation = 0;
  let intent = 0;
  let cleanup = Promise.resolve();
  let poisoned;
  const publish = (change) => {
    state = Object.freeze({ ...state, ...change, source });
    onChange(state);
  };
  const retire = (result) => {
    const session = active;
    if (!session || session.retired) { return cleanup; }
    session.retired = true;
    Atomics.store(session.ownership, 0, 0);
    session.abort.abort();
    clearTimer(session.timer);
    clearTimer(session.observer);
    publish({ stopping: true });
    const tasks = [];
    for (const method of ['cancelIo', 'terminateAll']) {
      if (session.runtime) {
        try { tasks.push(Promise.resolve(session.runtime[method]())); }
        catch (error) { tasks.push(Promise.reject(error)); }
      }
    }
    cleanup = Promise.allSettled(tasks).then((results) => {
      const rejected = results.find((entry) => entry.status === 'rejected');
      if (rejected) {
        poisoned = outcome('TEARDOWN', 'Complete worker and I/O teardown could not be verified. Reload before booting again.');
      }
      const final = poisoned ?? result;
      active = undefined;
      publish({ phase: final.phase, stopping: false, code: final.code, message: final.message, diagnostic: final.diagnostic });
      session.finish(final);
    });
    return cleanup;
  };
  const stop = () => {
    ++intent;
    return retire(cancelled());
  };
  const select = async (selected) => {
    const request = ++intent;
    await retire(cancelled());
    if (request !== intent) { return cancelled(); }
    if (poisoned) { return poisoned; }
    source = selected;
    publish({ phase: selected ? 'selected' : 'selection', stopping: false, code: undefined, message: undefined, diagnostic: undefined });
    return state;
  };
  const start = async () => {
    const request = ++intent;
    await retire(cancelled());
    if (request !== intent) { return cancelled(); }
    if (poisoned) { return poisoned; }
    if (!source) {
      const result = outcome('FILE', 'Select a local ISO before booting.');
      publish(result);
      return result;
    }
    if (generation === 0x7fffffff) {
      const result = outcome('GENERATION', 'Runtime generation budget exhausted. Reload before booting again.');
      publish(result);
      return result;
    }
    if (typeof SharedArrayBuffer !== 'function') {
      const result = outcome('CAPABILITY', 'Cross origin isolation and shared memory are required.');
      publish(result);
      return result;
    }
    const ownership = new Int32Array(new SharedArrayBuffer(4));
    const abort = new AbortController();
    let finish;
    const done = new Promise((resolve) => { finish = resolve; });
    const session = { generation: ++generation, ownership, abort, finish, retired: false,
      lastProgress: undefined, execution: 0, ioBytes: 0 };
    Atomics.store(ownership, 0, session.generation);
    active = session;
    publish({ phase: 'validating', stopping: false, code: undefined, message: undefined, diagnostic: undefined, generation: session.generation });
    session.timer = setTimer(() => {
      if (active === session && !session.retired) {
        void retire(outcome('WATCHDOG', 'Independent boot watchdog expired.'));
      }
    }, watchdogMs);
    const observe = () => {
      if (active !== session || session.retired || state.phase === 'running') { return; }
      const sample = session.runtime?.sampleProgress();
      const time = now();
      if (sample && Number.isInteger(sample.execution) && sample.execution >= 0 && sample.execution <= 0xffffffff &&
          Number.isSafeInteger(sample.ioBytes) && sample.ioBytes >= session.ioBytes &&
          (session.lastProgress === undefined || sample.execution !== session.execution || sample.ioBytes > session.ioBytes)) {
        session.lastProgress = time;
        session.execution = sample.execution;
        session.ioBytes = sample.ioBytes;
      }
      if (session.lastProgress !== undefined && time - session.lastProgress >= idleWatchdogMs) {
        void retire(outcome('NO_PROGRESS', 'No runtime execution or ISO completion progress observed.'));
        return;
      }
      session.observer = setTimer(observe, 250);
    };
    session.observer = setTimer(observe, 250);
    const report = (message) => {
      if (active !== session || session.retired || message?.generation !== session.generation) { return; }
      if (message.ok === false) {
        void retire(outcome(message.code ?? 'BOOT', message.message ?? 'Guest boot failed.', ['CANCELLED', 'GRAPHICS_CHECKPOINT', 'SINGLE_PLAYER_LAUNCH_REQUEST'].includes(message.code) ? 'stopped' : 'error', guestDiagnostic(message)));
      } else if (message.phase === 'booting' && state.phase === 'validating') {
        publish({ phase: 'booting' });
      } else if (message.phase === 'running' && state.phase === 'booting') {
        clearTimer(session.timer);
        clearTimer(session.observer);
        publish({ phase: 'running' });
      } else if (message.ok === true) {
        void retire(outcome('BOOT_PROTOCOL', 'An entry result is not proof of a running game.'));
      }
    };
    const prepare = async () => {
      try {
        await validateSelection(source, { signal: abort.signal });
        if (session.retired) { return; }
        if (typeof createRuntime !== 'function') {
          throw Object.assign(new Error('Complete Emscripten worker tree teardown adapter is unavailable.'), { code: 'LIFECYCLE_UNAVAILABLE' });
        }
        session.runtime = createRuntime();
        if (!session.runtime || ['start', 'cancelIo', 'terminateAll', 'sampleProgress'].some((method) => typeof session.runtime[method] !== 'function')) {
          poisoned = outcome('TEARDOWN', 'Runtime adapter does not provide complete worker and I/O teardown. Reload before booting again.');
          throw Object.assign(new Error(poisoned.message), { code: poisoned.code });
        }
        await session.runtime.start({ source, generation: session.generation, ownership, signal: abort.signal, report });
      } catch (error) {
        if (!session.retired) { void retire(failure(error)); }
      }
    };
    void prepare();
    return done;
  };
  return Object.freeze({ get state() { return state; }, select, start, stop });
};
