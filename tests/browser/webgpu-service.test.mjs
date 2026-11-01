import test from 'node:test';
import assert from 'node:assert/strict';
import { createMailbox, MailboxClient, serveMailbox, Status, Opcode, MAX_BYTES } from '../../src/browser/webgpu-transport.mjs';

test('owned bounded packets retain command/resource IDs and copy replies', async () => {
  const mailbox = createMailbox();
  const client = new MailboxClient(mailbox);
  const input = new Uint8Array([1, 2, 3]);
  const result = client.execute(Opcode.Flush, 17, input);
  input.fill(9);
  await serveMailbox(mailbox, async packet => {
    assert.equal(packet.command, 1);
    assert.equal(packet.resource, 17);
    assert.deepEqual([...packet.bytes], [1, 2, 3]);
    return { status: Status.Ok, bytes: packet.bytes };
  });
  assert.deepEqual([...(await result).bytes], [1, 2, 3]);
  await assert.rejects(client.execute(Opcode.Flush, 0, new Uint8Array(MAX_BYTES + 1)), /bounded/);
});

test('busy mailbox rejects overlap and shutdown resolves blocked requests', async () => {
  const mailbox = createMailbox();
  const client = new MailboxClient(mailbox);
  const pending = client.execute(Opcode.Flush, 23);
  await assert.rejects(client.execute(Opcode.Flush, 24), /busy/);
  client.close();
  assert.equal((await pending).status, Status.Closed);
  await assert.rejects(client.execute(Opcode.Flush, 25), /closed/);
});

test('exceptions and oversized responses become attributed bounded errors', async () => {
  for (let n = 0; n < 2; ++n) {
    const mailbox = createMailbox();
    const client = new MailboxClient(mailbox);
    const pending = client.execute(Opcode.Readback, 51);
    await serveMailbox(mailbox, async () => {
      if (n === 0) {
        throw new Error('validation');
      }
      return { status: Status.Ok, bytes: new Uint8Array(MAX_BYTES + 1) };
    });
    const result = await pending;
    assert.equal(result.status, Status.Error);
    assert.equal(result.resource, 51);
    assert.equal(result.command, 1);
  }
});

test('late completion after shutdown cannot reopen mailbox', async () => {
  const mailbox = createMailbox();
  const client = new MailboxClient(mailbox);
  const pending = client.execute(Opcode.Flush, 0);
  let finish;
  const service = serveMailbox(mailbox, () => new Promise(resolve => { finish = resolve; }));
  client.close();
  finish({ status: Status.Ok });
  await service;
  assert.equal((await pending).status, Status.Closed);
});
