import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createSaveService } from '../../src/browser/save-service.mjs';

const fakeWorker = ({ open = { state: 'empty', sequence: 0 } } = {}) => {
  const received = [];
  const held = [];
  let hold = false;
  const worker = {
    received,
    onmessage: undefined,
    hold: value => { hold = value; },
    release: () => { for (const reply of held.splice(0)) { reply(); } },
    failNext: undefined,
    postMessage: ({ id, type, ...input }) => {
      received.push({ type, ...input });
      const reply = () => {
        const failure = type === 'commit' && worker.failNext;
        if (failure) { worker.failNext = undefined; }
        const result = type === 'open' ? open : type === 'commit' ? { sequence: received.filter(entry => entry.type === 'commit').length, savedAt: '2026-11-07T12:00:00.000Z' } : {};
        queueMicrotask(() => worker.onmessage({ data: failure ? { id, ok: false, error: failure } : { id, ok: true, result } }));
      };
      if (hold && type === 'commit') { held.push(reply); } else { reply(); }
    },
    terminate: () => {},
  };
  return worker;
};
const flush = () => new Promise(resolve => setTimeout(resolve, 0));
const image = marker => Uint8Array.of(marker);

test('the newest published image wins while a write is in flight', async () => {
  const worker = fakeWorker();
  const changes = [];
  const saves = createSaveService({ createWorker: () => worker, onChange: state => changes.push(state.activity) });
  assert.equal((await saves.loaded).state, 'empty');
  saves.pending(true);
  assert.equal(saves.unsaved, true);
  worker.hold(true);
  saves.commit(image(1));
  saves.pending(false);
  await flush();
  saves.commit(image(2));
  saves.commit(image(3));
  assert.equal(saves.state.activity, 'saving');
  assert.equal(saves.unsaved, true);
  worker.hold(false);
  worker.release();
  await saves.settled();
  assert.deepEqual(worker.received.filter(entry => entry.type === 'commit').map(entry => entry.image[0]), [1, 3]);
  assert.equal(saves.state.activity, 'saved');
  assert.equal(saves.state.savedAt, '2026-11-07T12:00:00.000Z');
  assert.equal(saves.unsaved, false);
  assert.equal(saves.state.store, 'loaded');
  assert.deepEqual(changes.filter((value, n) => value !== changes[n - 1]), ['idle', 'pending', 'saving', 'saved']);
});

test('a failed write stays visible and unsaved until a later write succeeds', async () => {
  const worker = fakeWorker({ open: { state: 'loaded', sequence: 4, savedAt: '2026-11-06T10:00:00.000Z' } });
  const saves = createSaveService({ createWorker: () => worker });
  await saves.loaded;
  assert.equal(saves.state.savedAt, '2026-11-06T10:00:00.000Z');
  worker.failNext = { code: 'QUOTA', message: 'Browser storage is full.' };
  saves.commit(image(1));
  await saves.settled();
  assert.equal(saves.state.activity, 'failed');
  assert.equal(saves.state.error.code, 'QUOTA');
  assert.equal(saves.state.savedAt, '2026-11-06T10:00:00.000Z');
  assert.equal(saves.unsaved, true);
  saves.commit(image(2));
  await saves.settled();
  assert.equal(saves.state.activity, 'saved');
  assert.equal(saves.state.error, undefined);
  assert.equal(saves.unsaved, false);
});

test('a store that cannot save ignores published images and reports why', async () => {
  for (const state of ['locked', 'incompatible', 'unavailable']) {
    const worker = fakeWorker({ open: { state, message: `${state} message` } });
    const saves = createSaveService({ createWorker: () => worker });
    const loaded = await saves.loaded;
    assert.equal(loaded.payload, undefined);
    assert.equal(saves.state.store, state);
    assert.equal(saves.state.message, `${state} message`);
    saves.pending(true);
    saves.commit(image(1));
    await saves.settled();
    assert.equal(worker.received.filter(entry => entry.type === 'commit').length, 0, state);
    assert.equal(saves.enabled, false);
    assert.equal(saves.unsaved, false);
  }
});

test('settled resolves once nothing is pending or times out', async () => {
  const worker = fakeWorker();
  const saves = createSaveService({ createWorker: () => worker });
  await saves.loaded;
  saves.pending(true);
  assert.equal(await saves.settled(20), false);
  saves.pending(false);
  assert.equal(await saves.settled(20), true);
});

test('the next start boots from the newest accepted image and a stopped runtime reports lost changes', async () => {
  const worker = fakeWorker({ open: { state: 'loaded', sequence: 1, payload: image(9) } });
  const saves = createSaveService({ createWorker: () => worker });
  await saves.loaded;
  assert.deepEqual(saves.accepted, image(9));
  worker.failNext = { code: 'IO', message: 'failed' };
  saves.commit(image(1));
  await saves.settled();
  assert.deepEqual(saves.accepted, image(9));
  saves.commit(image(2));
  await saves.settled();
  assert.deepEqual(saves.accepted, image(2));
  saves.stopped();
  assert.equal(saves.state.activity, 'saved');
  saves.pending(true);
  saves.stopped();
  assert.equal(saves.state.activity, 'failed');
  assert.equal(saves.state.error.code, 'STOPPED');
  assert.equal(await saves.settled(20), true);
});
