export const syntheticDisc = ({ large = false, imageBytes = new Uint8Array([10, 20, 30, 40, 50, 60, 70]) } = {}) => {
  if (imageBytes.length > 2048) { throw new Error('Invented executable exceeds its fixture extent.'); }
  const bytes = new Uint8Array(44 * 2048);
  const view = new DataView(bytes.buffer);
  const magic = new TextEncoder().encode('MICROSOFT*XBOX*MEDIA');
  bytes.set(magic, 32 * 2048);
  bytes.set(magic, 32 * 2048 + 0x7ec);
  view.setUint32(32 * 2048 + 0x14, 40, true);
  view.setUint32(32 * 2048 + 0x18, 60, true);
  view.setUint16(40 * 2048 + 2, 7, true);
  view.setUint32(40 * 2048 + 28 + 4, 42 + (large ? 2 ** 21 : 0), true);
  view.setUint32(40 * 2048 + 28 + 8, imageBytes.length, true);
  bytes[40 * 2048 + 28 + 13] = 9;
  bytes.set(new TextEncoder().encode('story.dat'), 40 * 2048 + 28 + 14);
  bytes.set(imageBytes, 42 * 2048);
  view.setUint32(40 * 2048 + 4, 41 + (large ? 2 ** 21 : 0), true);
  view.setUint32(40 * 2048 + 8, imageBytes.length, true);
  bytes[40 * 2048 + 13] = 11;
  bytes.set(new TextEncoder().encode('default.xbe'), 40 * 2048 + 14);
  bytes.set(imageBytes, 41 * 2048);
  const file = new File([bytes], 'invented.iso');
  if (!large) { return file; }
  return {
    size: 6 * 1024 ** 3,
    slice: (start, end) => start >= 2 ** 32 ? file.slice(start - 2 ** 32, end - 2 ** 32) : file.slice(start, end),
  };
};
