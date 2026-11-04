const root = '/out/menu-shaders-tests/';
const text = async name => {
  const response = await fetch(root + name);
  if (!response.ok) {
    throw new Error(`missing fixture ${name}`);
  }
  return response.text();
};

const run = async () => {
  const adapter = await navigator.gpu.requestAdapter();
  if (!adapter) {
    throw new Error('WebGPU adapter unavailable');
  }
  const device = await adapter.requestDevice();
  const errors = [];
  device.addEventListener('uncapturederror', event => errors.push(event.error.message));
  const names = [...Array.from({ length: 16 }, (_, n) => `shadow${n}`), ...Array.from({ length: 16 }, (_, n) => `psshadow${n}`), 'depth-bias', 'depth-bias-color', 'depth-only', 'border', 'border-mip', 'cube', 'pscube', 'volume', 'psvolume1', 'psvolume2', 'psvolume3', 'psreflect', 'psdotst', 'psdep15', 'psdep16', 'plain', 'textured', 'flat', 'stages', 'program', 'paired', 'relative', 'rcc', 'rcp', 'rsq', 'pixel', 'psef', ...Array.from({ length: 8 }, (_, n) => `pssum${n}`), 'psdot1', 'psdot2', 'psdot3', 'pskill', 'pixel-program', 'psconstants', 'psmux', 'pssimultaneous', ...Array.from({ length: 8 }, (_, n) => `psinput${n}`), ...[0, 8, 16, 24, 32, 48].map(n => `psoutput${n}`), ...Array.from({ length: 13 }, (_, n) => `mac${n + 1}`), ...Array.from({ length: 23 }, (_, n) => n + 2).filter(n => n !== 17).map(n => `op${n}`), ...Array.from({ length: 5 }, (_, n) => `combine${n + 18}`)];
  const pipelines = new Map();
  let textured;
  for (const name of names) {
    const vertex = device.createShaderModule({ code: await text(`${name}.vs.wgsl`) });
    const fragment = device.createShaderModule({ code: await text(`${name}.fs.wgsl`) });
    for (const module of [vertex, fragment]) {
      const info = await module.getCompilationInfo();
      const failures = info.messages.filter(message => message.type === 'error');
      if (failures.length) {
        throw new Error(`${name}: ${JSON.stringify(failures.map(message => ({ text: message.message, line: message.lineNum })))}`);
      }
    }
    const regs = name.startsWith('op') || name === 'stages' || name === 'psreflect' || name === 'psdotst' || name.startsWith('psdep') ? Array.from({ length: 16 }, (_, n) => n) : [0, 3, 9];
    device.pushErrorScope('validation');
    const pipeline = await device.createRenderPipelineAsync({
      layout: 'auto',
      vertex: { module: vertex, entryPoint: 'vs_main', buffers: [{ arrayStride: 256, attributes: regs.map(n => ({ shaderLocation: n, offset: n * 16, format: 'float32x4' })) }] },
      fragment: { module: fragment, entryPoint: 'fs_main', targets: name === 'depth-only' || name === 'depth-bias' ? [] : [{ format: 'rgba8unorm' }] },
      ...(name.startsWith('depth-') ? { depthStencil: { format: 'depth24plus-stencil8', depthWriteEnabled: true, depthCompare: 'less-equal' } } : {}),
      primitive: { topology: 'triangle-list' }
    });
    const error = await device.popErrorScope();
    if (error) {
      throw new Error(`${name}: ${error.message}`);
    }
    pipelines.set(name, pipeline);
    if (name === 'textured') {
      textured = pipeline;
    }
  }
  const uniform = device.createBuffer({ size: 3824, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const constants = new Float32Array(await (await fetch(root + 'constants.bin')).arrayBuffer());
  device.queue.writeBuffer(uniform, 0, constants);
  const source = device.createTexture({ size: [1, 1], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
  device.queue.writeTexture({ texture: source }, new Uint8Array([64, 128, 192, 255]), {}, [1, 1]);
  const sampler = device.createSampler({ magFilter: 'nearest', minFilter: 'nearest' });
  const bind = pipeline => device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [
    { binding: 0, resource: { buffer: uniform } },
    { binding: 1, resource: source.createView() },
    ...([pipelines.get('border'), pipelines.get('border-mip')].includes(pipeline) ? [] : [{ binding: 2, resource: sampler }])
  ] });
  const vertices = new Float32Array(3 * 64);
  for (let n = 0; n < 3; ++n) {
    vertices.set([n === 1 ? 4 : 0, n === 2 ? 4 : 0, 0.5, 2], n * 64);
    vertices.set([0.5, 1, 0.25, 1], n * 64 + 12);
    vertices.set([0, 0, 0, 1], n * 64 + 36);
  }
  const vertexBuffer = device.createBuffer({ size: vertices.byteLength, usage: GPUBufferUsage.VERTEX | GPUBufferUsage.COPY_DST });
  device.queue.writeBuffer(vertexBuffer, 0, vertices);
  const target = device.createTexture({ size: [4, 4], format: 'rgba8unorm', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
  const readback = device.createBuffer({ size: 1024, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
  const render = async (pipeline = textured, binding = bind(pipeline)) => {
    device.queue.writeBuffer(vertexBuffer, 0, vertices);
    device.queue.writeBuffer(uniform, 0, constants);
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({ colorAttachments: [{ view: target.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }] });
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, binding);
    pass.setVertexBuffer(0, vertexBuffer);
    pass.draw(3);
    pass.end();
    encoder.copyTextureToBuffer({ texture: target }, { buffer: readback, bytesPerRow: 256 }, [4, 4]);
    device.queue.submit([encoder.finish()]);
    await readback.mapAsync(GPUMapMode.READ);
    const pixel = [...new Uint8Array(readback.getMappedRange()).slice(0, 4)];
    readback.unmap();
    return pixel;
  };
  const shadowPixels = [];
  const shadowSource = device.createTexture({ size: [2, 2], format: 'depth24plus-stencil8', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.RENDER_ATTACHMENT });
  const shadowWriter = device.createRenderPipeline({
    layout: 'auto',
    vertex: { module: device.createShaderModule({ code: '@vertex fn vs_main(@builtin(vertex_index) n: u32) -> @builtin(position) vec4<f32> { let p = array<vec2<f32>,3>(vec2<f32>(-1,-1),vec2<f32>(3,-1),vec2<f32>(-1,3)); return vec4<f32>(p[n],0.5,1); }' }), entryPoint: 'vs_main' },
    fragment: { module: device.createShaderModule({ code: '@fragment fn fs_main(@builtin(position) p: vec4<f32>) -> @builtin(frag_depth) f32 { return select(0.25, 0.75, p.x > 1.0); }' }), entryPoint: 'fs_main', targets: [] },
    depthStencil: { format: 'depth24plus-stencil8', depthWriteEnabled: true, depthCompare: 'always' }
  });
  const shadowEncoder = device.createCommandEncoder();
  const shadowPass = shadowEncoder.beginRenderPass({ colorAttachments: [], depthStencilAttachment: { view: shadowSource.createView(), depthLoadOp: 'clear', depthStoreOp: 'store', depthClearValue: 1, stencilLoadOp: 'clear', stencilStoreOp: 'store', stencilClearValue: 0 } });
  shadowPass.setPipeline(shadowWriter); shadowPass.draw(3); shadowPass.end();
  device.queue.submit([shadowEncoder.finish()]);
  const savedConstants = constants.slice();
  const savedVertices = vertices.slice();
  constants.set([2, 2, 0, 0], 780);
  for (let n = 0; n < 32; ++n) {
    const pipeline = pipelines.get((n < 16 ? 'shadow' : 'psshadow') + n % 16);
    const binding = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: uniform } }, { binding: 1, resource: shadowSource.createView({ aspect: 'depth-only' }) }] });
    const maximum = n % 16 < 8 ? 16777215 : 65535;
    for (const reference of [0.125, 0.5, 0.875]) {
      for (let n = 0; n < 3; ++n) { vertices.set([2, 2, reference * maximum * 2, 2], n * 64 + 36); }
      const value = await render(pipeline, binding);
      const comparisons = depth => [false, depth < reference, depth === reference, depth <= reference, depth > reference, depth !== reference, depth >= reference, true];
      const fraction = (Number(comparisons(0.25)[n % 8]) + Number(comparisons(0.75)[n % 8])) / 2;
      const expected = (n < 16 ? [1, 1, 1, 1] : [0.5, 1, 0.25, 1]).map(channel => Math.round(channel * fraction * 255));
      if (value.some((channel, n) => Math.abs(channel - expected[n]) > 1)) { throw new Error('shadow comparison/filter mismatch ' + n + ', ' + reference + ': ' + value + ' expected ' + expected); }
      shadowPixels.push(value);
    }
  }
  constants.set(savedConstants); vertices.set(savedVertices); shadowSource.destroy();
  const pixel = await render();
  if (pixel.some((value, n) => value !== [32, 128, 48, 255][n])) {
    throw new Error(`texture modulate alpha/UV/clip fixture pixel ${pixel}`);
  }
  const depthTarget = device.createTexture({ size: [4, 4], format: 'depth24plus-stencil8', usage: GPUTextureUsage.RENDER_ATTACHMENT });
  const depthProbe = await device.createRenderPipelineAsync({
    layout: 'auto',
    vertex: { module: device.createShaderModule({ code: '@vertex fn vs_main(@builtin(vertex_index) n: u32) -> @builtin(position) vec4<f32> { let positions = array<vec2<f32>,3>(vec2<f32>(-1,-1),vec2<f32>(3,-1),vec2<f32>(-1,3)); return vec4<f32>(positions[n],0.75,1); }' }), entryPoint: 'vs_main' },
    fragment: { module: device.createShaderModule({ code: '@fragment fn fs_main() -> @location(0) vec4<f32> { return vec4<f32>(1,0,0,1); }' }), entryPoint: 'fs_main', targets: [{ format: 'rgba8unorm' }] },
    depthStencil: { format: 'depth24plus-stencil8', depthWriteEnabled: false, depthCompare: 'less-equal' },
    primitive: { topology: 'triangle-list' }
  });
  const depthOnlyPixels = [];
  for (const alpha of [0, 255]) {
    device.queue.writeTexture({ texture: source }, new Uint8Array([64, 128, 192, alpha]), {}, [1, 1]);
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({ colorAttachments: [], depthStencilAttachment: { view: depthTarget.createView(), depthLoadOp: 'clear', depthStoreOp: 'store', depthClearValue: 1, stencilLoadOp: 'clear', stencilStoreOp: 'store', stencilClearValue: 0 } });
    pass.setPipeline(pipelines.get('depth-only'));
    pass.setBindGroup(0, bind(pipelines.get('depth-only')));
    pass.setVertexBuffer(0, vertexBuffer);
    pass.draw(3);
    pass.end();
    const probe = encoder.beginRenderPass({ colorAttachments: [{ view: target.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }], depthStencilAttachment: { view: depthTarget.createView(), depthLoadOp: 'load', depthStoreOp: 'store', stencilLoadOp: 'load', stencilStoreOp: 'store' } });
    probe.setPipeline(depthProbe);
    probe.draw(3);
    probe.end();
    encoder.copyTextureToBuffer({ texture: target }, { buffer: readback, bytesPerRow: 256 }, [4, 4]);
    device.queue.submit([encoder.finish()]);
    await readback.mapAsync(GPUMapMode.READ);
    const value = [...new Uint8Array(readback.getMappedRange()).slice(0, 4)];
    readback.unmap();
    const expected = alpha ? [0, 0, 0, 0] : [255, 0, 0, 255];
    if (value.some((channel, n) => channel !== expected[n])) { throw new Error('depth-only alpha test changed depth visibility: ' + alpha + ', ' + value); }
    depthOnlyPixels.push(value);
  }
  device.queue.writeTexture({ texture: source }, new Uint8Array([64, 128, 192, 255]), {}, [1, 1]);
  const depthBiasPixels = [];
  for (const name of ['depth-bias', 'depth-bias-color']) {
    for (const sample of [{ base: 0.7, gradient: 0, offset: 0, slope: 0, alpha: 255, visible: false },
      { base: 0.7, gradient: 0, offset: 0.125, slope: 0, alpha: 255, visible: true },
      { base: 0.7, gradient: 0, offset: -0.125, slope: 0, alpha: 255, visible: false },
      { base: 0.45, gradient: 0.6, offset: 0, slope: 2, alpha: 255, visible: true },
      { base: 0.45, gradient: 0.6, offset: 0, slope: -2, alpha: 255, visible: false },
      { base: 0.7, gradient: 0, offset: 10, slope: 0, alpha: 255, visible: true },
      { base: 0.7, gradient: 0, offset: -10, slope: 0, alpha: 255, visible: false },
      { base: 0.7, gradient: 0, offset: 0, slope: 0, alpha: 0, visible: true }]) {
      for (let n = 0; n < 3; ++n) { vertices[n * 64 + 2] = sample.base + (n === 1 ? sample.gradient : 0); }
      constants[952] = sample.offset; constants[953] = sample.slope;
      device.queue.writeBuffer(uniform, 0, constants);
      device.queue.writeBuffer(vertexBuffer, 0, vertices);
      device.queue.writeTexture({ texture: source }, new Uint8Array([64, 128, 192, sample.alpha]), {}, [1, 1]);
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginRenderPass({ colorAttachments: name === 'depth-bias' ? [] : [{ view: target.createView(), loadOp: 'clear', storeOp: 'store' }], depthStencilAttachment: { view: depthTarget.createView(), depthLoadOp: 'clear', depthStoreOp: 'store', depthClearValue: 1, stencilLoadOp: 'clear', stencilStoreOp: 'store', stencilClearValue: 0 } });
      pass.setPipeline(pipelines.get(name));
      pass.setBindGroup(0, bind(pipelines.get(name)));
      pass.setVertexBuffer(0, vertexBuffer);
      pass.draw(3);
      pass.end();
      const probe = encoder.beginRenderPass({ colorAttachments: [{ view: target.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }], depthStencilAttachment: { view: depthTarget.createView(), depthLoadOp: 'load', depthStoreOp: 'store', stencilLoadOp: 'load', stencilStoreOp: 'store' } });
      probe.setPipeline(depthProbe);
      probe.draw(3);
      probe.end();
      encoder.copyTextureToBuffer({ texture: target }, { buffer: readback, bytesPerRow: 256 }, [4, 4]);
      device.queue.submit([encoder.finish()]);
      await readback.mapAsync(GPUMapMode.READ);
      const value = [...new Uint8Array(readback.getMappedRange()).slice(0, 4)];
      readback.unmap();
      const expected = sample.visible ? [255, 0, 0, 255] : [0, 0, 0, 0];
      if (value.some((channel, n) => channel !== expected[n])) { throw new Error(name + ' visibility mismatch: ' + JSON.stringify(sample) + ', ' + value); }
      depthBiasPixels.push(value);
    }
  }
  constants[952] = constants[953] = 0;
  for (let n = 0; n < 3; ++n) { vertices[n * 64 + 2] = 0.5; }
  device.queue.writeTexture({ texture: source }, new Uint8Array([64, 128, 192, 255]), {}, [1, 1]);
  depthTarget.destroy();
  const borderPixels = [];
  for (const sample of [{ uv: [0.5, 0.5], color: [32, 128, 48, 255] }, { uv: [0, 0.5], color: [16, 64, 56, 255] },
    { uv: [-0.5, 0.5], color: [0, 0, 64, 255] }, { uv: [0, 0], color: [8, 32, 60, 255] }]) {
    for (let n = 0; n < 3; ++n) { vertices.set([...sample.uv, 0, 1], n * 64 + 36); }
    const value = await render(pipelines.get('border'));
    if (value.some((channel, n) => channel !== sample.color[n])) { throw new Error('filtered border sample ' + sample.uv + ': ' + value + ', expected ' + sample.color); }
    borderPixels.push(value);
  }
  const borderMipSource = device.createTexture({ size: [2, 2], mipLevelCount: 2, format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
  device.queue.writeTexture({ texture: borderMipSource }, new Uint8Array(Array(4).fill([255, 0, 0, 255]).flat()), { bytesPerRow: 8 }, [2, 2]);
  device.queue.writeTexture({ texture: borderMipSource, mipLevel: 1 }, new Uint8Array([0, 255, 0, 255]), {}, [1, 1]);
  const borderMipPipeline = pipelines.get('border-mip');
  const borderMipBind = device.createBindGroup({ layout: borderMipPipeline.getBindGroupLayout(0), entries: [
    { binding: 0, resource: { buffer: uniform } }, { binding: 1, resource: borderMipSource.createView() }
  ] });
  const borderMipPixels = [];
  for (const sample of [{ lod: 0, color: [128, 0, 0, 255] }, { lod: 1, color: [0, 255, 0, 255] }, { lod: 0.5, color: [64, 128, 0, 255] }]) {
    constants[948] = sample.lod;
    for (let n = 0; n < 3; ++n) { vertices.set([n === 1 ? 2.25 : 0.25, n === 2 ? 2.25 : 0.25, 0, 1], n * 64 + 36); }
    const value = await render(borderMipPipeline, borderMipBind);
    if (value.some((channel, n) => Math.abs(channel - sample.color[n]) > 1)) { throw new Error('border mip LOD ' + sample.lod + ': ' + value); }
    borderMipPixels.push(value);
  }
  constants[948] = 0;
  borderMipSource.destroy();
  for (let n = 0; n < 3; ++n) { vertices.set([0, 0, 0, 1], n * 64 + 36); }
  for (let n = 1; n < 3; ++n) {
    vertices.set([0, 0, 0, 1], n * 64 + 12);
  }
  const flatPixel = await render(pipelines.get('flat'));
  if (flatPixel.some((value, n) => value !== [32, 128, 48, 255][n])) {
    throw new Error(`flat color interpolation fixture pixel ${flatPixel}`);
  }
  for (let n = 1; n < 3; ++n) {
    vertices.set([0.5, 1, 0.25, 1], n * 64 + 12);
  }
  const programPixel = await render(pipelines.get('program'));
  if (programPixel.some((value, n) => value !== [32, 128, 48, 255][n])) {
    throw new Error(`programmable vertex move fixture pixel ${programPixel}`);
  }
  const registerPixels = [];
  for (const name of ['paired', 'relative']) {
    const value = await render(pipelines.get(name));
    if (value.some((channel, n) => channel !== [32, 128, 48, 255][n])) {
      throw new Error(`${name} register fixture pixel ${value}`);
    }
    registerPixels.push(value);
  }
  const macExpected = [[32, 128, 48, 255], [32, 128, 144, 255], [64, 128, 96, 255], [64, 128, 192, 255], [64, 128, 192, 255], [64, 128, 192, 255], [64, 128, 192, 255], [64, 128, 48, 255], [32, 128, 48, 255], [64, 128, 192, 255], [64, 128, 192, 255], [0, 0, 0, 255], [0, 0, 0, 255]];
  const macPixels = [];
  for (let n = 0; n < macExpected.length; ++n) {
    const value = await render(pipelines.get(`mac${n + 1}`));
    if (value.some((channel, index) => channel !== macExpected[n][index])) {
      throw new Error(`mac${n + 1} pixel ${value}, expected ${macExpected[n]}`);
    }
    macPixels.push(value);
  }
  const arithmeticTarget = device.createTexture({ size: [4, 4], format: 'rgba32float', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
  const arithmeticPixels = {};
  const arithmeticFixtures = [
    { name: 'rcc', flat: false, values: [2, -2, 0, -0, 1e-30, -1e-30, 1e30, -1e30, 1, -1], expected: input => {
      const inverse = Math.fround(1 / input);
      return Math.fround(Math.min(Math.max(Math.abs(inverse), Math.fround(5.42101e-20)), Math.fround(1.84467e19))) * (inverse < 0 ? -1 : 1);
    } },
    { name: 'rcp', flat: true, values: [2, -2, 0, -0, 1e-30, -1e-30, 1e30, -1e30, 1, -1], expected: input => Math.fround(1 / input) },
    { name: 'rsq', flat: true, values: [4, -4, 0, -0, 1e-30, -1e-30, 1e30, -1e30, 1, -1], expected: input => Math.fround(1 / Math.sqrt(Math.abs(input))) }
  ];
  for (const fixture of arithmeticFixtures) {
    const interpolation = fixture.flat ? ' @interpolate(flat)' : '';
    const vertex = device.createShaderModule({ code: (await text(fixture.name + '.vs.wgsl')).replace('@location(2) t0:', '@location(2)' + interpolation + ' t0:') });
    const fragment = device.createShaderModule({ code: '@fragment fn fs_main(@location(2)' + interpolation + ' value: vec4<f32>) -> @location(0) vec4<f32> { return value; }' });
    const pipeline = await device.createRenderPipelineAsync({
      layout: 'auto',
      vertex: { module: vertex, entryPoint: 'vs_main', buffers: [{ arrayStride: 256, attributes: [0, 3, 9].map(n => ({ shaderLocation: n, offset: n * 16, format: 'float32x4' })) }] },
      fragment: { module: fragment, entryPoint: 'fs_main', targets: [{ format: 'rgba32float' }] },
      primitive: { topology: 'triangle-list' }
    });
    const binding = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: uniform } }] });
    const samples = [];
    for (const value of fixture.values) {
      for (let n = 0; n < 3; ++n) { vertices[n * 64 + 13] = value; }
      device.queue.writeBuffer(vertexBuffer, 0, vertices);
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginRenderPass({ colorAttachments: [{ view: arithmeticTarget.createView(), loadOp: 'clear', storeOp: 'store' }] });
      pass.setPipeline(pipeline);
      pass.setBindGroup(0, binding);
      pass.setVertexBuffer(0, vertexBuffer);
      pass.draw(3);
      pass.end();
      encoder.copyTextureToBuffer({ texture: arithmeticTarget }, { buffer: readback, bytesPerRow: 256 }, [4, 4]);
      device.queue.submit([encoder.finish()]);
      await readback.mapAsync(GPUMapMode.READ);
      const pixel = [...new Float32Array(readback.getMappedRange()).slice(0, 4)];
      readback.unmap();
      const expected = fixture.expected(Math.fround(value));
      if (pixel.some(channel => !Number.isFinite(expected) ? channel !== expected : !Number.isFinite(channel) || Math.abs((channel - expected) / expected) > 0.000001)) {
        throw new Error(fixture.name + ' input ' + value + ' returned ' + pixel + ', expected ' + expected);
      }
      samples.push({ input: Object.is(value, -0) ? '-0' : value, pixel: pixel.map(channel => Number.isFinite(channel) ? channel : String(channel)) });
    }
    arithmeticPixels[fixture.name] = samples;
  }
  const reciprocalPixels = arithmeticPixels.rcc;
  const normalizationPixels = arithmeticPixels.rsq;
  const plainReciprocalPixels = arithmeticPixels.rcp;
  arithmeticTarget.destroy();
  for (let n = 0; n < 3; ++n) { vertices[n * 64 + 13] = 1; }
  const psExpected = new Map([
    ['pixel', [32, 128, 48, 255]], ['psef', [32, 128, 48, 255]], ...Array.from({ length: 8 }, (_, n) => [`pssum${n}`, [...[32 / 255, 128 / 255, 48 / 255].map((channel, component) => {
      const specular = [0.5, 1, 0.25][component];
      let sum = (n & 1 ? 1 - channel : channel) + (n & 2 ? 1 - specular : specular);
      if (n & 4) { sum = Math.max(0, Math.min(1, sum)); }
      return Math.round(Math.max(0, Math.min(1, sum - 0.5)) * 255);
    }), 255]]), ['psdot1', [208, 208, 208, 255]], ['psdot2', [208, 208, 208, 255]], ['psdot3', [255, 255, 255, 255]], ['pixel-program', [32, 128, 48, 255]],
    ['psconstants', [12, 21, 32, 80]], ['psmux', [64, 128, 192, 255]], ['pssimultaneous', [255, 255, 255, 64]],
    ...[[128, 255, 64, 255], [128, 0, 191, 0], [0, 255, 0, 255], [0, 0, 128, 0], [0, 128, 0, 128], [0, 0, 64, 0], [128, 255, 64, 255], [0, 0, 0, 0]].map((pixel, n) => [`psinput${n}`, pixel]),
    ...[0, 8, 16, 24, 32, 48].map((mapping, n) => [`psoutput${mapping}`, [[128, 255, 64, 255], [0, 128, 0, 128], [255, 255, 128, 255], [0, 255, 0, 255], [255, 255, 255, 255], [64, 128, 32, 128]][n]])
  ]);
  const psPixels = {};
  for (const [name, expected] of psExpected) {
    const value = await render(pipelines.get(name));
    if (value.some((channel, n) => Math.abs(channel - expected[n]) > (name.startsWith('pssum') ? 1 : 0))) {
      throw new Error(`${name} pixel ${value}, expected ${expected}`);
    }
    psPixels[name] = value;
  }
  const cube = device.createTexture({ size: [1, 1, 6], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
  const faceColors = [[255, 0, 0, 255], [0, 255, 0, 255], [0, 0, 255, 255], [255, 255, 0, 255], [255, 0, 255, 255], [0, 255, 255, 255]];
  for (let n = 0; n < 6; ++n) { device.queue.writeTexture({ texture: cube, origin: [0, 0, n] }, new Uint8Array(faceColors[n]), {}, [1, 1, 1]); }
  const reflected = pipelines.get('psreflect');
  const reflectionBind = device.createBindGroup({ layout: reflected.getBindGroupLayout(0), entries: [
    { binding: 0, resource: { buffer: uniform } }, { binding: 1, resource: source.createView() }, { binding: 2, resource: sampler },
    { binding: 7, resource: cube.createView({ dimension: 'cube' }) }, { binding: 8, resource: sampler }
  ] });
  for (let n = 0; n < 3; ++n) {
    vertices.set([1, 0, 0, 0], n * 64 + 40);
    vertices.set([0, 1, 0, 0], n * 64 + 44);
    vertices.set([0, 0, 1, 1], n * 64 + 48);
  }
  const reflectionPixel = await render(reflected, reflectionBind);
  if (reflectionPixel.some((channel, n) => channel !== faceColors[1][n])) { throw new Error('signed dot reflection selected the wrong face: ' + reflectionPixel); }
  const dotLookup = pipelines.get('psdotst');
  const lookupTexture = device.createTexture({ size: [2, 2], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
  const lookupColors = [faceColors[0], faceColors[1], faceColors[2], faceColors[3]];
  device.queue.writeTexture({ texture: lookupTexture }, new Uint8Array(lookupColors.flat()), { bytesPerRow: 8 }, [2, 2]);
  device.queue.writeTexture({ texture: source }, new Uint8Array([255, 0, 0, 255]), {}, [1, 1]);
  const lookupBinding = device.createBindGroup({ layout: dotLookup.getBindGroupLayout(0), entries: [
    { binding: 0, resource: { buffer: uniform } }, { binding: 1, resource: source.createView() }, { binding: 2, resource: sampler },
    { binding: 7, resource: lookupTexture.createView() }, { binding: 8, resource: sampler }
  ] });
  const dotLookupPixels = [];
  for (let n = 0; n < 4; ++n) {
    const x = n % 2 ? 0.75 : 0.25, y = n < 2 ? 0.25 : 0.75;
    for (let n = 0; n < 3; ++n) { vertices.set([x, 0, 0, 0], n * 64 + 44); vertices.set([y, 0, 0, 0], n * 64 + 48); }
    const value = await render(dotLookup, lookupBinding);
    if (value.some((channel, component) => channel !== lookupColors[n][component])) { throw new Error('dot lookup coordinate mismatch ' + n + ': ' + value); }
    dotLookupPixels.push(value);
  }
  for (const mode of [15, 16]) {
    const pipeline = pipelines.get(`psdep${mode}`);
    const binding = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: uniform } },
      { binding: 5, resource: lookupTexture.createView() }, { binding: 6, resource: sampler }
    ] });
    for (let n = 0; n < 4; ++n) {
      const x = n % 2 ? 0.75 : 0.25, y = n < 2 ? 0.25 : 0.75;
      const coordinate = mode === 15 ? [y, 0.25, 0.75, x] : [0.75, x, y, 0.25];
      for (let n = 0; n < 3; ++n) { vertices.set(coordinate, n * 64 + 36); }
      const value = await render(pipeline, binding);
      if (value.some((channel, component) => channel !== lookupColors[n][component])) {
        throw new Error(`dependent lookup ${mode} selected the wrong texel ${n}: ${value}`);
      }
    }
  }
  lookupTexture.destroy();
  device.queue.writeTexture({ texture: source }, new Uint8Array([64, 128, 192, 255]), {}, [1, 1]);
  cube.destroy();
  constants[772] = 255;
  const alphaDiscard = await render();
  if (alphaDiscard.some(value => value !== 0)) {
    throw new Error(`alpha-test discard failed ${alphaDiscard}`);
  }
  const psAlphaDiscard = await render(pipelines.get('pixel'));
  if (psAlphaDiscard.some(value => value !== 0)) {
    throw new Error(`programmable pixel alpha test did not discard ${psAlphaDiscard}`);
  }
  constants[772] = 128;
  for (let n = 0; n < 3; ++n) {
    vertices[n * 64 + 2] = 2;
  }
  const depthClip = await render();
  if (depthClip.some(value => value !== 0)) {
    throw new Error(`WebGPU clip-depth preservation failed ${depthClip}`);
  }
  for (let n = 0; n < 3; ++n) {
    vertices[n * 64 + 2] = 0.5;
  }
  constants[773] = 7;
  constants.set([0.25, 0.125, 0.75, 0.5], 776);
  device.queue.writeTexture({ texture: source }, new Uint8Array([64, 128, 192, 64]), {}, [1, 1]);
  const combineExpected = [[80, 136, 240, 64], [80, 80, 208, 64], [112, 152, 255, 64], [112, 80, 111, 64], [127, 127, 127, 255]];
  const combinePixels = [];
  for (let n = 0; n < combineExpected.length; ++n) {
    const value = await render(pipelines.get(`combine${n + 18}`));
    if (value.some((channel, index) => channel !== combineExpected[n][index])) {
      throw new Error(`combine${n + 18} pixel ${value}, expected ${combineExpected[n]}`);
    }
    combinePixels.push(value);
  }
  const muxLowPixel = await render(pipelines.get('psmux'));
  if (muxLowPixel.some((channel, n) => channel !== [128, 255, 64, 64][n])) {
    throw new Error(`low alpha mux pixel ${muxLowPixel}`);
  }
  device.queue.writeTexture({ texture: source }, new Uint8Array([64, 128, 192, 0]), {}, [1, 1]);
  const psAlphaKill = await render(pipelines.get('pskill'));
  if (psAlphaKill.some(value => value !== 0)) {
    throw new Error(`programmable pixel alpha kill did not discard ${psAlphaKill}`);
  }
  if (errors.length) {
    throw new Error(errors.join('\n'));
  }
  device.destroy();
  return { compiled: names.length, dotLookupPixels, shadowPixels, reflectionPixel, normalizationPixels, plainReciprocalPixels, depthOnlyPixels, depthBiasPixels, borderPixels, borderMipPixels, pixel, flatPixel, programPixel, registerPixels, macPixels, reciprocalPixels, psPixels, muxLowPixel, psAlphaDiscard, psAlphaKill, alphaDiscard, depthClip, combinePixels, result: 'menu-shaders-browser-ok' };
};

window.menuShaderResult = run().then(result => {
  document.body.textContent = JSON.stringify(result);
  return result;
}).catch(error => {
  document.body.textContent = String(error.stack || error);
  return { error: String(error.stack || error) };
});
