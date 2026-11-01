import createModule from '../../out/browser-webgpu-tests/webgpu-service.mjs';
export default async options => {
  const module = await createModule(options);
  globalThis.webgpuTestModule = module;
  return module;
};
