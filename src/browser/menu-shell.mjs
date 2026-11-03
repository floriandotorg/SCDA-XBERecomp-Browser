import { mountBootShell } from './boot-shell.mjs';
import { createMenuRuntime } from './menu-runtime.mjs';

export const mountMenuShell = ({ root = document } = {}) => {
  const diagnostic = root.querySelector('#gpu');
  let canvas = root.querySelector('#game');
  if (!diagnostic || !canvas) { throw new Error('Menu canvas and diagnostics are required.'); }
  let evidence;
  let started;
  const update = () => { diagnostic.textContent = JSON.stringify(evidence, null, 2); };
  const shell = mountBootShell({ root, createRuntime: () => createMenuRuntime({
    acquireCanvas: () => {
      if (!globalThis.navigator?.gpu || typeof canvas.transferControlToOffscreen !== 'function') {
        throw Object.assign(new Error('WebGPU and an offscreen canvas are required in this browser.'), { code: 'CAPABILITY' });
      }
      const replacement = canvas.cloneNode(false);
      canvas.replaceWith(replacement);
      canvas = replacement;
      started = performance.now();
      evidence = { renderedFrames: 0, firstPresentedFrameMilliseconds: null, adapter: null, validationErrors: [], resources: null };
      update();
      return canvas.transferControlToOffscreen();
    },
    onAdapter: (details) => { evidence.adapter = details; update(); },
    onPresented: (details) => {
      ++evidence.renderedFrames;
      evidence.resources = details.resources;
      evidence.lastCompletedCommand = details.command;
      if (evidence.firstPresentedFrameMilliseconds === null) {
        evidence.firstPresentedFrameMilliseconds = performance.now() - started;
        canvas.focus({ preventScroll: true });
      }
      if (evidence.renderedFrames === 1 || evidence.renderedFrames % 60 === 0) { update(); }
    },
    onDiagnostic: (details) => {
      if (details.guest) { evidence.guest = details.guest; }
      else {
        evidence.validationErrors.push(details);
        if (evidence.validationErrors.length > 16) { evidence.validationErrors.shift(); }
      }
      update();
    },
  }) });
  return Object.freeze({ controller: shell.controller, dispose: shell.dispose, get evidence() { return evidence; } });
};
