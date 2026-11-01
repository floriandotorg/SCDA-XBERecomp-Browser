import test from 'node:test';
import assert from 'node:assert/strict';
import { createMailbox, MailboxClient, serveMailbox, closeMailbox, Status, Opcode } from '../../src/browser/webgpu-transport.mjs';

test('shutdown during response copy cannot publish stale success', async () => {
  const mailbox = createMailbox();
  const client = new MailboxClient(mailbox);
  const pending = client.execute(Opcode.Flush);
  const original = Uint8Array.prototype.set;
  Uint8Array.prototype.set = function (source, offset) {
    if (source.length === 1 && source[0] === 123) { closeMailbox(mailbox); }
    return original.call(this, source, offset);
  };
  try {
    await serveMailbox(mailbox, async () => ({ status: Status.Ok, bytes: new Uint8Array([123]) }));
    assert.equal((await pending).status, Status.Closed);
    assert.equal(new Int32Array(mailbox)[0], 4);
  } finally { Uint8Array.prototype.set = original; }
});

test('shutdown during request copy cannot publish new work', async () => {
  const mailbox = createMailbox();
  const client = new MailboxClient(mailbox, { timeoutMs: 10 });
  const original = Uint8Array.prototype.set;
  Uint8Array.prototype.set = function (source, offset) {
    if (source.length === 1 && source[0] === 124) { closeMailbox(mailbox); }
    return original.call(this, source, offset);
  };
  try {
    const pending = client.execute(Opcode.Flush, 0, new Uint8Array([124]));
    assert.equal(new Int32Array(mailbox)[0], 4);
    assert.equal((await pending).status, Status.Closed);
    assert.equal(new Int32Array(mailbox)[0], 4);
  } finally { Uint8Array.prototype.set = original; }
});
