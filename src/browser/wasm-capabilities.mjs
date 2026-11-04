const tailCallProbe = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0,
  1, 4, 1, 96, 0, 0,
  3, 2, 1, 0,
  10, 6, 1, 4, 0, 18, 0, 11,
]);

const simdProbe = new Uint8Array([
  0, 97, 115, 109, 1, 0, 0, 0,
  1, 4, 1, 96, 0, 0,
  3, 2, 1, 0,
  10, 23, 1, 21, 0, 253, 12,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  26, 11,
]);

export const assertWasmCapabilities = (manifest, validate = (bytes) => WebAssembly.validate(bytes)) => {
  if (manifest.wasmRequirements?.includes('tail-call') && !validate(tailCallProbe)) {
    throw Object.assign(new Error('This build requires WebAssembly tail-call support.'), { code: 'CAPABILITY' });
  }
  if (manifest.wasmRequirements?.includes('simd128') && !validate(simdProbe)) {
    throw Object.assign(new Error('This build requires WebAssembly SIMD support.'), { code: 'CAPABILITY' });
  }
};
