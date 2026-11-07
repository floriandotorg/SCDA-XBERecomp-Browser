import manifest from './boot-manifest.mjs';
import { createSaveStore } from './save-store.mjs';

// Saved progress in the origin private file system, under scda/<namespace>/<XBE SHA-256>: the game executable the
// selected ISO was checked against, not the ISO's name. Writes go through a sync access handle and are flushed before
// the store reads them back.
const opfsDirectory = handle => {
  const access = async (name, create, use) => {
    const file = await handle.getFileHandle(name, { create });
    const sync = await file.createSyncAccessHandle();
    try { return use(sync); } finally { sync.close(); }
  };
  return {
    read: async name => {
      try {
        return await access(name, false, sync => {
          const bytes = new Uint8Array(sync.getSize());
          for (let offset = 0; offset < bytes.length;) {
            const read = sync.read(bytes.subarray(offset), { at: offset });
            if (!read) { return bytes.subarray(0, offset); }
            offset += read;
          }
          return bytes;
        });
      } catch (error) {
        if (error?.name === 'NotFoundError') { return undefined; }
        throw error;
      }
    },
    write: (name, bytes) => access(name, true, sync => {
      sync.truncate(0);
      for (let offset = 0; offset < bytes.length;) { offset += sync.write(bytes.subarray(offset), { at: offset }); }
      sync.flush();
    }),
    remove: async name => {
      try { await handle.removeEntry(name); } catch (error) { if (error?.name !== 'NotFoundError') { throw error; } }
    },
    list: async () => {
      const names = [];
      for await (const name of handle.keys()) { names.push(name); }
      return names;
    },
  };
};

const exclusive = name => new Promise(resolve => {
  if (!navigator.locks) { resolve(true); return; }
  navigator.locks.request(name, { ifAvailable: true }, lock => {
    resolve(Boolean(lock));
    return lock ? new Promise(() => {}) : undefined;
  }).catch(() => resolve(false));
});

let store;
const open = async namespace => {
  if (!/^[a-z0-9-]{1,40}$/.test(namespace)) { throw Object.assign(new Error('Invalid progress namespace.'), { code: 'NAMESPACE' }); }
  if (!navigator.storage?.getDirectory) { return { state: 'unavailable', message: 'This browser has no private file storage, so progress is not saved.' }; }
  if (!await exclusive(`scda-progress:${namespace}:${manifest.xbeSha256}`)) {
    return { state: 'locked', message: 'The game is open in another tab. This tab starts without saved progress and does not save.' };
  }
  let directory = await navigator.storage.getDirectory();
  for (const name of ['scda', namespace, manifest.xbeSha256]) { directory = await directory.getDirectoryHandle(name, { create: true }); }
  store = createSaveStore({ directory: opfsDirectory(directory), game: manifest.xbeSha256 });
  return store.load();
};
const usage = async () => {
  const [estimate, persisted] = await Promise.all([navigator.storage?.estimate?.().catch(() => undefined), navigator.storage?.persisted?.().catch(() => undefined)]);
  return { ...store?.status(), usage: estimate?.usage, quota: estimate?.quota, persisted };
};
const operations = {
  open: ({ namespace }) => open(namespace),
  commit: ({ image }) => store.commit(image),
  export: async ({ settings }) => store.exportPackage({ settings }),
  inspect: ({ bytes }) => store.inspectPackage(bytes),
  import: ({ bytes }) => store.importPackage(bytes),
  restore: () => store.restoreBackup(),
  status: usage,
};

self.onmessage = async ({ data: { id, type, ...input } }) => {
  try {
    if (!operations[type]) { throw Object.assign(new Error('Unknown progress operation.'), { code: 'PROTOCOL' }); }
    if (type !== 'open' && type !== 'status' && !store) { throw Object.assign(new Error('Saved progress is not available in this tab.'), { code: 'UNAVAILABLE' }); }
    const result = await operations[type](input);
    self.postMessage({ id, ok: true, result }, result instanceof Uint8Array ? [result.buffer] : []);
  } catch (error) {
    self.postMessage({ id, ok: false, error: { code: error?.code ?? 'IO', message: error?.message ?? String(error) } });
  }
};
