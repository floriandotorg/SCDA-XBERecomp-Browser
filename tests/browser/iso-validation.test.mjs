import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { validateDiscImage } from '../../src/browser/iso-validation.mjs';

const fixture = (length = 19) => {
  const image = Uint8Array.from({ length }, (_, n) => n % 251);
  const files = new Map([
    ['default.xbe', image], ['default/story.dat', new Uint8Array([1, 2, 3])],
  ]);
  const reads = [];
  const manifest = {
    schema: 1, image: 'default.xbe', xbeSize: image.length,
    xbeSha256: createHash('sha256').update(image).digest('hex'),
    requiredStoryFiles: [{ path: 'Default/Story.dat', minBytes: 3 }],
  };
  const disc = {
    find: async (path) => files.has(path) ? { size: files.get(path).length, dir: false } : undefined,
    read: async (path, offset, length) => {
      reads.push({ path, offset, length });
      return files.get(path).slice(offset, offset + length);
    },
  };
  return { disc, manifest, image, files, reads };
};
const code = (expected) => (error) => error.code === expected;

test('disc identity verifies bounded image reads and required story metadata without scanning assets', async () => {
  const source = fixture(2 ** 20 + 13);
  const result = await validateDiscImage(source);
  assert.deepEqual(result.bytes, source.image);
  assert.equal(result.sha256, source.manifest.xbeSha256);
  assert.equal(result.storyFiles, 1);
  assert.deepEqual(source.reads.map(({ offset, length }) => [offset, length]), [[0, 2 ** 20], [2 ** 20, 13]]);
  assert.ok(source.reads.every(({ path }) => path === 'default.xbe'));
});

test('absent trusted provenance blocks validation before any disc access', async () => {
  for (const change of [
    { xbeSha256: null }, { xbeSha256: 'not a digest' }, { xbeSize: null },
    { xbeSize: 64 * 2 ** 20 + 1 }, { xbeSize: 0 }, { xbeSize: 1.5 },
    { requiredStoryFiles: null }, { requiredStoryFiles: [] }, { requiredStoryFiles: new Array(1) },
    { requiredStoryFiles: new Array(257).fill({ path: 'story.dat', minBytes: 1 }) },
    { schema: 2 }, { image: 'sc4_adv.xbe' },
  ]) {
    const source = fixture();
    Object.assign(source.manifest, change);
    source.disc.find = () => assert.fail('untrusted manifest accessed the disc');
    await assert.rejects(validateDiscImage(source), code('MANIFEST'));
  }
});

test('story manifest paths are confined, unique and cannot demand a second executable', async () => {
  for (const path of ['/story', '../story', 'Default/../story', 'D:story', 'Default//story', 'Default/./story', 'a\u0000b', 'sc4_adv.xbe']) {
    const source = fixture();
    source.manifest.requiredStoryFiles = [{ path, minBytes: 1 }];
    await assert.rejects(validateDiscImage(source), code('MANIFEST'));
    assert.equal(source.reads.length, 0);
  }
  const source = fixture();
  source.manifest.requiredStoryFiles.push({ path: 'default\\STORY.dat', minBytes: 1 });
  await assert.rejects(validateDiscImage(source), code('MANIFEST'));
});

test('missing image, directory image, unsupported size and missing story are distinct failures', async () => {
  for (const [file, metadata, expected] of [
    ['default.xbe', undefined, 'MISSING_IMAGE'],
    ['default.xbe', { size: 19, dir: true }, 'MISSING_IMAGE'],
    ['default.xbe', { size: 20, dir: false }, 'IMAGE_SIZE'],
    ['default/story.dat', undefined, 'MISSING_STORY'],
    ['default/story.dat', { size: 0, dir: false }, 'MISSING_STORY'],
    ['default/story.dat', { size: 3, dir: true }, 'MISSING_STORY'],
  ]) {
    const source = fixture();
    const find = source.disc.find;
    source.disc.find = (path) => path === file ? metadata : find(path);
    await assert.rejects(validateDiscImage(source), code(expected));
    assert.equal(source.reads.length, 0);
  }
});

test('wrong executable hash reports expected and observed local identities and never returns bytes', async () => {
  const source = fixture();
  source.manifest.xbeSha256 = '0'.repeat(64);
  await assert.rejects(validateDiscImage(source), (error) => {
    assert.equal(error.code, 'IMAGE_HASH');
    assert.equal(error.expected, '0'.repeat(64));
    assert.equal(error.observed, createHash('sha256').update(source.image).digest('hex'));
    return true;
  });
});

test('short reads and inaccessible files cannot become successful validation', async () => {
  for (const read of [() => new Uint8Array(1), () => undefined, () => { throw new Error('private file path'); }]) {
    const source = fixture();
    source.disc.read = read;
    await assert.rejects(validateDiscImage(source), (error) => {
      assert.ok(['SHORT_READ', 'READ_FAILED'].includes(error.code));
      assert.ok(!error.message.includes('private file path'));
      return true;
    });
  }
});

test('cancellation before or during I/O prevents publication and additional reads', async () => {
  for (const during of [false, true]) {
    const source = fixture(2 ** 20 + 1);
    const controller = new AbortController();
    if (during) {
      const read = source.disc.read;
      source.disc.read = (...arguments_) => {
        controller.abort();
        return read(...arguments_);
      };
    } else {
      controller.abort();
    }
    await assert.rejects(validateDiscImage({ ...source, signal: controller.signal }), code('CANCELLED'));
    assert.equal(source.reads.length, during ? 1 : 0);
  }
});

test('cancellation during hashing cannot publish authenticated image bytes', async (context) => {
  const source = fixture();
  const controller = new AbortController();
  const digest = crypto.subtle.digest.bind(crypto.subtle);
  context.mock.method(crypto.subtle, 'digest', async (...arguments_) => {
    const result = await digest(...arguments_);
    controller.abort();
    return result;
  });
  await assert.rejects(validateDiscImage({ ...source, signal: controller.signal }), code('CANCELLED'));
});

test('validation snapshots the manifest across asynchronous reads', async () => {
  const source = fixture();
  const expected = source.manifest.xbeSha256;
  const read = source.disc.read;
  source.disc.read = (...arguments_) => {
    source.manifest.xbeSha256 = '0'.repeat(64);
    source.manifest.xbeSize = 1;
    return read(...arguments_);
  };
  assert.equal((await validateDiscImage(source)).sha256, expected);
});
