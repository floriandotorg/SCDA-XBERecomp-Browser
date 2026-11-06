globalThis.addEventListener?.('message', event => {
  const request = event.data;
  if (request?.type !== 'browser-webgpu-mailbox' || !(request.mailbox instanceof SharedArrayBuffer) ||
      request.mailbox.byteLength !== 4194368 * 5 + 1024 + 16777216 || request.mailbox.growable) { return; }
  if (Module.webgpuMailbox && Module.webgpuMailbox !== request.mailbox) { throw new Error('WebGPU worker mailbox ownership changed.'); }
  Module.webgpuMailbox = request.mailbox;
});
const gpuPreRun = Module.preRun ?? [];
Module.preRun = [...(typeof gpuPreRun === 'function' ? [gpuPreRun] : gpuPreRun), () => {
  if (Module.webgpuPthreads === false || !(Module.webgpuMailbox instanceof SharedArrayBuffer)) { return; }
  const bind = worker => worker.postMessage({ type: 'browser-webgpu-mailbox', mailbox: Module.webgpuMailbox });
  for (const worker of PThread.unusedWorkers) { bind(worker); }
  const allocate = PThread.allocateUnusedWorker;
  PThread.allocateUnusedWorker = () => {
    const worker = allocate();
    bind(worker);
    return worker;
  };
}];
