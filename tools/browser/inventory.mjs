import { createHash } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { generationInputs, generationOutputs, verifyGeneration } from './provenance.mjs';
import { exportCatalog } from './export-catalog.mjs';

const hash = (content) => createHash('sha256').update(content).digest('hex');
const unique = (names, category) => {
  const result = new Set(names);
  if (result.size !== names.length) {
    throw new Error(`duplicate ${category}`);
  }
  return result;
};

export const validateImageSymbols = (config, header) => {
  if (!/^path = "assets\/default\.xbe"$/m.test(config) || !/^#define IMAGE_XBE "default\.xbe"$/m.test(header)) {
    throw new Error('story image identity mismatch');
  }
  const section = config.split('[symbols]')[1]?.split(/^\[/m)[0];
  if (!section) {
    throw new Error('missing configuration symbols');
  }
  const addresses = new Map([...header.matchAll(/^#define SYM_(\w+) ([\dA-F]+)$/gm)].map((match) => [match[1], parseInt(match[2], 16)]));
  const present = new Map([...header.matchAll(/^#define HAS_(\w+) ([01])$/gm)].map((match) => [match[1], Number(match[2])]));
  let count = 0;
  for (const line of `${section}\n${config.split('[data]')[1]?.split(/^\[/m)[0] ?? ''}`.split('\n').map((line) => line.split('#')[0].trim()).filter(Boolean)) {
    const match = /^(\w+)\s*=\s*(0x[\dA-Fa-f]+|0)$/.exec(line);
    if (!match) {
      throw new Error(`unsupported symbol declaration: ${line}`);
    }
    const [, name, value] = match;
    const address = Number(value);
    if (present.get(name) !== Number(address !== 0) || (address !== 0 && addresses.get(name) !== address)) {
      throw new Error(`configuration symbol mismatch: ${name}`);
    }
    ++count;
  }
  if (count !== addresses.size || count !== present.size) {
    throw new Error('configuration symbol count mismatch');
  }
  return count;
};

export const validateSymbols = (header, partitions, mapping) => {
  const declarations = unique([...header.matchAll(/^X86_EXTERN_FUNC\((\w+)\);/gm)].map((match) => match[1]), 'declaration');
  const bodies = unique([...partitions, mapping].flatMap((text) => [...text.matchAll(/^X86_FUNC\(__imp__(\w+)\)/gm)].map((match) => match[1])), 'body');
  const aliases = unique([...partitions, mapping].flatMap((text) => [...text.matchAll(/^X86_WEAK_FUNC\((\w+), __imp__\1\)/gm)].map((match) => match[1])), 'alias');
  const mappings = [...mapping.matchAll(/\{0x([\dA-Fa-f]+)u, (\w+)\}/g)];
  const entries = unique(mappings.map((match) => match[2]), 'mapping');
  for (const [, address, name] of mappings) {
    if (name !== `sub_${address.toUpperCase().padStart(8, '0')}`) {
      throw new Error(`mapping address mismatch: ${address} ${name}`);
    }
  }
  for (const [category, names] of [['body', bodies], ['alias', aliases]]) {
    if (names.size !== declarations.size || [...declarations].some((name) => !names.has(name))) {
      throw new Error(`${category} inventory disagrees with declarations`);
    }
  }
  if (!declarations.size) {
    throw new Error('empty function inventory');
  }
  const absent = new Set([...mapping.matchAll(/^X86_FUNC\(__imp__(sub_FFFF[\dA-F]{4})\) \{ x86_trap\(ctx, 0xFFFF[\dA-F]{4}u, "not in this image"\); \}/gm)].map((match) => match[1]));
  const expectedEntries = [...declarations].filter((name) => !absent.has(name));
  if (entries.size !== expectedEntries.length || expectedEntries.some((name) => !entries.has(name))) {
    throw new Error('mapping inventory disagrees with image declarations');
  }
  return entries.size;
};

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const [root, output, toolchain, configuration, generationRecord] = process.argv.slice(2);
  const read = (path) => readFileSync(resolve(root, path));
  const config = read('config/scda_v2.toml').toString();
  const count = Number(config.match(/^partitions = (\d+)$/m)?.[1]);
  if (count !== 128) {
    throw new Error(`expected 128 story partitions, found ${count}`);
  }
  const sources = Array.from({ length: count }, (_, n) => `generated/game/x86_recomp.${n}.cpp`);
  const actual = readdirSync(resolve(root, 'generated/game')).filter((name) => /^x86_recomp\.\d+\.cpp$/.test(name));
  if (actual.length !== count) {
    throw new Error(`expected ${count} partitions, found ${actual.length}`);
  }
  const mapping = read('generated/game/x86_func_mapping.cpp').toString();
  const namedSymbolCount = validateImageSymbols(config, read('generated/game/image_symbols.h').toString());
  const functionCount = validateSymbols(read('generated/game/x86_recomp_shared.h').toString(), sources.map((path) => read(path).toString()), mapping);
  const ranges = Object.fromEntries([...mapping.matchAll(/^const uint32_t (X86\w+) = (0x[\dA-Fa-f]+)u;/gm)].map((match) => [match[1], Number(match[2])]));
  const paths = [...sources, 'generated/game/x86_func_mapping.cpp', 'generated/game/x86_recomp_shared.h', 'generated/game/image_symbols.h', 'config/scda_v2.toml', 'config/xdk_symbols.toml', 'src/cpu/x86_ops.h', 'src/cpu/x86_context.h', 'CMakeLists.txt', 'src/browser/diagnostic_service.h', 'src/browser/diagnostic.cpp', 'src/browser/placeholders.cpp', 'src/browser/index.html', 'src/browser/diagnostic.mjs', 'tools/browser/CMakeLists.txt', 'tools/browser/inventory.mjs'];
  if (configuration.includes('full-cpu=ON') || configuration.includes('boot=ON')) {
    paths.push('src/browser/cpu_runtime.h', 'src/browser/cpu_runtime.cpp', 'tests/browser/cpu.cpp', 'tests/browser/full-cpu.html', 'tools/browser/adapt-memory.mjs', 'tools/browser/adapt-arithmetic.mjs', 'tools/browser/prepare-cpu.mjs');
  }
  let kernelExports;
  if (configuration.includes('boot=ON')) {
    const declarations = readdirSync(resolve(root, 'src/kernel')).filter((name) => name.endsWith('.cpp')).sort().map((name) => `src/kernel/${name}`);
    kernelExports = exportCatalog(declarations.map((path) => ({ path, text: read(path).toString() })));
    paths.push('tools/browser/export-catalog.mjs', ...declarations);
    paths.push(...['boot-worker', 'boot-session', 'boot-controller', 'boot-runtime', 'boot-shell', 'boot-worker-broker', 'boot-worker-host', 'iso-reader', 'iso-mailbox', 'iso-wasm', 'iso-io-service', 'iso-io-worker'].map((name) => `src/browser/${name}.mjs`));
    paths.push('src/browser/boot.html');
    paths.push('src/browser/platform-profile.cpp', 'src/browser/platform-profile.h');
    paths.push('src/browser/shutdown-provider.cpp', 'src/browser/shutdown-provider.h');
    paths.push('src/browser/dpc-provider.cpp', 'src/browser/dpc-provider.h');
    paths.push('src/browser/timer-provider.cpp', 'src/browser/timer-provider.h');
    paths.push('src/browser/sdk-provider.cpp', 'src/browser/sdk-provider.h',
      'src/browser/diagnostic-graphics.cpp', 'src/browser/diagnostic-graphics.h', 'src/gpu/backend.h', 'src/gpu/state.h',
      'src/browser/silent-audio.cpp', 'src/browser/silent-audio.h', 'src/audio/output.h');
    paths.push('src/browser/virtual-memory.cpp', 'src/browser/virtual-memory.h');
    paths.push('src/browser/contiguous-memory.cpp', 'src/browser/contiguous-memory.h');
    paths.push('src/browser/string-provider.cpp', 'src/browser/string-provider.h');
    paths.push('src/browser/section-provider.cpp', 'src/browser/section-provider.h');
    paths.push('src/browser/file-provider.cpp', 'src/browser/file-provider.h', 'src/browser/temporary-filesystem.cpp', 'src/browser/temporary-filesystem.h');
    paths.push('src/browser/raw-cache.cpp', 'src/browser/raw-cache.h', 'src/browser/cache-fatx.cpp', 'src/browser/cache-fatx.h');
    paths.push('src/browser/kernel-provider.cpp', 'src/browser/kernel-provider.h', 'src/browser/thread-provider.cpp', 'src/browser/thread-provider.h', 'src/kernel/crypto.cpp', 'src/kernel/crypto.h');
    paths.push('src/browser/boot-wasm.cpp', 'src/browser/disc-wasm.cpp', 'src/browser/image.cpp', 'src/browser/image.h', 'src/kernel/disc_index.cpp', 'src/kernel/disc_index.h', 'src/browser/iso-validation.mjs', 'src/browser/iso-image.mjs');
  }
  paths.push('tools/browser/provenance.mjs');
  const discIdentity = generationRecord ? verifyGeneration(JSON.parse(readFileSync(generationRecord, 'utf8')), read) : {
    image: 'default.xbe', xbeSha256: null, xbeSize: null, requiredStoryFiles: null,
    provenance: 'Generated input hashes captured. Trusted generation XBE identity is absent; real boot is blocked. Never derive trust from the selected ISO.',
  };
  if (generationRecord) { paths.push(...generationInputs, ...generationOutputs); }
  const inputs = Object.fromEntries([...new Set(paths)].map((path) => [path, hash(read(path))]));
  const manifest = { schema: 1, compatibility: (configuration.includes('full-cpu=ON') || configuration.includes('boot=ON')) ? 'wasm-compact-leased-v1' : 'wasm-weak-alias-v1', toolchain, configuration, partitionCount: count, functionCount, namedSymbolCount, ranges, ...discIdentity, ...(kernelExports ? { kernelExports } : {}), inputs };
  const identity = hash(JSON.stringify(manifest));
  const emit = (name, content) => {
    const path = resolve(output, name);
    if (!existsSync(path) || readFileSync(path, 'utf8') !== content) { writeFileSync(path, content); }
  };
  emit('input-manifest.json', JSON.stringify({ identity, ...manifest }, null, 2) + '\n');
  if (configuration.includes('boot=ON')) {
    if (!generationRecord) { throw new Error('Boot requires trusted generation evidence'); }
    emit('boot-manifest.mjs', `export default ${JSON.stringify({ identity, ...manifest })};\n`);
  }
  emit('build_identity.h', `#pragma once\n#define BROWSER_BUILD_ID "${identity}"\n#define BROWSER_FUNCTION_COUNT ${functionCount}u\n#define BROWSER_TOOLCHAIN "${toolchain}"\n`);
}
