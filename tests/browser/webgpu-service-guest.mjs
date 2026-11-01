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
    const module = await createGuest({ webgpuMailbox: event.data.mailbox });
    const evidence = [];
    const execute = (opcode, resource, bytes = new Uint8Array(), expected = Status.Ok) => {
      const input = join(bytes);
      const pointer = module._malloc(input.byteLength || 1);
      module.HEAPU8.set(input, pointer);
      const status = module._gpu_execute(opcode, resource, pointer, input.byteLength);
      module._free(pointer);
      const reply = module.HEAPU8.slice(module._gpu_reply(), module._gpu_reply() + module._gpu_reply_size());
      evidence.push({ command: module._gpu_command(), resource, opcode, status, pthreadMailboxUnbound: module._gpu_pthread_unbound() === 1 });
      if (module._gpu_pthread_unbound() !== 1) { throw new Error('fixture did not exercise unbound actual pthread'); }
      if (status !== expected) { throw new Error(`command ${module._gpu_command()} resource ${resource}: status ${status} ${new TextDecoder().decode(reply)}`); }
      return reply;
    };
    if (event.data.blockOnly) {
      execute(Opcode.Flush, 0, undefined, Status.Closed);
      globalThis.postMessage({ ok: true, shutdownResolvedBlockedGuest: true, evidence });
      return;
    }
    execute(Opcode.Configure, 0, words(4, 4));
    execute(Opcode.CreateTexture, 1, join(words(2, 2, 0), new Uint8Array(texels.flat())));
    execute(Opcode.CreatePipeline, 3, join(words(vertex.length, fragment.length, 4, 1, 1, 0, 2, 1, 1, 2, 1, 1, 15, 1, 1, 8), vertex, fragment));
    for (let n = 0; n < 2; ++n) {
      const width = n === 0 ? 4 : 8;
      execute(Opcode.Configure, 0, words(width, 4));
      execute(Opcode.CreateTexture, 2, join(words(width, 4, 0), new Uint8Array(width * 4 * 4)));
      execute(Opcode.CreateTexture, 4, words(width, 4, 1));
      execute(Opcode.CreateTexture, 5, join(words(width, 4, 0), new Uint8Array(width * 4 * 4)));
      execute(Opcode.Clear, 2, join(floats(0, 0, 0, 0, 1), words(0, 7, 4)));
      const vertices = new Float32Array(64 * 3);
      vertices.set([-1, -1, 0, 1, 0, 1, 0, 0], 0); vertices.set([3, -1, 0, 1, 2, 1, 0, 0], 64); vertices.set([-1, 3, 0, 1, 0, -1, 0, 0], 128);
      const sampling = words(...Array(4).fill([1, 1, 1, 1, 1]).flat());
      const draw = indices => join(words(3, 1, 1, 1, 1, 4, 3, indices.length, indices.BYTES_PER_ELEMENT === 4 ? 1 : 0, vertices.byteLength, indices.byteLength, 16), floats(0, 0, width, 4, 0, 1), words(0, 0, width, 4), sampling, vertices, indices, floats(1, 1, 1, 1));
      if (n === 0) { execute(Opcode.Draw, 2, draw(new Uint8Array())); }
      else { execute(Opcode.Draw, 2, draw(new Uint16Array([0, 1, 2]))); execute(Opcode.Draw, 2, draw(new Uint32Array([0, 1, 2]))); }
      execute(Opcode.Flush, 0);
      execute(Opcode.Copy, 5, words(2, width, 4));
      const pixels = execute(Opcode.Readback, 5);
      if (pixels.length !== width * 4 * 4 || pixels.some((value, index) => {
        const pixel = Math.floor(index / 4);
        const x = pixel % width;
        const y = Math.floor(pixel / width);
        return value !== texels[(y < 2 ? 0 : 2) + (x < width / 2 ? 0 : 1)][index % 4];
      })) { throw new Error('textured draw orientation, alpha or channel mismatch'); }
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
    execute(Opcode.CreatePipeline, 77, join(words(3, 3, 4, 1, 1, 0, 2, 1, 1, 2, 1, 1, 15, 0, 0, 8), encoder.encode('badbad')), Status.Error);
    execute(Opcode.Destroy, 999, undefined, Status.Error);
    execute(Opcode.Destroy, 1);
    execute(Opcode.Destroy, 3);
    execute(Opcode.Flush, 0);
    execute(Opcode.Shutdown, 0);
    globalThis.postMessage({ ok: true, evidence, blockedGuestCompleted: true, texturedDrawSizes: [[4, 4], [8, 4]], expectedTexels: texels });
  } catch (error) { globalThis.postMessage({ ok: false, error: String(error) }); }
};
