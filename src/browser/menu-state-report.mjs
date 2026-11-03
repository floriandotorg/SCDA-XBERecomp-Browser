export const readMenuFileDiagnostics = (module) => {
  if (typeof module._image_file_open_count !== 'function') { return undefined; }
  const reject = () => {
    throw Object.assign(new Error('Menu file diagnostics are malformed or exceed their budget.'), { code: 'GRAPHICS_PROTOCOL' });
  };
  const word = (value) => {
    if (!Number.isInteger(value) || value < -0x80000000 || value > 0xffffffff) { reject(); }
    return value >>> 0;
  };
  const count = word(module._image_file_open_count());
  if (count > 32) { reject(); }
  const path = (index, field) => {
    const address = word(module._image_file_open_path(index, field));
    const heap = module.HEAPU8;
    if (!address || !(heap instanceof Uint8Array) || address >= heap.byteLength) { reject(); }
    const limit = Math.min(address + 241, heap.byteLength);
    let end = address;
    while (end < limit && heap[end] !== 0) { ++end; }
    if (end === limit) { reject(); }
    return new TextDecoder().decode(heap.slice(address, end));
  };
  return Array.from({ length: count }, (_, n) => ({
    status: word(module._image_file_open_word(n, 0)), root: word(module._image_file_open_word(n, 1)),
    disposition: word(module._image_file_open_word(n, 2)), options: word(module._image_file_open_word(n, 3)),
    access: word(module._image_file_open_word(n, 4)), attributes: word(module._image_file_open_word(n, 5)),
    allocationBytes: (BigInt(word(module._image_file_open_word(n, 6))) | (BigInt(word(module._image_file_open_word(n, 7))) << 32n)).toString(),
    originalPath: path(n, 0), resolvedPath: path(n, 1),
  }));
};

export const readMenuStateDiagnostic = (module) => {
  if (typeof module._image_graphics_capture_word !== 'function') { return undefined; }
  const reject = () => {
    throw Object.assign(new Error('Menu state capture is malformed or exceeds its budget.'), { code: 'GRAPHICS_PROTOCOL' });
  };
  const word = (group, index) => {
    const value = module._image_graphics_capture_word(group, index);
    if (!Number.isInteger(value) || value < -0x80000000 || value > 0xffffffff) { reject(); }
    return value >>> 0;
  };
  const read = (group, limit, exact, alignment = 1) => {
    const count = word(group, 0xffffffff);
    if (count > limit || (exact && count !== limit) || count % alignment !== 0) { reject(); }
    return Array.from({ length: count }, (_, n) => word(group, n));
  };
  const handles = read(4, 2, true);
  const records = (group, count, width) => {
    const words = read(group, count * width, true);
    return Array.from({ length: count }, (_, n) => words.slice(n * width, (n + 1) * width));
  };
  const report = {
    renderState: read(1, 166, true),
    simpleState: read(2, 2048, true),
    textureState: read(3, 128, true),
    vertex: {
      handle: handles[0], declaration: read(5, 1024, false), program: read(6, 544, false, 4),
      constants: read(8, 768, true),
    },
    pixel: { handle: handles[1], definition: read(7, 60, false), constants: read(9, 64, true) },
    presentation: read(10, 10, true),
    textures: records(11, 4, 6), streams: records(12, 16, 2), index: read(13, 3, true), targets: records(14, 2, 4),
  };
  if (report.pixel.definition.length !== 0 && report.pixel.definition.length !== 60) { reject(); }
  return report;
};
