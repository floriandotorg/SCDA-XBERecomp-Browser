import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { resolve, join } from 'node:path';
import { optimizerIdentity, publishWasm, tailCallFlags } from '../../tools/browser/tail-call-postlink.mjs';

const parent = resolve('out/m4-current/tail-postlink-tests');
mkdirSync(parent, { recursive: true });
const wasm = Buffer.from([0, 97, 115, 109, 1, 0, 0, 0]);
const fixture = ({ version = 133, fail = false } = {}) => {
  const directory = mkdtempSync(join(parent, 'case-'));
  const tool = join(directory, 'wasm-opt');
  const input = join(directory, 'raw.wasm');
  const output = join(directory, 'final.wasm');
  const script = (suffix = '') => `#!/usr/bin/env node
const fs = require('node:fs');
if (process.argv.includes('--version')) { console.log('wasm-opt version ${version}'); process.exit(0); }
fs.writeFileSync(${JSON.stringify(join(directory, 'args.json'))}, JSON.stringify(process.argv.slice(2)));
fs.writeFileSync(process.argv[process.argv.indexOf('-o') + 1], Buffer.concat([fs.readFileSync(process.argv[2]), Buffer.from([0, 2, 1, 116])]));
${fail ? 'process.exit(7);' : ''}
${suffix}
`;
  writeFileSync(tool, script());
  chmodSync(tool, 0o755);
  writeFileSync(input, wasm);
  return { directory, tool, input, output, script };
};

test('tail-only flags explicitly preserve existing features without widening imports', () => {
  assert.deepEqual(tailCallFlags, ['--mvp-features', '--enable-threads', '--enable-mutable-globals',
    '--enable-nontrapping-float-to-int', '--enable-sign-ext', '--enable-bulk-memory',
    '--enable-exception-handling', '--enable-multivalue', '--enable-reference-types',
    '--enable-tail-call', '--tail-call', '--debuginfo']);
  assert.equal(tailCallFlags.includes('--all-features'), false);
});

test('optimizer identity pins the actual version and binary digest', () => {
  const files = fixture();
  const identity = optimizerIdentity(files.tool);
  assert.equal(identity.mode, 'actual-tail-call');
  assert.equal(identity.version, 'wasm-opt version 133');
  assert.equal(identity.sha256, createHash('sha256').update(readFileSync(files.tool)).digest('hex'));
  assert.deepEqual(identity.flags, tailCallFlags);
  assert.throws(() => optimizerIdentity(join(files.directory, 'absent')), /optimizer.*missing|ENOENT/i);
  assert.throws(() => optimizerIdentity(fixture({ version: 132 }).tool), /133/);
});

test('failure and provenance mismatches never replace the previous final artifact', () => {
  const files = fixture({ fail: true });
  const optimization = { ...optimizerIdentity(files.tool), tool: files.tool };
  writeFileSync(files.output, 'previous complete deployment');
  assert.throws(() => publishWasm({ ...files, optimization }), /optimizer.*failed/i);
  assert.equal(readFileSync(files.output, 'utf8'), 'previous complete deployment');
  assert.equal(readdirSync(files.directory).some((name) => name.includes('.tmp-')), false);
  assert.throws(() => publishWasm({ ...files, optimization: { ...optimization, sha256: '0'.repeat(64) } }), /identity|digest|changed/i);
  assert.throws(() => publishWasm({ ...files, optimization: { ...optimization, flags: ['--all-features'] } }), /flags|identity/i);
  assert.equal(readFileSync(files.output, 'utf8'), 'previous complete deployment');
});

test('off/on/off and relinks always start from pristine input and publish deterministically', () => {
  const files = fixture();
  const optimization = { ...optimizerIdentity(files.tool), tool: files.tool };
  publishWasm({ ...files, optimization: { mode: 'off' } });
  assert.deepEqual(readFileSync(files.output), wasm);
  publishWasm({ ...files, optimization });
  const optimized = readFileSync(files.output);
  assert.deepEqual(optimized, Buffer.concat([wasm, Buffer.from([0, 2, 1, 116])]));
  assert.deepEqual(JSON.parse(readFileSync(join(files.directory, 'args.json'), 'utf8')).slice(1, -2), tailCallFlags);
  publishWasm({ ...files, optimization });
  assert.deepEqual(readFileSync(files.output), optimized);
  const relink = Buffer.concat([wasm, Buffer.from([0, 2, 1, 114])]);
  writeFileSync(files.input, relink);
  publishWasm({ ...files, optimization });
  assert.deepEqual(readFileSync(files.output), Buffer.concat([relink, Buffer.from([0, 2, 1, 116])]));
  publishWasm({ ...files, optimization: { mode: 'off' } });
  assert.deepEqual(readFileSync(files.output), relink);
  assert.deepEqual(readFileSync(files.input), relink);
  assert.equal(existsSync(files.output), true);
});

test('failed optimization preserves the matching module and build-owned manifest together', () => {
  const files = fixture({ fail: true });
  const moduleInput = join(files.directory, 'raw.mjs');
  const moduleOutput = join(files.directory, 'final.mjs');
  const manifestInput = join(files.directory, 'new-manifest.mjs');
  const manifestOutput = join(files.directory, 'boot-manifest.mjs');
  writeFileSync(files.output, wasm);
  writeFileSync(moduleInput, 'new module');
  writeFileSync(moduleOutput, 'old module');
  writeFileSync(manifestInput, 'new manifest requiring tail calls');
  writeFileSync(manifestOutput, 'old manifest without tail requirement');
  const companions = [{ input: moduleInput, output: moduleOutput }, { input: manifestInput, output: manifestOutput }];
  const optimization = { ...optimizerIdentity(files.tool), tool: files.tool };
  assert.throws(() => publishWasm({ ...files, optimization, companions }), /optimizer.*failed/i);
  assert.deepEqual(readFileSync(files.output), wasm);
  assert.equal(readFileSync(moduleOutput, 'utf8'), 'old module');
  assert.equal(readFileSync(manifestOutput, 'utf8'), 'old manifest without tail requirement');
  publishWasm({ ...files, optimization: { mode: 'off' }, companions });
  assert.equal(readFileSync(moduleOutput, 'utf8'), 'new module');
  assert.equal(readFileSync(manifestOutput, 'utf8'), 'new manifest requiring tail calls');
});

test('changing a pinned tool requires a new configured identity', () => {
  const files = fixture();
  const before = optimizerIdentity(files.tool);
  writeFileSync(files.tool, files.script('void 1;'));
  assert.throws(() => publishWasm({ ...files, optimization: { ...before, tool: files.tool } }), /identity|digest|changed/i);
  const after = optimizerIdentity(files.tool);
  assert.notEqual(after.sha256, before.sha256);
  publishWasm({ ...files, optimization: { ...after, tool: files.tool } });
  assert.equal(WebAssembly.validate(readFileSync(files.output)), true);
});
