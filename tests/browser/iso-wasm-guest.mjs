import create from '../../iso-disc.mjs';
import { createWasmIsoReader } from '../../src/browser/iso-wasm.mjs';
import { validateDiscImage } from '../../src/browser/iso-validation.mjs';

export const runIsoWasmGuest = async ({ port, mailbox, generation, ownership, size = 44 * 2048 }) => {
  let module;
  const read = createWasmIsoReader({ port, mailbox, generation, ownership, getHeap: () => module.HEAPU8 });
  module = await create({ readIsoBytes: read });
  try {
    if (module._mount_disc(size) !== 1) {
      throw new Error(`Disc mount failed: ${module.isoError ?? 'PARSER'}`);
    }
    const invalidDestination = module._read_guest(0x10fff, 7, 0) === -1;
    const bytes = module._read_guest(0x10001, 7, 0);
    if (bytes !== 7 || module._check_guest() !== 1) {
      throw new Error('Guest disc bytes or adjacent sentinels differ.');
    }
    if (module._read_guest(0x10001, 7, 7) !== 0 || module._read_guest(0x10001, 7, 5) !== 2) {
      throw new Error('Guest disc EOF contract failed.');
    }
    const find = (path) => {
      const encoded = new TextEncoder().encode(path);
      if (encoded.length > 240) { throw new Error('Fixture path budget exceeded.'); }
      module.HEAPU8.set(encoded, module._validation_path());
      const kind = module._validation_find(encoded.length);
      return kind ? { size: module._validation_size(), dir: kind === 2 } : undefined;
    };
    const disc = { find, read: (path, position, length) => {
      if (!find(path)) { throw new Error('Fixture file is absent.'); }
      const completed = module._validation_read(position, length);
      const start = module._validation_buffer();
      return module.HEAPU8.slice(start, start + completed);
    } };
    const manifest = {
      schema: 1, image: 'default.xbe', xbeSize: 7,
      xbeSha256: '11f4cb6cc597bb0f8947f50fc0370362f74ba8ceaa7ecf32446373361ae9e3be',
      requiredStoryFiles: [{ path: 'STORY.DAT', minBytes: 3 }],
    };
    const verified = await validateDiscImage({ disc, manifest });
    const validated = verified.bytes.length === 7 && verified.storyFiles === 1;
    let rejectedMismatch = false;
    try {
      await validateDiscImage({ disc, manifest: { ...manifest, xbeSha256: '0'.repeat(64) } });
    } catch (error) {
      rejectedMismatch = error.code === 'IMAGE_HASH';
    }
    if (!validated || !rejectedMismatch) { throw new Error('Disc identity contract failed.'); }
    Atomics.store(ownership, 0, generation + 1);
    const stale = module._read_guest(0x10001, 7, 0) === 0 && module.isoError === 'STALE';
    return { ok: true, bytes, partition: module._disc_partition(), invalidDestination, stale, validated, rejectedMismatch };
  } finally {
    port.close();
  }
};
