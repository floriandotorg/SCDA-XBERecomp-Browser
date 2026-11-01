import { createGPUService } from '../../src/browser/webgpu-service.mjs';
import { Opcode, Status } from '../../src/browser/webgpu-transport.mjs';
globalThis.onmessage = async () => {
  let service;
  try {
    let resolveLost;
    const loss = new Promise(resolve => { resolveLost = resolve; });
    service = await createGPUService({ canvas: new OffscreenCanvas(4, 4), moduleURL: new URL('./webgpu-service-loss-module.mjs', import.meta.url).href, diagnostic: value => { if (value.status === Status.Lost) { resolveLost(value); } } });
    const pending = service.execute({ opcode: Opcode.Flush, command: 91, resource: 0, bytes: new Uint8Array() });
    globalThis.webgpuTestModule._webgpu_test_loss();
    const [completion, diagnostic] = await Promise.all([pending, loss]);
    const afterLoss = await service.execute({ opcode: Opcode.Flush, command: 92, resource: 0, bytes: new Uint8Array() });
    service.shutdown();
    globalThis.postMessage({ ok: afterLoss.status === Status.Lost && service.resources() === 0, pendingStatus: completion.status, afterLoss: afterLoss.status, diagnostic, resources: service.resources() });
  } catch (error) { service?.shutdown(); globalThis.postMessage({ ok: false, error: String(error) }); }
};
