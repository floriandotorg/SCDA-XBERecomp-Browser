import createGuest from '../../out/browser-webgpu-tests/webgpu-service-guest.mjs';
import { Opcode, Status } from '../../src/browser/webgpu-transport.mjs';
const encoder = new TextEncoder();
const join = (...parts) => {
  const result = new Uint8Array(parts.reduce((sum, bytes) => sum + bytes.byteLength, 0));
  let offset = 0;
  for (const bytes of parts) { result.set(new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength), offset); offset += bytes.byteLength; }
  return result;
};
const words = (...values) => new Uint32Array(values);
const floats = (...values) => new Float32Array(values);
const vertex = encoder.encode('struct Output { @builtin(position) position: vec4<f32>, @location(0) uv: vec2<f32> }; @vertex fn vs_main(@location(0) position: vec4<f32>, @location(1) uv: vec4<f32>) -> Output { var output: Output; output.position = position; output.uv = uv.xy; return output; }');
const fragment = encoder.encode('@group(0) @binding(1) var image: texture_2d<f32>; @group(0) @binding(2) var sampling: sampler; @fragment fn fs_main(@location(0) uv: vec2<f32>) -> @location(0) vec4<f32> { return textureSample(image, sampling, uv); }');
const texels = [[17, 83, 149, 255], [200, 11, 57, 128], [90, 160, 30, 255], [7, 19, 231, 255]];
globalThis.onmessage = async event => {
  try {
    const localTransport = event.data.localTransport === true;
    const module = await createGuest({ webgpuMailbox: event.data.mailbox, webgpuPthreads: localTransport });
    if (module._gpu_clock_probe() !== 1) { throw new Error("guest pthread clock is not aligned to its host worker"); }
    const evidence = [];
    const execute = (opcode, resource, bytes = new Uint8Array(), expected = Status.Ok) => {
      const input = join(bytes);
      const pointer = module._malloc(input.byteLength || 1);
      module.HEAPU8.set(input, pointer);
      const status = module._gpu_execute(opcode, resource, pointer, input.byteLength);
      module._free(pointer);
      const reply = module.HEAPU8.slice(module._gpu_reply(), module._gpu_reply() + module._gpu_reply_size());
      evidence.push({ command: module._gpu_command(), resource, opcode, status, pthreadMailboxUnbound: module._gpu_pthread_unbound() === 1 });
      if (module._gpu_pthread_unbound() !== (localTransport ? 0 : 1)) { throw new Error('fixture did not exercise the requested pthread transport'); }
      if (status !== expected) { throw new Error(`command ${module._gpu_command()} resource ${resource}: status ${status} ${new TextDecoder().decode(reply)}`); }
      return reply;
    };
    if (event.data.blockOnly) {
      execute(Opcode.Flush, 0, undefined, Status.Closed);
      globalThis.postMessage({ ok: true, shutdownResolvedBlockedGuest: true, evidence });
      return;
    }
    execute(Opcode.Configure, 0, words(4, 4));
    execute(Opcode.CreateTexture, 1, join(words(2, 2, 0, 1), new Uint8Array(texels.flat())));
    const pipeline = (compare = 8, write = 15, depthWrite = 1, stencil = [0, 0, 0, 0, 0, 0, 0], fragmentBytes = fragment.length, colorTarget = 1) =>
      words(vertex.length, fragmentBytes, 4, 1, 1, 0, 2, 1, 1, 2, 1, 1, write, 1, depthWrite, compare, ...stencil, 0, colorTarget, 0, 0);
    execute(Opcode.CreatePipeline, 3, join(pipeline(), vertex, fragment));
    execute(Opcode.CreatePipeline, 6, join(pipeline(4), vertex, fragment));
    execute(Opcode.CreatePipeline, 7, join(pipeline(8, 0, 0, [1, 8, 1, 1, 3, 255, 16]), vertex, fragment));
    execute(Opcode.CreatePipeline, 8, join(pipeline(8, 15, 0, [1, 3, 1, 1, 1, 16, 0]), vertex, fragment));
    const mipFragment = encoder.encode(new TextDecoder().decode(fragment).replace('textureSample(image, sampling, uv)', 'textureSampleLevel(image, sampling, uv, 1.0)'));
    execute(Opcode.CreatePipeline, 9, join(pipeline(8, 15, 0, undefined, mipFragment.length), vertex, mipFragment));
    const biasFragment = encoder.encode(new TextDecoder().decode(fragment).replace('textureSample(image, sampling, uv)', 'textureSampleBias(image, sampling, uv, 3.0)'));
    execute(Opcode.CreatePipeline, 11, join(pipeline(8, 15, 0, undefined, biasFragment.length), vertex, biasFragment));
    const depthOnlyFragment = encoder.encode('@fragment fn fs_main() {}');
    execute(Opcode.CreatePipeline, 15, join(pipeline(8, 0, 1, undefined, depthOnlyFragment.length, 0), vertex, depthOnlyFragment));
    for (let n = 0; n < 2; ++n) {
      const width = n === 0 ? 4 : 8;
      execute(Opcode.Configure, 0, words(width, 4));
      execute(Opcode.CreateTexture, 2, join(words(width, 4, 0, 1), new Uint8Array(width * 4 * 4)));
      execute(Opcode.CreateTexture, 4, words(width, 4, 1, 1));
      execute(Opcode.CreateTexture, 5, join(words(width, 4, 0, 1), new Uint8Array(width * 4 * 4)));
      execute(Opcode.Clear, 2, join(floats(0, 0, 0, 0, 1), words(0, 7, 4, 0)));
      const vertices = new Float32Array(64 * 3);
      vertices.set([-1, -1, 0, 1, 0, 1, 0, 0], 0); vertices.set([3, -1, 0, 1, 2, 1, 0, 0], 64); vertices.set([-1, 3, 0, 1, 0, -1, 0, 0], 128);
      const sampling = words(...Array(4).fill([1, 1, 1, 1, 1, 0, 32]).flat());
      const draw = (indices, pipeline = 3, reference = 0, image = 1) => join(words(pipeline, image, image, image, image, 4, 3, indices.length, indices.BYTES_PER_ELEMENT === 4 ? 1 : 0, vertices.byteLength, indices.byteLength, 16), floats(0, 0, width, 4, 0, 1), words(0, 0, width, 4), sampling, words(reference), vertices, indices, floats(1, 1, 1, 1));
      if (event.data.invalidBatch) {
        const invalid = draw(new Uint8Array());
        new Uint32Array(invalid.buffer, 0, 51)[20] = width + 1;
        execute(Opcode.DrawBatch, 0, join(words(1, 2, invalid.byteLength), invalid));
        execute(Opcode.Flush, 0, undefined, Status.Error);
        globalThis.postMessage({ ok: true, deferredValidation: true, evidence });
        return;
      }
      if (n === 0) { execute(Opcode.Draw, 2, draw(new Uint8Array())); }
      else { execute(Opcode.Draw, 2, draw(new Uint16Array([0, 1, 2]))); execute(Opcode.Draw, 2, draw(new Uint32Array([0, 1, 2]))); }
      execute(Opcode.Flush, 0);
      execute(Opcode.Copy, 5, words(2, width, 4, 0, 0, 0, 0, 0));
      const pixels = execute(Opcode.Readback, 5);
      if (pixels.length !== width * 4 * 4 || pixels.some((value, index) => {
        const pixel = Math.floor(index / 4);
        const x = pixel % width;
        const y = Math.floor(pixel / width);
        return value !== texels[(y < 2 ? 0 : 2) + (x < width / 2 ? 0 : 1)][index % 4];
      })) { throw new Error('textured draw orientation, alpha or channel mismatch'); }
      execute(Opcode.Clear, 5, join(floats(0, 0, 0, 0, 0), words(0, 1, 0, 0)));
      execute(Opcode.Copy, 5, words(2, 2, 2, 1, 1, width - 2, 0, 0));
      if (execute(Opcode.Readback, 5).some((value, n) => {
        const pixel = Math.floor(n / 4), x = pixel % width, y = Math.floor(pixel / width);
        return value !== (x >= width - 2 && y < 2 ? pixels[((y + 1) * width + x - width + 3) * 4 + n % 4] : 0);
      })) { throw new Error('partial copy changed its source coordinates or destination neighbors'); }
      execute(Opcode.Copy, 5, words(2, width, 4, 0, 0, 0, 0, 0));
      execute(Opcode.Clear, 5, join(floats(0, 0, 0, 0, 0), words(0, 1, 0, 0)));
      execute(Opcode.Copy, 5, words(2, 2, 2, 1, 1, width - 2, 0, 1));
      if (execute(Opcode.Readback, 5).some((value, n) => {
        const pixel = Math.floor(n / 4), x = pixel % width, y = Math.floor(pixel / width);
        return value !== (x >= width - 2 && y < 2 ? n % 4 === 3 ? 255 : pixels[((y + 1) * width + x - width + 3) * 4 + n % 4] : 0);
      })) { throw new Error('X8 region copy changed RGB, failed opaque alpha or touched neighbors'); }
      execute(Opcode.Copy, 5, words(2, width, 4, 0, 0, 0, 0, 0));
      execute(Opcode.Clear, 2, join(floats(0, 0, 0, 0, 0), words(0, 1, 0, 0)));
      const leftDraw = draw(new Uint8Array()), rightDraw = draw(new Uint8Array());
      new Uint32Array(leftDraw.buffer)[20] = width / 2;
      new Uint32Array(rightDraw.buffer)[18] = width / 2;
      new Uint32Array(rightDraw.buffer)[20] = width / 2;
      execute(Opcode.DrawBatch, 0, join(words(2, 2, leftDraw.byteLength), leftDraw, words(2, rightDraw.byteLength), rightDraw));
      if (execute(Opcode.Readback, 2).some((value, n) => value !== pixels[n])) { throw new Error('batched draws lost owned packet state or render-pass ordering'); }
      const compactPipeline = pipeline();
      compactPipeline[26] = 3;
      execute(Opcode.CreatePipeline, 17, join(compactPipeline, vertex, fragment));
      const compactVertices = new Float32Array(24);
      for (let n = 0; n < 3; ++n) { compactVertices.set(vertices.subarray(n * 64, n * 64 + 8), n * 8); }
      const compactDraw = join(draw(new Uint8Array()).subarray(0, 204), compactVertices, new Uint16Array([0, 1, 2]), floats(1, 1, 1, 1));
      const compactWords = new Uint32Array(compactDraw.buffer, 0, 51);
      compactWords[0] = 17; compactWords[7] = 3; compactWords[9] = compactVertices.byteLength; compactWords[10] = 6;
      execute(Opcode.Clear, 2, join(floats(0, 0, 0, 0, 1), words(0, 7, 4, 0)));
      const compactLeft = compactDraw.slice(), compactRight = compactDraw.slice();
      new Uint32Array(compactLeft.buffer, 0, 51)[20] = width / 2;
      new Uint32Array(compactRight.buffer, 0, 51)[18] = width / 2;
      new Uint32Array(compactRight.buffer, 0, 51)[20] = width / 2;
      execute(Opcode.DrawBatch, 0, join(words(2, 2, compactLeft.byteLength), compactLeft, words(2, compactRight.byteLength), compactRight));
      if (execute(Opcode.Readback, 2).some((value, n) => value !== pixels[n])) { throw new Error('compact indexed vertices changed shader locations, UVs or pixels'); }
      const samplerDraws = [];
      for (let n = 0; n < 140; ++n) {
        const owned = compactDraw.slice();
        const state = new Uint32Array(owned.buffer, 0, 51);
        const firstKey = n * 4;
        for (let n = 0; n < 4; ++n) {
          const key = firstKey + n;
          state[22 + n * 7] = 1 + key % 3;
          state[23 + n * 7] = 1 + Math.floor(key / 3) % 3;
          state[27 + n * 7] = Math.floor(key / 9) % 33;
          state[28 + n * 7] = 32;
        }
        samplerDraws.push(words(2, owned.byteLength), owned);
      }
      samplerDraws.push(words(2, compactDraw.byteLength), compactDraw);
      execute(Opcode.DrawBatch, 0, join(words(141), ...samplerDraws));
      if (execute(Opcode.Readback, 2).some((value, n) => value !== pixels[n])) { throw new Error('sampler eviction or shared upload arenas corrupted final draw'); }
      execute(Opcode.Destroy, 17);
      const packedDepth = new Uint32Array(width * 4);
      const stencilMask = new Uint32Array(8);
      for (let n = 0; n < packedDepth.length; ++n) {
        const stencil = ((n % 4) * 85 + Math.floor(n / width) * 17) & 255;
        packedDepth[n] = (((n + Math.floor(n / width)) % 2 ? 0xc00000 : 0x400000) << 8) | stencil;
        stencilMask[stencil >>> 5] |= 1 << (stencil & 31);
      }
      execute(Opcode.UploadDepth, 4, join(words(width, 4, 24), stencilMask, packedDepth));
      const shadowFragment = encoder.encode('@group(0) @binding(1) var image: texture_depth_2d; @fragment fn fs_main(@builtin(position) p: vec4<f32>) -> @location(0) vec4<f32> { return vec4<f32>(select(0.0, 1.0, textureLoad(image, vec2<i32>(p.xy), 0) > 0.5)); }');
      const shadowPipeline = pipeline(8, 15, 0, undefined, shadowFragment.length);
      shadowPipeline[13] = 0;
      shadowPipeline[25] = 15;
      execute(Opcode.CreatePipeline, 16, join(shadowPipeline, vertex, shadowFragment));
      const shadowDraw = draw(new Uint8Array(), 16, 0, 4);
      new Uint32Array(shadowDraw.buffer)[5] = 0;
      execute(Opcode.Draw, 2, shadowDraw);
      if (execute(Opcode.Readback, 2).some((value, n) => value !== ((packedDepth[Math.floor(n / 4)] >>> 8) > 0x800000 ? 255 : 0))) { throw new Error('completed sampled depth target changed'); }
      execute(Opcode.Destroy, 16);
      execute(Opcode.Clear, 2, join(floats(0, 0, 0, 0, 0), words(0, 1, 0, 0)));
      const importedStencil = execute(Opcode.Readback, 4, words(1));
      if (importedStencil.length !== packedDepth.length || importedStencil.some((value, n) => value !== (((n % 4) * 85 + Math.floor(n / width) * 17) & 255))) {
        throw new Error('nonuniform imported stencil values changed');
      }
      execute(Opcode.Clear, 2, join(floats(0, 0, 0, 0, 0), words(0, 1, 0, 0)));
      execute(Opcode.Draw, 2, draw(new Uint8Array(), 7, 16));
      const maskedStencil = execute(Opcode.Readback, 4, words(1));
      if (maskedStencil.some((value, n) => value !== ((importedStencil[n] & ~16) | 16))) {
        throw new Error('stencil replace changed bits outside its write mask');
      }
      if (execute(Opcode.Readback, 2).some(value => value !== 0)) {
        throw new Error('stencil-only mask draw changed color contents');
      }
      vertices[2] = vertices[66] = vertices[130] = 0.5;
      execute(Opcode.Draw, 2, draw(new Uint8Array(), 6));
      const depthPixels = execute(Opcode.Readback, 2);
      if (depthPixels.some((value, n) => value !== ((Math.floor(n / 4) + Math.floor(n / (width * 4))) % 2 ? pixels[n] : 0))) {
        throw new Error('nonuniform imported depth comparison changed: ' + JSON.stringify({ actual: [...depthPixels], original: [...pixels] }));
      }
      execute(Opcode.Clear, 2, join(floats(0, 0, 0, 0, 0), words(0, 1, 0, 0)));
      execute(Opcode.Draw, 2, draw(new Uint8Array(), 8, 0));
      if (execute(Opcode.Readback, 2).some(value => value !== 0)) {
        throw new Error('stencil comparison ignored the dynamic reference or read mask');
      }
      execute(Opcode.Draw, 2, draw(new Uint8Array(), 8, 16));
      if (execute(Opcode.Readback, 2).some((value, n) => value !== pixels[n])) {
        throw new Error('stencil comparison rejected its matching dynamic reference');
      }
      const mipPixels = new Uint8Array([...Array(4).fill([255, 0, 0, 255]).flat(), 0, 0, 255, 255]);
      execute(Opcode.CreateTexture, 10, join(words(2, 2, 0, 2), mipPixels));
      execute(Opcode.Draw, 2, draw(new Uint8Array(), 3, 0, 10));
      if (execute(Opcode.Readback, 2).some((value, n) => value !== [255, 0, 0, 255][n % 4])) { throw new Error('mip chain base level changed'); }
      execute(Opcode.Draw, 2, draw(new Uint8Array(), 9, 0, 10));
      if (execute(Opcode.Readback, 2).some((value, n) => value !== [0, 0, 255, 255][n % 4])) { throw new Error('mip chain level one was omitted or uploaded out of order'); }
      sampling[5] = 1;
      execute(Opcode.Draw, 2, draw(new Uint8Array(), 3, 0, 10));
      if (execute(Opcode.Readback, 2).some((value, n) => value !== [0, 0, 255, 255][n % 4])) { throw new Error('minimum mip clamp was ignored'); }
      sampling[5] = 0;
      execute(Opcode.Draw, 2, draw(new Uint8Array(), 11, 0, 10));
      if (execute(Opcode.Readback, 2).some((value, n) => value !== [0, 0, 255, 255][n % 4])) { throw new Error('texture LOD bias was ignored'); }
      sampling[6] = 0;
      execute(Opcode.Draw, 2, draw(new Uint8Array(), 11, 0, 10));
      if (execute(Opcode.Readback, 2).some((value, n) => value !== [255, 0, 0, 255][n % 4])) { throw new Error('disabled mip filtering did not clamp the biased sample'); }
      sampling[6] = 32;
      const cubeColors = [[255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255], [255, 255, 0, 255], [255, 0, 255, 255], [0, 255, 255, 255]];
      execute(Opcode.CreateTexture, 12, join(words(1, 1, 2, 1), new Uint8Array(cubeColors.flat())));
      const directions = ['1,0,0', '-1,0,0', '0,1,0', '0,-1,0', '0,0,1', '0,0,-1'];
      for (let n = 0; n < 6; ++n) {
        const cubeFragment = encoder.encode('@group(0) @binding(1) var image: texture_cube<f32>; @group(0) @binding(2) var sampling: sampler; @fragment fn fs_main() -> @location(0) vec4<f32> { return textureSample(image, sampling, vec3<f32>(' + directions[n] + ')); }');
        const cubePipeline = pipeline(8, 15, 0, undefined, cubeFragment.length);
        cubePipeline[23] = 15;
        execute(Opcode.CreatePipeline, 13, join(cubePipeline, vertex, cubeFragment));
        execute(Opcode.Draw, 2, draw(new Uint8Array(), 13, 0, 12));
        if (execute(Opcode.Readback, 2).some((value, channel) => value !== cubeColors[n][channel % 4])) { throw new Error('cube face direction or channels changed: ' + n); }
        execute(Opcode.Destroy, 13);
      }
      execute(Opcode.Destroy, 12);

      const rectangles = new Int32Array([-1, 1, 2, 3, width - 1, 0, width + 2, 1]);
      const inside = pixel => {
        const x = pixel % width, y = Math.floor(pixel / width);
        return (x < 2 && y >= 1 && y < 3) || (x === width - 1 && y === 0);
      };
      for (let n = 1; n <= 7; ++n) {
        const flags = n;
        execute(Opcode.UploadDepth, 4, join(words(width, 4, 24), stencilMask, packedDepth));
        execute(Opcode.Clear, 2, join(floats(7 / 255, 9 / 255, 11 / 255, 1, 0), words(0, 1, 0, 0)));
        execute(Opcode.Clear, flags & 1 ? 2 : 0, join(floats(33 / 255, 66 / 255, 99 / 255, 128 / 255, 0.75), words(73, flags, flags & 6 ? 4 : 0, 2), rectangles));
        const clearPixels = execute(Opcode.Readback, 2);
        if (clearPixels.some((value, n) => value !== (inside(Math.floor(n / 4)) && (flags & 1) ? [33, 66, 99, 128] : [7, 9, 11, 255])[n % 4])) { throw new Error('partial color clear changed its rectangle or neighboring pixels: ' + flags); }
        const clearStencil = execute(Opcode.Readback, 4, words(1));
        if (clearStencil.some((value, n) => value !== (inside(n) && (flags & 4) ? 73 : importedStencil[n]))) { throw new Error('partial stencil clear changed its rectangle or neighboring stencil: ' + flags); }
        execute(Opcode.Draw, 2, draw(new Uint8Array(), 6));
        if (execute(Opcode.Readback, 2).some((value, n) => value !== ((inside(Math.floor(n / 4)) && (flags & 2)) || (packedDepth[Math.floor(n / 4)] >>> 8) > 0x800000 ? pixels[n] : clearPixels[n]))) { throw new Error('partial depth clear changed its rectangle or neighboring depth: ' + flags); }
      }
      const beforeDepthOnly = execute(Opcode.Readback, 2);
      execute(Opcode.Clear, 0, join(floats(0, 0, 0, 0, 1), words(22, 6, 4, 0)));
      vertices[2] = vertices[66] = vertices[130] = 0.25;
      execute(Opcode.Draw, 0, draw(new Uint8Array(), 15));
      if (execute(Opcode.Readback, 2).some((value, n) => value !== beforeDepthOnly[n])) { throw new Error('depth-only draw changed color'); }
      if (execute(Opcode.Readback, 4, words(1)).some(value => value !== 22)) { throw new Error('depth-only draw changed stencil'); }
      execute(Opcode.Clear, 2, join(floats(0, 0, 0, 0, 0), words(0, 1, 0, 0)));
      vertices[2] = vertices[66] = vertices[130] = 0.5;
      execute(Opcode.Draw, 2, draw(new Uint8Array(), 6));
      if (execute(Opcode.Readback, 2).some(value => value !== 0)) { throw new Error('depth-only draw did not update depth'); }
      execute(Opcode.Destroy, 10);
      execute(Opcode.Present, 5, words(0));
      const identity = execute(Opcode.Readback, 0);
      if (identity.length !== pixels.length || identity.some((value, index) => value !== pixels[index])) { throw new Error('identity gamma presentation mismatch'); }
      if (n === 0) {
        const gamma = new Uint8Array(768);
        for (let n = 0; n < 256; ++n) { gamma[n] = 255 - n; gamma[n + 256] = (n + 37) % 256; gamma[n + 512] = Math.floor(n / 2); }
        execute(Opcode.SetGamma, 0, gamma);
        execute(Opcode.Present, 5, words(0));
        const composed = execute(Opcode.Readback, 0);
        if (composed.some((value, index) => {
          const channel = index % 4;
          return value !== (channel === 3 ? pixels[index] : gamma[channel * 256 + pixels[index]]);
        })) { throw new Error('per-channel gamma LUT mismatch'); }
        const raw = execute(Opcode.Readback, 5);
        if (raw.some((value, index) => value !== pixels[index])) { throw new Error('presentation gamma modified the source backbuffer'); }
        execute(Opcode.Configure, 0, words(8, 8));
        execute(Opcode.Present, 5, words(1));
        const pillarbox = execute(Opcode.Readback, 0);
        if (pillarbox.length !== 8 * 8 * 4 || pillarbox.some((value, index) => {
          const pixel = Math.floor(index / 4), x = pixel % 8, y = Math.floor(pixel / 8), channel = index % 4;
          if (y === 0 || y === 7) { return value !== (channel === 3 ? 255 : 0); }
          const source = texels[(y < 4 ? 0 : 2) + (x < 4 ? 0 : 1)][channel];
          return value !== (channel === 3 ? source : gamma[channel * 256 + source]);
        })) { throw new Error('presentation scaling or pillarbox mismatch'); }
        for (let n = 0; n < 768; ++n) { gamma[n] = n % 256; }
        execute(Opcode.SetGamma, 0, gamma);
        execute(Opcode.SetGamma, 0, new Uint8Array(767), Status.Error);
      }
      execute(Opcode.Destroy, 2);
      execute(Opcode.Destroy, 4);
      execute(Opcode.Destroy, 5);
    }
    const cubeStress = join(words(256, 256, 2, 1), new Uint8Array(256 * 256 * 6 * 4));
    for (let n = 0; n < 210; ++n) {
      execute(Opcode.CreateTexture, 14, cubeStress);
      execute(Opcode.Destroy, 14);
    }
    execute(Opcode.CreatePipeline, 77, join(words(3, 3, 4, 1, 1, 0, 2, 1, 1, 2, 1, 1, 15, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 1), encoder.encode('badbad')), Status.Error);
    execute(Opcode.Destroy, 999, undefined, Status.Error);
    execute(Opcode.DrawBatch, 0, words(1, 999, 0), Status.Error);
    execute(Opcode.Destroy, 15);
    execute(Opcode.Destroy, 1);
    execute(Opcode.Destroy, 3);
    execute(Opcode.Destroy, 6);
    execute(Opcode.Destroy, 7);
    execute(Opcode.Destroy, 8);
    execute(Opcode.Destroy, 9);
    execute(Opcode.Destroy, 11);
    execute(Opcode.Flush, 0);
    execute(Opcode.Shutdown, 0);
    globalThis.postMessage({ ok: true, evidence, blockedGuestCompleted: true, texturedDrawSizes: [[4, 4], [8, 4]], expectedTexels: texels });
  } catch (error) { globalThis.postMessage({ ok: false, error: String(error) }); }
};
