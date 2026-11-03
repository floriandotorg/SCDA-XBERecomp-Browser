import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, renameSync, unlinkSync, writeFileSync, existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

export const tailCallFlags = Object.freeze([
  '--mvp-features', '--enable-threads', '--enable-mutable-globals',
  '--enable-nontrapping-float-to-int', '--enable-sign-ext', '--enable-bulk-memory',
  '--enable-exception-handling', '--enable-multivalue', '--enable-reference-types',
  '--enable-tail-call', '--tail-call', '--debuginfo',
]);

const digest = (path) => createHash('sha256').update(readFileSync(path)).digest('hex');

export const optimizerIdentity = (tool) => {
  if (!tool || !existsSync(tool)) {
    throw new Error(`Tail-call optimizer is missing: ${tool || 'set BROWSER_WASM_OPT'}`);
  }
  const result = spawnSync(tool, ['--version'], { encoding: 'utf8' });
  const version = result.stdout?.trim();
  if (result.error || result.status !== 0 || version !== 'wasm-opt version 133') {
    throw new Error(`Tail-call optimizer requires Binaryen 133; found ${version || result.error?.message || result.stderr}`);
  }
  return { mode: 'actual-tail-call', version, sha256: digest(tool), flags: [...tailCallFlags] };
};

const verifyOptimizer = (optimization) => {
  const actual = optimizerIdentity(optimization.tool);
  if (actual.version !== optimization.version || actual.sha256 !== optimization.sha256 ||
      JSON.stringify(actual.flags) !== JSON.stringify(optimization.flags)) {
    throw new Error('Tail-call optimizer identity or flags changed; reconfigure the build.');
  }
};

export const publishWasm = ({ input, output, optimization, companions = [] }) => {
  const publications = [{ input, output }, ...companions].map((file) => ({ ...file, temporary: `${file.output}.tmp-${process.pid}` }));
  if (publications.some((file) => resolve(file.input) === resolve(file.output)) ||
      new Set(publications.map((file) => resolve(file.output))).size !== publications.length) {
    throw new Error('Postlink inputs must remain pristine and separate from distinct final outputs.');
  }
  if (!optimization || !['off', 'actual-tail-call'].includes(optimization.mode)) {
    throw new Error('Unknown Wasm postlink mode.');
  }
  const temporary = publications[0].temporary;
  try {
    for (const file of publications) {
      mkdirSync(dirname(file.output), { recursive: true });
    }
    for (const file of publications.slice(1)) {
      writeFileSync(file.temporary, readFileSync(file.input));
    }
    if (optimization.mode === 'actual-tail-call') {
      verifyOptimizer(optimization);
      const result = spawnSync(optimization.tool, [input, ...tailCallFlags, '-o', temporary], { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });
      if (result.error || result.status !== 0) {
        throw new Error(`Tail-call optimizer failed: ${result.error?.message || result.stderr || `exit ${result.status}`}`);
      }
      verifyOptimizer(optimization);
    } else {
      writeFileSync(temporary, readFileSync(input));
    }
    const bytes = readFileSync(temporary);
    if (bytes.length < 8 || !bytes.subarray(0, 8).equals(Buffer.from([0, 97, 115, 109, 1, 0, 0, 0]))) {
      throw new Error('Postlink did not produce a Wasm module.');
    }
    for (const file of publications) {
      renameSync(file.temporary, file.output);
    }
  } finally {
    for (const file of publications) {
      if (existsSync(file.temporary)) {
        unlinkSync(file.temporary);
      }
    }
  }
};

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const [input, output, configuration, ...files] = process.argv.slice(2);
  if (files.length % 2 !== 0) {
    throw new Error('Postlink companions require input/output pairs.');
  }
  const companions = Array.from({ length: files.length / 2 }, (_, n) => ({ input: files[n * 2], output: files[n * 2 + 1] }));
  publishWasm({ input, output, optimization: JSON.parse(readFileSync(configuration, 'utf8')), companions });
}
