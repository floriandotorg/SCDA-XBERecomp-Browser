import test from 'node:test';
import assert from 'node:assert/strict';
import { createMailbox, MailboxClient, serveMailbox, Status, Opcode, MAX_BYTES } from '../../src/browser/webgpu-transport.mjs';

test('deadline closes a dead service and wakes the request', async () => {
  const client = new MailboxClient(createMailbox(), { timeoutMs: 10 });
  assert.equal((await client.execute(Opcode.Flush)).status, Status.Closed);
});

test('malformed request lengths never reach the GPU executor', async () => {
  const mailbox = createMailbox();
  const header = new Int32Array(mailbox, 0, 16);
  header[4] = -1;
  header[0] = 1;
  let calls = 0;
  await serveMailbox(mailbox, () => { ++calls; });
  assert.equal(calls, 0);
  assert.equal(header[6], Status.Error);
});

test('malformed replies close mailbox rather than copying invalid bytes', async () => {
  const mailbox = createMailbox();
  const client = new MailboxClient(mailbox);
  const pending = client.execute(Opcode.Flush);
  const header = new Int32Array(mailbox, 0, 16);
  header[5] = MAX_BYTES + 1;
  header[6] = 0;
  Atomics.store(header, 0, 3);
  Atomics.notify(header, 0);
  assert.equal((await pending).status, Status.Error);
});

test('command sequence exhaustion is terminal rather than wrapping', async () => {
  const client = new MailboxClient(createMailbox());
  client.sequence = 0xffffffff;
  await assert.rejects(client.execute(Opcode.Flush), /sequence/);
});
