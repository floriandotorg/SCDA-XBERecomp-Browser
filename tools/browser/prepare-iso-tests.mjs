import { copyFile, mkdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../../', import.meta.url));
const destination = resolve(process.argv[2] ?? resolve(root, 'out/iso-tests'));
const files = [
  'src/browser/iso-reader.mjs',
  'src/browser/iso-mailbox.mjs',
  'src/browser/iso-io-service.mjs',
  'src/browser/iso-io-worker.mjs',
  'tests/browser/iso.html',
  'tests/browser/iso-browser.mjs',
  'tests/browser/iso-guest-browser.mjs',
];
for (const file of files) {
  const path = resolve(destination, file);
  await mkdir(dirname(path), { recursive: true });
  await copyFile(resolve(root, file), path);
}
console.log(`ISO reader fixtures: ${destination}/tests/browser/iso.html`);
