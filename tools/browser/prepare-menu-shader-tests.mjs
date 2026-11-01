import { copyFile, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';

const root = resolve(import.meta.dirname, '../..');
const output = resolve(root, 'out/menu-shaders-public');
const names = ['plain', 'textured', 'stages', ...Array.from({ length: 23 }, (_, n) => n + 2).filter((n) => n <= 16 || n >= 23).map((n) => `op${n}`)];
const paths = ['tests/browser/menu-shaders-browser.html', 'tests/browser/menu-shaders-browser.mjs',
  'out/menu-shaders-tests/constants.bin', ...names.flatMap((name) => ['vs', 'fs'].map((stage) => `out/menu-shaders-tests/${name}.${stage}.wgsl`))];
for (const path of paths) {
  const destination = resolve(output, path);
  await mkdir(dirname(destination), { recursive: true });
  await copyFile(resolve(root, path), destination);
}
console.log(output);
