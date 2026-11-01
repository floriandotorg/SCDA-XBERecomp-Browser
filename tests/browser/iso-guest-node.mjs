import { parentPort } from 'node:worker_threads';
import { readIsoRange } from '../../src/browser/iso-mailbox.mjs';

parentPort.on('message', ({ port, mailbox, generation }) => {
  try {
    const bytes = [...readIsoRange(port, mailbox, 13, 7, 1, generation)];
    let rangeError;
    try {
      readIsoRange(port, mailbox, 255, 4, 2, generation);
    } catch (error) {
      rangeError = error.code;
    }
    parentPort.postMessage({ ok: true, bytes, rangeError });
  } catch (error) {
    parentPort.postMessage({ ok: false, code: error.code, message: error.message });
  } finally {
    port.close();
  }
});
