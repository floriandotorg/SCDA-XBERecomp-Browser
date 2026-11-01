import { readIsoRange } from '../../src/browser/iso-mailbox.mjs';

addEventListener('message', ({ data: { port, mailbox, generation } }) => {
  try {
    let bytesRead = 0;
    for (let n = 0; n < 128; ++n) {
      const offset = (n % 8) * 65536 + 13;
      const bytes = readIsoRange(port, mailbox, offset, 65536, n + 1, generation);
      for (let n = 0; n < bytes.length; ++n) {
        if (bytes[n] !== (offset + n) % 251) {
          throw new Error('Synthetic bytes differ from the independent pattern.');
        }
      }
      bytesRead += bytes.length;
    }
    const eof = readIsoRange(port, mailbox, 1024 * 1024, 0, 129, generation);
    if (eof.length !== 0) {
      throw new Error('End of file returned bytes.');
    }
    postMessage({ ok: true, bytesRead });
  } catch (error) {
    postMessage({ ok: false, code: error.code, message: error.message });
  } finally {
    port.close();
  }
});
