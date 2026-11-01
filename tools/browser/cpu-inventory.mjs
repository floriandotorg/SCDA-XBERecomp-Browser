import { readdirSync, readFileSync, mkdirSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';

const output = resolve(process.argv[2] ?? 'out/cpu-tests/memory-inventory.json');
const sources = ['src', 'generated/game'].flatMap(directory => readdirSync(directory, { recursive: true, withFileTypes: true })
  .filter(entry => entry.isFile() && /\.(cpp|h)$/.test(entry.name))
  .map(entry => `${entry.parentPath}/${entry.name}`));
const patterns = {
  flatAddress: /\b(?:g_base|base)\s*\+/,
  hostPointer: /\bHostPtr\s*\(/,
  guestReference: /\bG\s*</,
  addressCast: /reinterpret_cast|\(\s*(?:u?int\d+_t|float|double|void)\s*\*\s*\)/,
  translatedPointer: /X86_PTR\s*\(/,
  bulk: /(?:std::)?(?:memcpy|memmove|memset)\s*\(/,
  atomic: /__atomic_\w+\s*\(/,
};
const sites = [];
const atomicOperations = {};
const atomicWidths = {};
for (const path of sources.sort()) {
  const lines = readFileSync(path, 'utf8').split('\n');
  for (let n = 0; n < lines.length; ++n) {
    const line = lines[n];
    const categories = Object.entries(patterns).filter(([, pattern]) => pattern.test(line)).map(([name]) => name);
    if (categories.length) {
      sites.push({ path, line: n + 1, categories, source: line.trim(), scope: path.startsWith('generated/') || path === 'src/cpu/x86_ops.h' || path.startsWith('src/browser/cpu_runtime.') ? 'cpu-prototype' : 'native-not-ported' });
    }
    if (path.startsWith('generated/')) {
      for (const match of line.matchAll(/(__atomic_\w+)\(/g)) { atomicOperations[match[1]] = (atomicOperations[match[1]] ?? 0) + 1; }
      for (const match of line.matchAll(/__atomic_\w+\(X86_PTR\((\w+),/g)) { atomicWidths[match[1]] = (atomicWidths[match[1]] ?? 0) + 1; }
    }
  }
}
const inventory = { sources: sources.length, atomicOperations, atomicWidths, sites };
mkdirSync(dirname(output), { recursive: true });
writeFileSync(output, `${JSON.stringify(inventory, null, 2)}\n`);
console.log(JSON.stringify({ output, sources: sources.length, sites: sites.length, atomicOperations, atomicWidths }, null, 2));
