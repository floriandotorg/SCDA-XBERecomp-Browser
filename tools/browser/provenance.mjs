import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

// the two recompiled images: story (default.xbe) and versus (sc4_adv.xbe), each with its generator configuration,
// generated directory, partition count and the disc files a boot requires
export const IMAGES = Object.freeze({
  'default.xbe': Object.freeze({
    id: 'story', xbe: 'default.xbe', config: 'config/scda_v2.toml', symbols: 'config/xdk_symbols.toml', generated: 'generated/game', partitions: 128,
    policy: 'scda-story-metadata-v1',
    requiredFiles: [
      { path: 'Default/DataXb/LMaps/common.lin', minBytes: 1 },
      { path: 'Default/DataXb/LMaps/Menu/Menu.lin', minBytes: 1 },
    ],
  }),
  'sc4_adv.xbe': Object.freeze({
    id: 'versus', xbe: 'sc4_adv.xbe', config: 'config/sc4_adv.toml', symbols: 'config/xdk_symbols_adv.toml', generated: 'generated/versus', partitions: 64,
    policy: 'scda-versus-metadata-v1',
    requiredFiles: [
      { path: 'adv/System/Entry.lin', minBytes: 1 },
      { path: 'adv/System/Lobby.lin', minBytes: 1 },
    ],
  }),
});
export const imageById = (id) => Object.values(IMAGES).find((image) => image.id === id);

const recompilerSources = ['analysis.cpp', 'analysis.h', 'codegen.cpp', 'codegen.h', 'codegen_emit.cpp', 'codegen_fp.cpp', 'codegen_int.cpp', 'main.cpp'].map((name) => `tools/recomp/${name}`);
const generatedFiles = ['x86_func_mapping.cpp', 'x86_recomp_shared.h', 'image_symbols.h', 'exports.def', 'imports.def', 'x86_recomp_stats.txt'];
export const imageGeneration = (xbe) => {
  const image = IMAGES[xbe];
  if (!image) { throw new Error(`unknown image ${xbe}`); }
  return {
    inputs: ['CMakeLists.txt', image.config, image.symbols, 'src/xbe/xbe.h', ...recompilerSources],
    outputs: [
      ...Array.from({ length: image.partitions }, (_, n) => `${image.generated}/x86_recomp.${n}.cpp`),
      ...generatedFiles.map((name) => `${image.generated}/${name}`),
    ],
  };
};
export const { inputs: generationInputs, outputs: generationOutputs } = imageGeneration('default.xbe');
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
const reject = () => { throw new Error('Trusted generation evidence is absent, incomplete or stale.'); };

export const verifyGeneration = (record, read) => {
  const image = IMAGES[record?.image];
  if (record?.schema !== 1 || !image || !record.inputs || !Array.isArray(record.comparison)) { reject(); }
  const { inputs, outputs } = imageGeneration(image.xbe);
  if (record.comparison.length !== outputs.length) { reject(); }
  const bytes = read(`assets/${image.xbe}`);
  if (record.xbeSize !== bytes.length || record.xbeSha256 !== hash(bytes)) { reject(); }
  for (const path of inputs) {
    if (record.inputs[path] !== hash(read(path))) { reject(); }
  }
  const compared = new Map();
  for (const file of record.comparison) {
    if (!file || typeof file.name !== 'string' || compared.has(file.name)) { reject(); }
    compared.set(file.name, file);
  }
  for (const path of outputs) {
    const file = compared.get(path.split('/').at(-1));
    const expected = hash(read(path));
    if (file?.retained !== expected || file?.regenerated !== expected) { reject(); }
  }
  return {
    image: image.xbe, xbeSha256: record.xbeSha256, xbeSize: record.xbeSize,
    storyPolicy: image.policy,
    requiredStoryFiles: image.requiredFiles.map((file) => ({ ...file })),
    provenance: 'Retained generation reproduced byte for byte. Initial story metadata gate approved by the user; not complete startup asset coverage.',
  };
};

// a record for xbe from a fresh recompiler run whose outputs regenerated(name) returns; every output must match
export const recordGeneration = (xbe, read, regenerated) => {
  const { inputs, outputs } = imageGeneration(xbe);
  const bytes = read(`assets/${xbe}`);
  const comparison = outputs.map((path) => {
    const name = path.split('/').at(-1);
    const retained = hash(read(path));
    const fresh = regenerated(name);
    if (!fresh || hash(fresh) !== retained) { throw new Error(`the generator did not reproduce ${name}`); }
    return { name, retained, regenerated: retained };
  });
  return { schema: 1, scope: 'Local reproduction evidence, not a production boot manifest', image: xbe, xbeSha256: hash(bytes), xbeSize: bytes.length,
    inputs: Object.fromEntries(inputs.map((path) => [path, hash(read(path))])), comparison };
};

// node tools/browser/provenance.mjs record <xbe> <regenerated directory> <record.json>
if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href && process.argv[2] === 'record') {
  const [xbe, directory, output] = process.argv.slice(3);
  const root = resolve(new URL('../..', import.meta.url).pathname);
  const record = recordGeneration(xbe, (path) => readFileSync(resolve(root, path)), (name) => readFileSync(resolve(directory, name)));
  writeFileSync(output, JSON.stringify(record, null, 2) + '\n');
}
