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
  const names = ['plain', 'textured', 'flat', 'stages', 'program', 'paired', 'relative', 'rcc', 'pixel', 'pskill', 'pixel-program', 'psconstants', 'psmux', 'pssimultaneous', ...Array.from({ length: 8 }, (_, n) => `psinput${n}`), ...[0, 8, 16, 24, 32, 48].map(n => `psoutput${n}`), ...Array.from({ length: 13 }, (_, n) => `mac${n + 1}`), ...Array.from({ length: 23 }, (_, n) => n + 2).filter(n => n !== 17).map(n => `op${n}`), ...Array.from({ length: 5 }, (_, n) => `combine${n + 18}`)];
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
    const regs = name.startsWith('op') || name === 'stages' ? Array.from({ length: 16 }, (_, n) => n) : [0, 3, 9];
    device.pushErrorScope('validation');
    const pipeline = await device.createRenderPipelineAsync({
      layout: 'auto',
      vertex: { module: vertex, entryPoint: 'vs_main', buffers: [{ arrayStride: 256, attributes: regs.map(n => ({ shaderLocation: n, offset: n * 16, format: 'float32x4' })) }] },
      fragment: { module: fragment, entryPoint: 'fs_main', targets: [{ format: 'rgba8unorm' }] },
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
  const uniform = device.createBuffer({ size: 3792, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const constants = new Float32Array(await (await fetch(root + 'constants.bin')).arrayBuffer());
  device.queue.writeBuffer(uniform, 0, constants);
  const source = device.createTexture({ size: [1, 1], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
  device.queue.writeTexture({ texture: source }, new Uint8Array([64, 128, 192, 255]), {}, [1, 1]);
  const sampler = device.createSampler({ magFilter: 'nearest', minFilter: 'nearest' });
  const bind = pipeline => device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [
    { binding: 0, resource: { buffer: uniform } },
    { binding: 1, resource: source.createView() },
    { binding: 2, resource: sampler }
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
  const render = async (pipeline = textured) => {
    device.queue.writeBuffer(vertexBuffer, 0, vertices);
    device.queue.writeBuffer(uniform, 0, constants);
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({ colorAttachments: [{ view: target.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }] });
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, bind(pipeline));
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
  const pixel = await render();
  if (pixel.some((value, n) => value !== [32, 128, 48, 255][n])) {
    throw new Error(`texture modulate alpha/UV/clip fixture pixel ${pixel}`);
  }
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
  const reciprocalVertex = device.createShaderModule({ code: await text('rcc.vs.wgsl') });
  const reciprocalFragment = device.createShaderModule({ code: '@fragment fn fs_main(@location(2) value: vec4<f32>) -> @location(0) vec4<f32> { return value; }' });
  const reciprocalPipeline = await device.createRenderPipelineAsync({
    layout: 'auto',
    vertex: { module: reciprocalVertex, entryPoint: 'vs_main', buffers: [{ arrayStride: 256, attributes: [0, 3, 9].map(n => ({ shaderLocation: n, offset: n * 16, format: 'float32x4' })) }] },
    fragment: { module: reciprocalFragment, entryPoint: 'fs_main', targets: [{ format: 'rgba32float' }] },
    primitive: { topology: 'triangle-list' }
  });
  const reciprocalBind = device.createBindGroup({ layout: reciprocalPipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: uniform } }] });
  const reciprocalTarget = device.createTexture({ size: [4, 4], format: 'rgba32float', usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.COPY_SRC });
  const reciprocalPixels = [];
  for (const value of [2, -2, 0, -0, 1e-30, -1e-30, 1e30, -1e30, 1, -1]) {
    for (let n = 0; n < 3; ++n) {
      vertices[n * 64 + 13] = value;
    }
    device.queue.writeBuffer(vertexBuffer, 0, vertices);
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({ colorAttachments: [{ view: reciprocalTarget.createView(), loadOp: 'clear', storeOp: 'store' }] });
    pass.setPipeline(reciprocalPipeline);
    pass.setBindGroup(0, reciprocalBind);
    pass.setVertexBuffer(0, vertexBuffer);
    pass.draw(3);
    pass.end();
    encoder.copyTextureToBuffer({ texture: reciprocalTarget }, { buffer: readback, bytesPerRow: 256 }, [4, 4]);
    device.queue.submit([encoder.finish()]);
    await readback.mapAsync(GPUMapMode.READ);
    const pixel = [...new Float32Array(readback.getMappedRange()).slice(0, 4)];
    readback.unmap();
    const input = Math.fround(value);
    const inverse = Math.fround(1 / input);
    const expected = Math.fround(Math.min(Math.max(Math.abs(inverse), Math.fround(5.42101e-20)), Math.fround(1.84467e19))) * (inverse < 0 ? -1 : 1);
    if (pixel.some(channel => !Number.isFinite(channel) || Math.abs((channel - expected) / expected) > 0.000001)) {
      throw new Error(`RCC input ${Object.is(value, -0) ? '-0' : value} returned ${pixel}, expected ${expected}`);
    }
    reciprocalPixels.push({ input: Object.is(value, -0) ? '-0' : value, pixel });
  }
  for (let n = 0; n < 3; ++n) {
    vertices[n * 64 + 13] = 1;
  }
  const psExpected = new Map([
    ['pixel', [32, 128, 48, 255]], ['pixel-program', [32, 128, 48, 255]],
    ['psconstants', [12, 21, 32, 80]], ['psmux', [64, 128, 192, 255]], ['pssimultaneous', [255, 255, 255, 64]],
    ...[[128, 255, 64, 255], [128, 0, 191, 0], [0, 255, 0, 255], [0, 0, 128, 0], [0, 128, 0, 128], [0, 0, 64, 0], [128, 255, 64, 255], [0, 0, 0, 0]].map((pixel, n) => [`psinput${n}`, pixel]),
    ...[0, 8, 16, 24, 32, 48].map((mapping, n) => [`psoutput${mapping}`, [[128, 255, 64, 255], [0, 128, 0, 128], [255, 255, 128, 255], [0, 255, 0, 255], [255, 255, 255, 255], [64, 128, 32, 128]][n]])
  ]);
  const psPixels = {};
  for (const [name, expected] of psExpected) {
    const value = await render(pipelines.get(name));
    if (value.some((channel, n) => channel !== expected[n])) {
      throw new Error(`${name} pixel ${value}, expected ${expected}`);
    }
    psPixels[name] = value;
  }
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
  return { compiled: names.length, pixel, flatPixel, programPixel, registerPixels, macPixels, reciprocalPixels, psPixels, muxLowPixel, psAlphaDiscard, psAlphaKill, alphaDiscard, depthClip, combinePixels, result: 'menu-shaders-browser-ok' };
};

window.menuShaderResult = run().then(result => {
  document.body.textContent = JSON.stringify(result);
  return result;
}).catch(error => {
  document.body.textContent = String(error.stack || error);
  return { error: String(error.stack || error) };
});
