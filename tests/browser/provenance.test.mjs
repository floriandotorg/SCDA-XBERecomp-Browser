import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { verifyGeneration, generationInputs, generationOutputs, imageGeneration, recordGeneration } from '../../tools/browser/provenance.mjs';

const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
const fixture = () => {
  const files = new Map([...generationInputs, ...generationOutputs, 'assets/default.xbe'].map((path) => [path, Buffer.from(path)]));
  const record = {
    schema: 1, image: 'default.xbe', xbeSha256: hash(files.get('assets/default.xbe')),
    xbeSize: files.get('assets/default.xbe').length,
    inputs: Object.fromEntries(generationInputs.map((path) => [path, hash(files.get(path))])),
    comparison: generationOutputs.map((path) => ({ name: path.split('/').at(-1), retained: hash(files.get(path)), regenerated: hash(files.get(path)) })),
  };
  const read = (path) => {
    if (!files.has(path)) { throw new Error('Missing input'); }
    return files.get(path);
  };
  return { files, record, read };
};

test('generation proof binds executable, generator inputs, every retained output and approved policy', () => {
  const { record, read } = fixture();
  const manifest = verifyGeneration(record, read);
  assert.equal(manifest.xbeSha256, record.xbeSha256);
  assert.equal(manifest.xbeSize, record.xbeSize);
  assert.equal(manifest.image, 'default.xbe');
  assert.equal(manifest.requiredStoryFiles.length, 2);
  assert.equal(manifest.storyPolicy, 'scda-story-metadata-v1');
  record.comparison[0].retained = '0'.repeat(64);
  assert.equal(manifest.xbeSha256, hash(read('assets/default.xbe')));
});

test('missing, duplicate, extra and mismatching generation evidence fail closed', () => {
  const mutations = [
    (record) => { record.schema = 2; },
    (record) => { record.image = 'sc4_adv.xbe'; },
    (record) => { record.xbeSha256 = null; },
    (record) => { ++record.xbeSize; },
    (record) => { delete record.inputs[generationInputs[0]]; },
    (record) => { record.inputs[generationInputs[0]] = '0'.repeat(64); },
    (record) => { record.comparison.pop(); },
    (record) => { record.comparison.push(record.comparison[0]); },
    (record) => { record.comparison[0] = record.comparison[1]; },
    (record) => { record.comparison[0].name = '../x86_recomp.0.cpp'; },
    (record) => { record.comparison[0].regenerated = '0'.repeat(64); },
  ];
  for (const mutate of mutations) {
    const { record, read } = fixture();
    mutate(record);
    assert.throws(() => verifyGeneration(record, read), /generation/i);
  }
  assert.throws(() => verifyGeneration(null, () => { throw new Error('Unexpected I/O'); }), /generation/i);
});

test('a proof cannot authorize changed executable bytes, generated partitions or generator sources', () => {
  for (const path of ['assets/default.xbe', generationOutputs[50], generationInputs[3]]) {
    const { record, files, read } = fixture();
    files.set(path, Buffer.from('changed'));
    assert.throws(() => verifyGeneration(record, read), /generation/i);
  }
});

test('a versus record binds sc4_adv.xbe, its own config and 64 partitions, and names the versus disc files', () => {
  const { inputs, outputs } = imageGeneration('sc4_adv.xbe');
  assert.ok(inputs.includes('config/sc4_adv.toml') && inputs.includes('config/xdk_symbols_adv.toml') && !inputs.includes('config/scda_v2.toml'));
  assert.equal(outputs.filter((path) => /x86_recomp\.\d+\.cpp$/.test(path)).length, 64);
  assert.ok(outputs.every((path) => path.startsWith('generated/versus/')));
  const files = new Map([...inputs, ...outputs, 'assets/sc4_adv.xbe'].map((path) => [path, Buffer.from(path)]));
  const read = (path) => files.get(path) ?? (() => { throw new Error('Missing input'); })();
  const record = recordGeneration('sc4_adv.xbe', read, (name) => files.get(`generated/versus/${name}`));
  const manifest = verifyGeneration(record, read);
  assert.equal(manifest.image, 'sc4_adv.xbe');
  assert.ok(manifest.requiredStoryFiles.some((file) => file.path === 'adv/System/Entry.lin'));
  files.set('generated/versus/x86_recomp.63.cpp', Buffer.from('changed'));
  assert.throws(() => verifyGeneration(record, read), /generation/i);
  assert.throws(() => recordGeneration('sc4_adv.xbe', read, (name) => name === 'x86_recomp.5.cpp' ? Buffer.from('other') : files.get(`generated/versus/${name}`)), /reproduce/);
});
