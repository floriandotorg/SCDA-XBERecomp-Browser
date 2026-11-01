const output = document.querySelector('#output');
const status = document.querySelector('#status');
const placeholder = document.querySelector('#placeholder');
const capabilities = {
  crossOriginIsolated,
  sharedArrayBuffer: typeof SharedArrayBuffer === 'function',
  sharedWasmMemory: false,
  workers: typeof Worker === 'function',
  webgpu: Boolean(navigator.gpu),
  fileAPI: typeof File === 'function' && typeof Blob.prototype.slice === 'function',
  audioWorklet: typeof AudioWorkletNode === 'function',
  wasmExceptionHandling: typeof WebAssembly.Exception === 'function',
  userAgent: navigator.userAgent,
};
try {
  capabilities.sharedWasmMemory = new WebAssembly.Memory({ initial: 1, maximum: 1, shared: true }).buffer instanceof SharedArrayBuffer;
} catch {
  capabilities.sharedWasmMemory = false;
}
document.querySelector('#capabilities').textContent = JSON.stringify(capabilities, null, 2);
const print = (text) => { output.textContent = `${output.textContent}${text}\n`.slice(-12000); };
const metrics = { compileMs: 0, instantiateMs: 0, wasmBytes: 0 };

try {
  const { default: createDiagnostic } = await import('./story_diagnostic.mjs');
  const response = await fetch('./story_diagnostic.wasm');
  if (!response.ok) {
    throw new Error(`Wasm fetch failed: ${response.status}`);
  }
  const bytes = await response.arrayBuffer();
  metrics.wasmBytes = bytes.byteLength;
  const compileStart = performance.now();
  const compiled = await WebAssembly.compile(bytes);
  metrics.compileMs = performance.now() - compileStart;
  const module = await new Promise((resolve, reject) => {
    createDiagnostic({
      print,
      printErr: print,
      instantiateWasm: (imports, receiveInstance) => {
        const instantiateStart = performance.now();
        WebAssembly.instantiate(compiled, imports).then((instance) => {
          metrics.instantiateMs = performance.now() - instantiateStart;
          receiveInstance(instance, compiled);
        }).catch(reject);
        return {};
      },
    }).then(resolve, reject);
  });
  const count = module._diagnostic_mapping_count();
  const firstGuest = module._diagnostic_mapping_guest(0);
  print(`Browser metrics: ${JSON.stringify(metrics)}`);
  status.textContent = 'Diagnostic ready';
  window.diagnostic = { capabilities, metrics, count, firstGuest, ready: true };
  placeholder.disabled = false;
  placeholder.onclick = () => {
    if (module._diagnostic_placeholder() !== 1) {
      status.textContent = 'Placeholder failed to trap';
    }
  };
} catch (error) {
  status.textContent = 'Diagnostic failed';
  print(String(error));
}
