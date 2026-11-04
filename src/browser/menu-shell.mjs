import { mountBootShell } from './boot-shell.mjs';
import { createMenuRuntime } from './menu-runtime.mjs';
import { createMissionMetrics } from './mission-metrics.mjs';

export const mountMenuShell = ({ root = document } = {}) => {
  const diagnostic = root.querySelector('#gpu');
  let canvas = root.querySelector('#game');
  if (!diagnostic || !canvas) { throw new Error('Menu canvas and diagnostics are required.'); }
  let evidence;
  let started;
  let lastUpdated = 0;
  const measurement = createMissionMetrics();
  const suspend = () => { measurement.suspend(); };
  globalThis.addEventListener('blur', suspend);
  root.addEventListener('visibilitychange', suspend);
  const update = () => { evidence.performance = measurement.snapshot(); diagnostic.textContent = JSON.stringify(evidence, null, 2); };
  const shell = mountBootShell({ root, createRuntime: () => createMenuRuntime({
    developmentStart: root.querySelector('#development-start')?.checked === true,
    acquireCanvas: () => {
      if (!globalThis.navigator?.gpu || typeof canvas.transferControlToOffscreen !== 'function') {
        throw Object.assign(new Error('WebGPU and an offscreen canvas are required in this browser.'), { code: 'CAPABILITY' });
      }
      const replacement = canvas.cloneNode(false);
      canvas.replaceWith(replacement);
      canvas = replacement;
      started = performance.now();
      measurement.begin();
      lastUpdated = started;
      evidence = { developmentStart: root.querySelector('#development-start')?.checked === true,
        renderedFrames: 0, firstPresentedFrameMilliseconds: null, adapter: null, validationErrors: [], resources: null };
      update();
      return canvas.transferControlToOffscreen();
    },
    onAdapter: (details) => { evidence.adapter = details; update(); },
    onPresented: (details) => {
      const time = performance.now();
      measurement.present(time, details);
      ++evidence.renderedFrames;
      evidence.resources = details.resources;
      evidence.lastCompletedCommand = details.command;
      if (evidence.firstPresentedFrameMilliseconds === null) {
        evidence.firstPresentedFrameMilliseconds = performance.now() - started;
        canvas.focus({ preventScroll: true });
      }
      if (evidence.renderedFrames === 1 || time - lastUpdated >= 1000) { lastUpdated = time; update(); }
    },
    onDiagnostic: (details) => {
      if (details.io) {
        evidence.io = details.io;
        const loading = root.querySelector('#loading');
        if (loading) { loading.textContent = `Read ${(details.io.bytesRead / 1048576).toFixed(1)} MiB from the ISO. ${details.io.active} active reads.`; }
      }
      else if (details.guest) { evidence.guest = details.guest; }
      else if (details.runtime) { evidence.runtime = details.runtime; }
      else {
        evidence.validationErrors.push(details);
        if (evidence.validationErrors.length > 16) { evidence.validationErrors.shift(); }
      }
      update();
    },
  }) });
  return Object.freeze({ controller: shell.controller,
    dispose: async () => {
      globalThis.removeEventListener('blur', suspend);
      root.removeEventListener('visibilitychange', suspend);
      await shell.dispose();
    },
    beginMeasurement: () => { measurement.begin(); },
    get measurement() { return measurement.snapshot(); },
    get evidence() { return evidence; },
  });
};
