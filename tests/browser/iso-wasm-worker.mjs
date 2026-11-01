import { runIsoWasmGuest } from './iso-wasm-guest.mjs';

self.onmessage = async ({ data }) => {
  try {
    self.postMessage(await runIsoWasmGuest(data));
  } catch (error) {
    self.postMessage({ ok: false, message: error.message });
  }
};
