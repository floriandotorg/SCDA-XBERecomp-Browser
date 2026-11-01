import { parentPort } from 'node:worker_threads';
import { runIsoWasmGuest } from './iso-wasm-guest.mjs';

parentPort.once('message', async (message) => {
  try {
    parentPort.postMessage(await runIsoWasmGuest(message));
  } catch (error) {
    parentPort.postMessage({ ok: false, message: error.message });
  }
});
