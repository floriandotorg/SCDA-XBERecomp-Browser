import { validateDiscImage } from './iso-validation.mjs';

export const prepareDiscImage = async ({ disc, manifest, module, layout, signal }) => {
  const { base, end, entry } = layout ?? {};
  if (![base, end, entry].every((value) => Number.isSafeInteger(value) && value >= 0x10000 && value <= 0xffffffff) ||
      base % 4096 !== 0 || end <= base || entry < base || entry >= end) {
    throw new Error('Invalid generated image layout.');
  }
  const verified = await validateDiscImage({ disc, manifest, signal });
  signal?.throwIfAborted();
  try {
    const destination = module._image_allocate(verified.bytes.length);
    if (!destination) { throw new Error('Original image allocation failed.'); }
    module.HEAPU8.set(verified.bytes, destination);
    if (module._image_prepare(base, end, entry) !== 1) {
      throw new Error('Original image parsing or preparation failed.');
    }
    return {
      sections: module._image_sections(),
      imports: module._image_imports(),
      tlsBytes: module._image_tls_bytes(),
      backingBytes: module._image_backing(),
    };
  } catch (error) {
    module._image_reset();
    throw error;
  }
};
