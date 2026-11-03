import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { chmodSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { generationInputs, generationOutputs } from '../../tools/browser/provenance.mjs';
import { tailCallFlags } from '../../tools/browser/tail-call-postlink.mjs';

const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
const parent = resolve('out/m4-current/tail-inventory-tests');
mkdirSync(parent, { recursive: true });
const fixture = () => {
  const root = mkdtempSync(join(parent, 'inputs-'));
  const put = (path, bytes) => {
    mkdirSync(dirname(join(root, path)), { recursive: true });
    writeFileSync(join(root, path), bytes);
  };
  const placeholders = (path) => {
    for (const entry of readdirSync(path, { withFileTypes: true })) {
      const name = join(path, entry.name);
      if (entry.isDirectory()) { placeholders(name); }
      else { put(name, `public fixture ${name}\n`); }
    }
  };
  for (const path of ['src', 'tools/browser', 'tools/recomp', 'tests/browser']) { placeholders(path); }
  for (const path of [...generationInputs, ...generationOutputs, 'assets/default.xbe']) { put(path, `public fixture ${path}\n`); }
  put('config/scda_v2.toml', '[xbe]\npath = "assets/default.xbe"\npartitions = 128\n[symbols]\nExample = 0x11000\n[data]\n');
  put('generated/game/image_symbols.h', '#define IMAGE_XBE "default.xbe"\n#define SYM_Example 00011000\n#define HAS_Example 1\n');
  put('generated/game/x86_recomp_shared.h', 'X86_EXTERN_FUNC(sub_00011000);\n');
  put('generated/game/x86_recomp.0.cpp', 'X86_FUNC(__imp__sub_00011000) {}\nX86_WEAK_FUNC(sub_00011000, __imp__sub_00011000)\n');
  put('generated/game/x86_func_mapping.cpp', '{0x11000u, sub_00011000},\n');
  const read = (path) => readFileSync(join(root, path));
  const record = {
    schema: 1, image: 'default.xbe', xbeSha256: hash(read('assets/default.xbe')), xbeSize: read('assets/default.xbe').length,
    inputs: Object.fromEntries(generationInputs.map((path) => [path, hash(read(path))])),
    comparison: generationOutputs.map((path) => ({ name: path.split('/').at(-1), retained: hash(read(path)), regenerated: hash(read(path)) })),
  };
  put('record.json', JSON.stringify(record));
  const tool = join(root, 'wasm-opt');
  const writeTool = (version = 133, suffix = '') => {
    writeFileSync(tool, `#!/usr/bin/env node\nconsole.log('wasm-opt version ${version}');\n${suffix}\n`);
    chmodSync(tool, 0o755);
  };
  writeTool();
  return { root, put, tool, writeTool };
};

test('real inventory binds mode, flags, actual binary and helper sources without output hash cycles', () => {
  const files = fixture();
  const config = 'Release;story=OFF;full-cpu=OFF;boot=ON;menu-capture=OFF;menu=ON';
  const inventory = (mode, name, { trusted = true, tool = files.tool } = {}) => {
    const output = join(files.root, name);
    mkdirSync(output, { recursive: true });
    const result = spawnSync(process.execPath, ['tools/browser/inventory.mjs', files.root, output, 'emcc 6.0.13-git', `${config};actual-tail-calls=${mode}`, trusted ? join(files.root, 'record.json') : '', tool], { encoding: 'utf8' });
    return { result, output, manifest: result.status === 0 ? JSON.parse(readFileSync(join(output, 'input-manifest.json'), 'utf8')) : undefined };
  };
  const off = inventory('OFF', 'off', { tool: '/missing/ignored-optimizer' });
  assert.equal(off.result.status, 0, off.result.stderr);
  assert.deepEqual(off.manifest.wasmRequirements, []);
  assert.deepEqual(off.manifest.wasmOptimization, { mode: 'off', flags: [] });
  const on = inventory('ON', 'on');
  assert.equal(on.result.status, 0, on.result.stderr);
  assert.deepEqual(on.manifest.wasmRequirements, ['tail-call']);
  assert.deepEqual(on.manifest.wasmOptimization.flags, tailCallFlags);
  assert.equal(on.manifest.wasmOptimization.version, 'wasm-opt version 133');
  assert.equal(on.manifest.wasmOptimization.sha256, hash(readFileSync(files.tool)));
  assert.equal(on.manifest.wasmOptimization.tool, undefined);
  assert.equal(on.manifest.xbeSha256, off.manifest.xbeSha256);
  assert.notEqual(on.manifest.identity, off.manifest.identity);
  for (const path of ['tools/browser/tail-call-postlink.mjs', 'src/browser/wasm-capabilities.mjs']) {
    assert.ok(off.manifest.inputs[path]);
    assert.ok(on.manifest.inputs[path]);
  }
  assert.equal(Object.keys(on.manifest.inputs).some((path) => /story_boot\.wasm|boot-manifest|wasm-postlink/.test(path)), false);
  const privateConfig = JSON.parse(readFileSync(join(on.output, 'wasm-postlink.json'), 'utf8'));
  assert.equal(privateConfig.tool, files.tool);
  const deployed = awaitManifest(join(on.output, 'boot-manifest.mjs'));
  assert.deepEqual(deployed, on.manifest);
  files.writeTool(133, 'void 1;');
  assert.notEqual(inventory('ON', 'changed-tool').manifest.identity, on.manifest.identity);
  assert.equal(inventory('OFF', 'off-tool-changed').manifest.identity, off.manifest.identity);
  files.put('tools/browser/tail-call-postlink.mjs', 'changed shared helper');
  assert.notEqual(inventory('OFF', 'off-source-changed').manifest.identity, off.manifest.identity);
  files.writeTool(132);
  assert.notEqual(inventory('ON', 'wrong-version').result.status, 0);
  assert.notEqual(inventory('ON', 'missing', { tool: join(files.root, 'absent') }).result.status, 0);
  files.writeTool(133);
  const untrusted = inventory('ON', 'untrusted', { trusted: false });
  assert.notEqual(untrusted.result.status, 0);
  assert.match(untrusted.result.stderr, /trusted generation/i);
  files.put('assets/default.xbe', 'changed untrusted image');
  const mismatch = inventory('ON', 'changed-image');
  assert.notEqual(mismatch.result.status, 0);
  assert.match(mismatch.result.stderr, /executable|identity|generation/i);
});

const awaitManifest = (path) => JSON.parse(readFileSync(path, 'utf8').replace(/^export default /, '').replace(/;\s*$/, ''));
