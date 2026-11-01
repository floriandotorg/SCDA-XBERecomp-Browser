import create from '../../iso-disc.mjs';
import { createWasmIsoReader } from '../../src/browser/iso-wasm.mjs';
import { prepareDiscImage } from '../../src/browser/iso-image.mjs';

self.onmessage = async ({ data: { port, mailbox, generation, ownership, size, manifest } }) => {
  let module;
  try {
    const read = createWasmIsoReader({ port, mailbox, generation, ownership, getHeap: () => module.HEAPU8 });
    module = await create({ readIsoBytes: read });
    if (module._mount_disc(size) !== 1) { throw new Error(`Disc mount failed: ${module.isoError ?? 'PARSER'}`); }
    const find = (path) => {
      const encoded = new TextEncoder().encode(path);
      if (encoded.length > 240) { throw new Error('Path budget exceeded.'); }
      module.HEAPU8.set(encoded, module._validation_path());
      const kind = module._validation_find(encoded.length);
      return kind ? { size: module._validation_size(), dir: kind === 2 } : undefined;
    };
    const disc = { find, read: (path, position, length) => {
      if (!find(path)) { throw new Error('Disc file is absent.'); }
      const completed = module._validation_read(position, length);
      const start = module._validation_buffer();
      return module.HEAPU8.slice(start, start + completed);
    } };
    const result = await prepareDiscImage({
      disc, manifest, module,
      layout: { base: manifest.ranges.X86ImageBase, end: manifest.ranges.X86ImageEnd, entry: manifest.ranges.X86EntryPoint },
    });
    self.postMessage({ ok: true, ...result, partition: module._disc_partition(), wasmBytes: module.HEAPU8.byteLength });
  } catch (error) {
    self.postMessage({ ok: false, message: error.message });
  } finally {
    module?._image_reset();
    port.close();
  }
};
