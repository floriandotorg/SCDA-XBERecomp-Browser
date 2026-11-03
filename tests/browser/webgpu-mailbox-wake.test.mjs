import test from 'node:test';
import assert from 'node:assert/strict';
import { createMailbox, closeMailbox, waitForMailboxRequest, MailboxClient, serveMailbox } from '../../src/browser/webgpu-transport.mjs';

test('GPU service wakes on publication without scheduling a polling timer', async () => {
  const mailbox = createMailbox();
  const header = new Int32Array(mailbox, 0, 16);
  const original = globalThis.setTimeout;
  globalThis.setTimeout = () => { throw new Error('GPU request wait scheduled a polling timer'); };
  try {
    const ready = waitForMailboxRequest(mailbox);
    Atomics.store(header, 0, 1);
    Atomics.notify(header, 0);
    assert.equal(await ready, true);
    assert.equal(await waitForMailboxRequest(mailbox), true);
  } finally {
    globalThis.setTimeout = original;
  }
});

test('GPU service survives response acknowledgement and wakes on closure', async () => {
  const mailbox = createMailbox();
  const header = new Int32Array(mailbox, 0, 16);
  Atomics.store(header, 0, 3);
  const ready = waitForMailboxRequest(mailbox);
  Atomics.store(header, 0, 0);
  Atomics.notify(header, 0);
  await new Promise(resolve => setImmediate(resolve));
  closeMailbox(mailbox);
  assert.equal(await ready, false);
  assert.equal(await waitForMailboxRequest(mailbox), false);
});

test('GPU service supports browsers without waitAsync', async () => {
  const mailbox = createMailbox();
  const header = new Int32Array(mailbox, 0, 16);
  const original = Atomics.waitAsync;
  Atomics.waitAsync = undefined;
  try {
    const ready = waitForMailboxRequest(mailbox);
    Atomics.store(header, 0, 1);
    assert.equal(await ready, true);
  } finally {
    Atomics.waitAsync = original;
  }
});

test('GPU client wakes on completion without scheduling a polling timer', async () => {
  const mailbox = createMailbox();
  const original = globalThis.setTimeout;
  globalThis.setTimeout = () => { throw new Error('GPU client scheduled a polling timer'); };
  try {
    const reply = new MailboxClient(mailbox).execute(7);
    await serveMailbox(mailbox, async () => ({ status: 0 }));
    assert.equal((await reply).status, 0);
  } finally {
    globalThis.setTimeout = original;
  }
});
