import { createHash } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { adaptMemory } from './adapt-memory.mjs';
import { adaptArithmetic } from './adapt-arithmetic.mjs';

const [root, output, mode] = process.argv.slice(2);
if (!root || !output) { throw new Error('Expected repository and output directory'); }
mkdirSync(output, { recursive: true });
const inputs = {};
for (let n = 0; n < 129; ++n) {
  const name = n === 128 ? 'x86_func_mapping.cpp' : `x86_recomp.${n}.cpp`;
  const source = readFileSync(resolve(root, 'generated/game', name), 'utf8');
  const adapted = adaptArithmetic(adaptMemory(source, { blocks: mode === 'mission-blocks', flat: mode === 'mission-flat' }));
  const path = resolve(output, name);
  const previous = existsSync(path) ? readFileSync(path, 'utf8') : null;
  if (previous !== adapted) { writeFileSync(path, adapted); }
  inputs[name] = createHash('sha256').update(source).digest('hex');
}
writeFileSync(resolve(output, 'input-hashes.json'), JSON.stringify(inputs, null, 2) + '\n');
