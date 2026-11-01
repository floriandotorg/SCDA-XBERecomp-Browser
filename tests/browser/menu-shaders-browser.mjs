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
  const names = ['plain', 'textured', 'stages', ...Array.from({ length: 23 }, (_, n) => n + 2).filter(n => n <= 16 || n >= 23).map(n => `op${n}`)];
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
    if (name === 'textured') {
      textured = pipeline;
    }
  }
  const uniform = device.createBuffer({ size: 3504, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
  const constants = new Float32Array(await (await fetch(root + 'constants.bin')).arrayBuffer());
  device.queue.writeBuffer(uniform, 0, constants);
  const source = device.createTexture({ size: [1, 1], format: 'rgba8unorm', usage: GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST });
  device.queue.writeTexture({ texture: source }, new Uint8Array([64, 128, 192, 255]), {}, [1, 1]);
  const sampler = device.createSampler({ magFilter: 'nearest', minFilter: 'nearest' });
  const group = device.createBindGroup({ layout: textured.getBindGroupLayout(0), entries: [
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
  const render = async () => {
    device.queue.writeBuffer(vertexBuffer, 0, vertices);
    device.queue.writeBuffer(uniform, 0, constants);
    const encoder = device.createCommandEncoder();
    const pass = encoder.beginRenderPass({ colorAttachments: [{ view: target.createView(), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }] });
    pass.setPipeline(textured);
    pass.setBindGroup(0, group);
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
  constants[772] = 255;
  const alphaDiscard = await render();
  if (alphaDiscard.some(value => value !== 0)) {
    throw new Error(`alpha-test discard failed ${alphaDiscard}`);
  }
  constants[772] = 128;
  for (let n = 0; n < 3; ++n) {
    vertices[n * 64 + 2] = 2;
  }
  const depthClip = await render();
  if (depthClip.some(value => value !== 0)) {
    throw new Error(`WebGPU clip-depth preservation failed ${depthClip}`);
  }
  if (errors.length) {
    throw new Error(errors.join('\n'));
  }
  device.destroy();
  return { compiled: names.length, pixel, alphaDiscard, depthClip, result: 'menu-shaders-browser-ok' };
};

window.menuShaderResult = run().then(result => {
  document.body.textContent = JSON.stringify(result);
  return result;
}).catch(error => {
  document.body.textContent = String(error.stack || error);
  return { error: String(error.stack || error) };
});
