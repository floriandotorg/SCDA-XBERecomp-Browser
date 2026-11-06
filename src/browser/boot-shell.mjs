import { createBootController } from './boot-controller.mjs';
import { createBootRuntime } from './boot-runtime.mjs';
import { assertWasmCapabilities } from './wasm-capabilities.mjs';

const phaseText = Object.freeze({
  selection: 'Select an ISO to begin.',
  selected: 'Local ISO selected. Ready to validate.',
  validating: 'Preparing the runtime and validating the selected disc against this build.',
  booting: 'Disc authenticated. Binding exports and entering the guest.',
  running: 'Guest runtime active. Full game boot has not been verified.',
  stopped: 'Stopped. The selected ISO can be reused.',
  error: 'Boot failed.',
});

const loadBootManifest = async () => (await import('./boot-manifest.mjs')).default;

export const createShellController = ({ createRuntime = createBootRuntime, loadManifest = loadBootManifest, validateWasm, onChange }) => createBootController({
  createRuntime,
  validateSelection: async (source, { signal }) => {
    signal.throwIfAborted();
    if (!globalThis.crossOriginIsolated || typeof SharedArrayBuffer !== 'function' || typeof Worker !== 'function') {
      throw Object.assign(new Error('A secure context with cross origin isolation and browser workers is required.'), { code: 'CAPABILITY' });
    }
    if (!Number.isSafeInteger(source.size) || source.size < 1 || typeof source.slice !== 'function') {
      throw Object.assign(new Error('Select a nonempty local ISO.'), { code: 'FILE' });
    }
    const manifest = await loadManifest();
    signal.throwIfAborted();
    assertWasmCapabilities(manifest, validateWasm);
  },
  onChange,
});

export const mountBootShell = ({ root = document, createRuntime = createBootRuntime,
  loadManifest = loadBootManifest, validateWasm } = {}) => {
  const input = root.querySelector('#iso');
  const boot = root.querySelector('#boot');
  const stop = root.querySelector('#stop');
  const status = root.querySelector('#status');
  const detail = root.querySelector('#detail');
  const selected = root.querySelector('#selected');
  if (!input || !boot || !stop || !status || !detail || !selected) {
    throw new Error('Boot shell controls are missing.');
  }
  const render = (state) => {
    const active = ['validating', 'booting', 'running'].includes(state.phase);
    boot.disabled = !state.source || active || state.stopping || state.code === 'TEARDOWN';
    stop.disabled = !active || state.stopping;
    input.disabled = state.stopping || state.code === 'TEARDOWN';
    status.textContent = state.stopping ? 'Stopping guest workers and cancelling pending reads.' :
      state.phase === 'stopped' && state.code === 'SINGLE_PLAYER_LAUNCH_REQUEST' ? 'Single player launch boundary reached.' : phaseText[state.phase];
    selected.textContent = state.source ? `${state.source.name ?? 'Local ISO'} (${state.source.size.toLocaleString()} bytes)` : 'No ISO selected.';
    const diagnostic = state.diagnostic;
    const service = diagnostic?.exportName ? `${diagnostic.exportName} (${diagnostic.ordinal}). ` : '';
    const entry = diagnostic?.entryEntered ? diagnostic.entryReturned ? ' The initial guest entry executed and returned.' : ' Guest entry execution started.' : '';
    const thread = diagnostic?.threadId ? ` Guest thread ${diagnostic.threadId}.` : '';
    detail.textContent = state.message ? `${state.code}: ${service}${state.message}${entry}${thread}` : '';
    status.dataset.state = state.phase;
    boot.setAttribute('aria-busy', String(state.phase === 'validating' || state.phase === 'booting' || state.stopping));
  };
  const controller = createShellController({ createRuntime, loadManifest, validateWasm, onChange: render });
  const select = () => { void controller.select(input.files?.[0]); };
  const start = () => { void controller.start(); };
  const stopBoot = () => { void controller.stop(); };
  input.addEventListener('change', select);
  boot.addEventListener('click', start);
  stop.addEventListener('click', stopBoot);
  render(controller.state);
  return Object.freeze({
    controller,
    dispose: async () => {
      input.removeEventListener('change', select);
      boot.removeEventListener('click', start);
      stop.removeEventListener('click', stopBoot);
      await controller.stop();
    },
  });
};
