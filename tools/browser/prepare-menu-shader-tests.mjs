import { copyFile, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '../..');
const output = resolve(root, 'out/menu-shaders-public');
const names = [...Array.from({ length: 16 }, (_, n) => `shadow${n}`), ...Array.from({ length: 16 }, (_, n) => `psshadow${n}`), 'depth-bias', 'depth-bias-color', 'depth-only', 'border', 'border-mip', 'cube', 'pscube', 'psreflect', 'psdotst', 'plain', 'textured', 'flat', 'stages', 'program', 'paired', 'relative', 'rcc', 'rcp', 'rsq', 'pixel', 'psef', ...Array.from({ length: 8 }, (_, n) => `pssum${n}`), 'psdot1', 'psdot2', 'psdot3', 'pskill', 'pixel-program', 'psconstants', 'psmux', 'pssimultaneous', ...Array.from({ length: 8 }, (_, n) => `psinput${n}`), ...[0, 8, 16, 24, 32, 48].map(n => `psoutput${n}`), ...Array.from({ length: 13 }, (_, n) => `mac${n + 1}`), ...Array.from({ length: 23 }, (_, n) => n + 2).filter((n) => n !== 17).map((n) => `op${n}`), ...Array.from({ length: 5 }, (_, n) => `combine${n + 18}`)];
const paths = ['tests/browser/menu-shaders-browser.html', 'tests/browser/menu-shaders-browser.mjs',
  'out/menu-shaders-tests/constants.bin', ...names.flatMap((name) => ['vs', 'fs'].map((stage) => `out/menu-shaders-tests/${name}.${stage}.wgsl`))];
for (const path of paths) {
  const destination = resolve(output, path);
  await mkdir(dirname(destination), { recursive: true });
  await copyFile(resolve(root, path), destination);
}
console.log(output);
