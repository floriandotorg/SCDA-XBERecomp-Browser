import { createHash } from 'node:crypto';

export const generationInputs = [
  'CMakeLists.txt', 'config/scda_v2.toml', 'config/xdk_symbols.toml', 'src/xbe/xbe.h',
  ...['analysis.cpp', 'analysis.h', 'codegen.cpp', 'codegen.h', 'codegen_emit.cpp', 'codegen_fp.cpp', 'codegen_int.cpp', 'main.cpp'].map((name) => `tools/recomp/${name}`),
];
export const generationOutputs = [
  ...Array.from({ length: 128 }, (_, n) => `generated/game/x86_recomp.${n}.cpp`),
  ...['x86_func_mapping.cpp', 'x86_recomp_shared.h', 'image_symbols.h', 'exports.def', 'imports.def', 'x86_recomp_stats.txt'].map((name) => `generated/game/${name}`),
];
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
const reject = () => { throw new Error('Trusted generation evidence is absent, incomplete or stale.'); };

export const verifyGeneration = (record, read) => {
  if (record?.schema !== 1 || record.image !== 'default.xbe' || !record.inputs ||
      !Array.isArray(record.comparison) || record.comparison.length !== generationOutputs.length) {
    reject();
  }
  const bytes = read('assets/default.xbe');
  if (record.xbeSize !== bytes.length || record.xbeSha256 !== hash(bytes)) { reject(); }
  for (const path of generationInputs) {
    if (record.inputs[path] !== hash(read(path))) { reject(); }
  }
  const compared = new Map();
  for (const file of record.comparison) {
    if (!file || typeof file.name !== 'string' || compared.has(file.name)) { reject(); }
    compared.set(file.name, file);
  }
  for (const path of generationOutputs) {
    const file = compared.get(path.split('/').at(-1));
    const expected = hash(read(path));
    if (file?.retained !== expected || file?.regenerated !== expected) { reject(); }
  }
  return {
    image: 'default.xbe', xbeSha256: record.xbeSha256, xbeSize: record.xbeSize,
    storyPolicy: 'scda-story-metadata-v1',
    requiredStoryFiles: [
      { path: 'Default/DataXb/LMaps/common.lin', minBytes: 1 },
      { path: 'Default/DataXb/LMaps/Menu/Menu.lin', minBytes: 1 },
    ],
    provenance: 'Retained generation reproduced byte for byte. Initial story metadata gate approved by the user; not complete startup asset coverage.',
  };
};
