import { decodeSaveContainer, encodeSaveContainer, parseStorageImage } from './save-format.mjs';

// Accepted progress lives in two slots written in turn: a commit replaces only the older slot and is accepted once it
// reads back intact, so a failed or torn write leaves the newer slot as it was.
const SLOTS = ['slot-a.scdasave', 'slot-b.scdasave'];
const BACKUP = 'backup.scdasave';
const MAXIMUM_DAMAGED = 8;

const fail = (code, message) => { throw Object.assign(new Error(message), { code }); };
const classify = error => {
  if (error?.code) { return error; }
  const name = error?.name;
  if (name === 'QuotaExceededError') { return Object.assign(new Error('Browser storage is full.'), { code: 'QUOTA', cause: error }); }
  if (name === 'NoModificationAllowedError' || name === 'InvalidStateError') {
    return Object.assign(new Error('Saved progress is open in another tab.'), { code: 'LOCKED', cause: error });
  }
  return Object.assign(new Error(`Browser storage failed: ${error?.message ?? error}`), { code: 'IO', cause: error });
};
const sameBytes = (left, right) => left?.length === right.length && left.every((value, n) => value === right[n]);

export const createSaveStore = ({ directory, game, now = () => new Date() }) => {
  let state = 'unloaded';
  let current;
  let backup = false;
  let damaged = 0;
  let queue = Promise.resolve();
  const serial = action => {
    const result = queue.then(action);
    queue = result.catch(() => {});
    return result;
  };
  const storage = async action => {
    try { return await action(); } catch (error) { throw classify(error); }
  };
  const decode = async bytes => {
    const container = await decodeSaveContainer(bytes, { game });
    return { ...container, listing: parseStorageImage(container.payload) };
  };
  const readSlot = async name => {
    const bytes = await storage(() => directory.read(name));
    if (!bytes) { return { name }; }
    try { return { name, bytes, ...(await decode(bytes)) }; } catch (error) { return { name, bytes, error }; }
  };
  const writeVerified = async (name, bytes) => {
    await storage(() => directory.write(name, bytes));
    if (!sameBytes(await storage(() => directory.read(name)), bytes)) {
      fail('WRITE_VERIFY', 'Browser storage did not keep the saved progress.');
    }
  };
  const commit = async payload => {
    if (state === 'unloaded') { fail('UNLOADED', 'Saved progress has not been loaded.'); }
    if (state === 'incompatible') { fail('INCOMPATIBLE', 'Stored progress uses a newer format and is not replaced.'); }
    parseStorageImage(payload);
    const sequence = (current?.sequence ?? 0) + 1;
    const savedAt = now().toISOString();
    const name = current?.name === SLOTS[0] ? SLOTS[1] : SLOTS[0];
    const bytes = await encodeSaveContainer({ header: { game, kind: 'slot', sequence, savedAt }, payload });
    await writeVerified(name, bytes);
    current = { name, sequence, savedAt, payload, bytes };
    state = 'loaded';
    return { sequence, savedAt };
  };
  const status = () => ({ state, sequence: current?.sequence ?? 0, savedAt: current?.savedAt, backup, damaged });

  return Object.freeze({
    load: () => serial(async () => {
      const slots = await Promise.all(SLOTS.map(readSlot));
      const names = await storage(() => directory.list());
      backup = names.includes(BACKUP);
      damaged = names.filter(name => name.startsWith('damaged-')).length;
      current = undefined;
      if (slots.some(slot => slot.error?.code === 'VERSION')) {
        state = 'incompatible';
        return { ...status(), message: 'Stored progress was made by a newer version of this page. It is kept unchanged and nothing is saved.' };
      }
      const newest = slots.filter(slot => slot.header).sort((left, right) => right.header.sequence - left.header.sequence)[0];
      if (newest) {
        current = { name: newest.name, sequence: newest.header.sequence, savedAt: newest.header.savedAt, payload: newest.payload, bytes: newest.bytes };
        state = 'loaded';
        return { ...status(), payload: newest.payload };
      }
      const present = slots.filter(slot => slot.bytes);
      if (!present.length) {
        state = 'empty';
        return status();
      }
      const stamp = now().toISOString().replace(/[^0-9]/g, '');
      for (const slot of present) {
        if (damaged >= MAXIMUM_DAMAGED) { break; }
        await writeVerified(`damaged-${stamp}-${slot.name}`, slot.bytes);
        ++damaged;
      }
      state = 'damaged';
      return { ...status(), message: 'Stored progress was damaged. A copy was set aside and the game starts without it.' };
    }),
    commit: payload => serial(() => commit(payload)),
    exportPackage: ({ settings } = {}) => serial(async () => {
      if (!current) { fail('EMPTY', 'There is no saved progress to export.'); }
      return encodeSaveContainer({ header: { game, kind: 'export', sequence: current.sequence, savedAt: current.savedAt, settings }, payload: current.payload });
    }),
    inspectPackage: async bytes => {
      const { header, listing } = await decode(bytes);
      return { savedAt: header.savedAt, files: listing.files, bytes: listing.bytes, settings: header.settings };
    },
    importPackage: bytes => serial(async () => {
      if (state === 'incompatible') { fail('INCOMPATIBLE', 'Stored progress uses a newer format and is not replaced.'); }
      const { header, payload } = await decode(bytes);
      if (current) {
        await writeVerified(BACKUP, current.bytes);
        backup = true;
      }
      await commit(payload);
      return { settings: header.settings, payload };
    }),
    restoreBackup: () => serial(async () => {
      const bytes = await storage(() => directory.read(BACKUP));
      if (!bytes) { fail('NO_BACKUP', 'There is no earlier progress to restore.'); }
      const { payload } = await decode(bytes);
      await commit(payload);
      return { payload };
    }),
    status,
  });
};
