import { test } from 'node:test';
import assert from 'node:assert/strict';
import { cachedDisc, createDiscCache, prepareDisc } from '../../src/browser/disc-cache.mjs';

const directory = () => {
  const files = new Map();
  const directories = new Map();
  let failure;
  let copies = 0;
  return {
    files,
    fail: stage => { failure = stage; },
    copies: () => copies,
    getDirectoryHandle: async (name, options = {}) => {
      if (!directories.has(name)) {
        if (!options.create) { throw new DOMException('Missing', 'NotFoundError'); }
        directories.set(name, directory());
      }
      return directories.get(name);
    },
    getFileHandle: async (name, options = {}) => {
      if (!files.has(name)) {
        if (!options.create) { throw new DOMException('Missing', 'NotFoundError'); }
        files.set(name, new File([], name));
      }
      return {
        getFile: async () => files.get(name),
        createWritable: async () => {
          if (failure === 'open') { throw new DOMException('Locked', 'NoModificationAllowedError'); }
          const chunks = [];
          const stream = new WritableStream({
            write: chunk => {
              if (name === 'disc.iso' && failure === 'write') { throw new DOMException('Full', 'QuotaExceededError'); }
              chunks.push(chunk);
            },
            close: () => {
              if (name === 'disc.iso' && failure === 'close') { throw new Error('Interrupted copy'); }
              if (name === 'disc-id.json' && chunks[0] !== 'null' && failure === 'metadata') { throw new Error('Metadata interrupted'); }
              files.set(name, new File(chunks, name));
              if (name === 'disc.iso') { ++copies; }
            },
          });
          stream.write = async value => {
            const writer = stream.getWriter();
            await writer.write(value);
            writer.releaseLock();
          };
          stream.close = async () => { await stream.getWriter().close(); };
          return stream;
        },
      };
    },
  };
};
const disc = new File(['disc content'], 'game.iso', { lastModified: 100 });
const estimate = { quota: 1000, usage: 0 };

test('a disc is copied once and reused while it is unchanged', async () => {
  const root = directory();
  const reported = [];
  const copied = await prepareDisc(root, disc, { estimate, progress: (done, total) => { reported.push([done, total]); } });
  assert.equal(await copied.text(), 'disc content');
  assert.deepEqual(reported.at(-1), [disc.size, disc.size]);
  assert.equal(await (await prepareDisc(root, disc, { estimate })).text(), 'disc content');
  assert.equal(root.copies(), 1);
  await prepareDisc(root, new File(['other disc'], 'game.iso', { lastModified: 200 }), { estimate });
  assert.equal(root.copies(), 2);
});

test('an interrupted copy is never offered and is copied again', async () => {
  for (const stage of ['write', 'close', 'metadata']) {
    const root = directory();
    await prepareDisc(root, disc, { estimate });
    root.fail(stage);
    await assert.rejects(() => prepareDisc(root, new File(['changed disc'], 'game.iso', { lastModified: 300 }), { estimate }));
    root.fail(undefined);
    assert.equal(await root.files.get('disc-id.json').text(), 'null');
    assert.equal(await cachedDisc(root), null);
    const before = root.copies();
    await prepareDisc(root, disc, { estimate });
    assert.equal(root.copies(), before + 1);
  }
});

test('a copy that does not fit the remaining storage is refused before writing', async () => {
  const root = directory();
  await assert.rejects(() => prepareDisc(root, disc, { estimate: { quota: 10, usage: 5 } }), /storage/i);
  assert.equal(root.files.size, 0);
  await prepareDisc(root, disc, { estimate });
  assert.equal(await (await prepareDisc(root, disc, { estimate: { quota: 10, usage: 10 } })).text(), 'disc content');
});

test('only a complete copy is offered for starting without the original file', async () => {
  const root = directory();
  assert.equal(await cachedDisc(root), null);
  await prepareDisc(root, disc, { estimate });
  assert.equal(await (await cachedDisc(root)).text(), 'disc content');
  root.files.set('disc-id.json', new File(['{broken'], 'disc-id.json'));
  assert.equal(await cachedDisc(root), null);
  root.files.set('disc-id.json', new File([JSON.stringify({ name: 'game.iso', size: 3, modified: 100 })], 'disc-id.json'));
  assert.equal(await cachedDisc(root), null);
});

test('the cache keeps its copy apart from saved progress and asks to keep the storage', async () => {
  const root = directory();
  let persisted = 0;
  const storage = { getDirectory: async () => root, estimate: async () => estimate, persist: async () => { ++persisted; return true; } };
  const cache = createDiscCache({ storage });
  assert.equal(await cache.cached(), null);
  assert.equal(await (await cache.store(disc)).text(), 'disc content');
  assert.equal(persisted, 1);
  assert.equal(root.files.size, 0);
  assert.equal(await (await cache.cached()).text(), 'disc content');
});

test('a browser without private file storage keeps using the original file', async () => {
  const cache = createDiscCache({ storage: undefined });
  assert.equal(await cache.cached(), null);
  assert.equal(await cache.store(disc), disc);
});
