// The page's end of saved progress: save-store-worker.mjs owns the browser storage; the game runtime publishes storage
// images (phase "storage") and says when the guest has changed its files without a published image yet ("pending").
// Activity: idle, pending (changed, not yet published), saving (written, not yet read back), saved or failed.
const WRITABLE = new Set(['empty', 'loaded', 'damaged']);

export const createSaveService = ({ createWorker = () => new Worker(new URL('./save-store-worker.mjs', import.meta.url), { type: 'module' }),
  namespace = 'progress', storage = globalThis.navigator?.storage, onChange = () => {} } = {}) => {
  const worker = createWorker();
  const requests = new Map();
  let nextId = 0;
  const request = (type, input = {}, transfer = []) => new Promise((resolve, reject) => {
    const id = ++nextId;
    requests.set(id, { resolve, reject });
    try { worker.postMessage({ id, type, ...input }, transfer); }
    catch (error) { requests.delete(id); reject(error); }
  });
  let state = Object.freeze({ store: 'loading', activity: 'idle' });
  let guestPending = false, latest, writing, accepted, draining = false, persistenceRequested = false;
  const waiters = new Set();
  const busy = () => guestPending || Boolean(latest) || Boolean(writing);
  const publish = change => {
    state = Object.freeze({ ...state, ...change });
    onChange(state);
    if (!busy()) {
      for (const waiter of waiters) { waiter(true); }
      waiters.clear();
    }
  };
  const fault = error => Object.assign(new Error(error?.message ?? 'Saved progress failed.'), { code: error?.code ?? 'IO' });
  worker.onmessage = ({ data }) => {
    const entry = requests.get(data?.id);
    if (!entry) { return; }
    requests.delete(data.id);
    if (data.ok) { entry.resolve(data.result); } else { entry.reject(fault(data.error)); }
  };
  worker.onerror = event => {
    for (const entry of requests.values()) { entry.reject(fault({ code: 'WORKER', message: event?.message ?? 'The progress worker failed.' })); }
    requests.clear();
    publish({ store: 'unavailable', message: 'Saved progress stopped working. Reload the page before playing on.' });
  };
  const enabled = () => WRITABLE.has(state.store);
  const loaded = request('open', { namespace }).then(result => {
    accepted = result.payload;
    publish({ store: result.state, message: result.message, savedAt: result.savedAt, backup: result.backup });
    return result;
  }, error => {
    publish({ store: 'unavailable', message: error.message });
    return { state: 'unavailable', message: error.message };
  });
  const drain = async () => {
    draining = true;
    while (latest && enabled()) {
      writing = latest;
      latest = undefined;
      publish({ activity: 'saving' });
      try {
        const result = await request('commit', { image: writing });
        accepted = writing;
        writing = undefined;
        publish({ store: 'loaded', activity: latest ? 'saving' : guestPending ? 'pending' : 'saved', savedAt: result.savedAt, error: undefined });
        if (!persistenceRequested) {
          persistenceRequested = true;
          void storage?.persist?.().then(persisted => publish({ persisted }), () => {});
        }
      } catch (error) {
        writing = undefined;
        publish({ activity: 'failed', error: { code: error.code, message: error.message } });
      }
    }
    latest = undefined;
    draining = false;
  };
  const refresh = async () => {
    const status = await request('status');
    publish({ store: status.state, savedAt: status.savedAt, backup: status.backup, usage: status.usage, quota: status.quota, persisted: status.persisted });
    return status;
  };

  return Object.freeze({
    loaded,
    get state() { return state; },
    get enabled() { return enabled(); },
    get unsaved() { return enabled() && (busy() || state.activity === 'failed'); },
    get accepted() { return accepted; },
    pending: value => {
      guestPending = value === true && enabled();
      if (guestPending && !writing) { publish({ activity: 'pending' }); }
      else if (!guestPending && state.activity === 'pending') { publish({ activity: state.savedAt ? 'saved' : 'idle' }); }
    },
    stopped: () => {
      if (!guestPending) { return; }
      guestPending = false;
      publish({ activity: 'failed', error: { code: 'STOPPED', message: 'The game stopped before its last changes were saved.' } });
    },
    commit: image => {
      if (!enabled()) { return; }
      latest = image;
      if (!draining) { void drain(); }
    },
    settled: (timeout = Infinity) => {
      if (!enabled() || !busy()) { return Promise.resolve(true); }
      return new Promise(resolve => {
        waiters.add(resolve);
        if (Number.isFinite(timeout)) { setTimeout(() => { waiters.delete(resolve); resolve(false); }, timeout); }
      });
    },
    exportPackage: settings => request('export', { settings }),
    inspectPackage: bytes => request('inspect', { bytes }),
    importPackage: async bytes => {
      const result = await request('import', { bytes });
      accepted = result.payload;
      await refresh();
      return result;
    },
    restoreBackup: async () => {
      accepted = (await request('restore')).payload;
      await refresh();
    },
    refresh,
    dispose: () => { worker.terminate(); },
  });
};
