import { createHash } from 'node:crypto';
import { existsSync, readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { generationInputs, generationOutputs, verifyGeneration } from './provenance.mjs';
import { exportCatalog } from './export-catalog.mjs';
import { optimizerIdentity } from './tail-call-postlink.mjs';

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

const symbolTable = (symbols) => new Map([...symbols.matchAll(/^#define SYM_(\w+) ([\dA-Fa-f]+)$/gm)].map((match) => [match[1], match[2].toUpperCase().padStart(8, '0')]));

const guestOverrides = (text, address) => [
  ...[...text.matchAll(/^X86_FUNC\(sub_([\dA-Fa-f]{8})\)\s*\{/gm)].map((match) => match[1].toUpperCase()),
  ...[...text.matchAll(/^\s*X86_FUNC\(\w*GUEST\((\w+)\)\)\s*\{/gm)].map((match) => address(match[1])),
  ...[...text.matchAll(/^(?:HLE_STDCALL|HLE_FASTCALL|HLE_CDECL|\w+_BOUNDARY)\((\w+)\b/gm)].map((match) => address(match[1])),
  ...[...text.matchAll(/^\s*X\((\w+), \d+, \d+\)/gm)].map((match) => address(match[1])),
].filter(Boolean);

// A strong sub_X from a selected game hook replaces the same weak alias a browser override does: the two would collide
// at link, so the hook must be renamed to game_sub_X and chained from the browser override.
export const validateHookOverrides = ({ browser, hooks, renames, symbols }) => {
  const table = symbolTable(symbols);
  const owners = new Map();
  for (const [path, text] of Object.entries(browser)) {
    for (const address of guestOverrides(text, (name) => table.get(name))) { owners.set(address, path); }
  }
  const renamed = new Set(renames.map((address) => address.toUpperCase().padStart(8, '0')));
  const entries = [];
  const claimed = new Map();
  for (const [hook, text] of Object.entries(hooks).sort(([left], [right]) => left.localeCompare(right))) {
    const address = (name) => {
      if (!table.has(name)) { throw new Error(`hook ${hook} names unknown guest symbol ${name}`); }
      return table.get(name);
    };
    for (const guest of [...new Set(guestOverrides(text, address))].sort()) {
      if (claimed.has(guest)) { throw new Error(`sub_${guest} is hooked by both ${claimed.get(guest)} and ${hook}`); }
      claimed.set(guest, hook);
      const owner = owners.get(guest);
      if (owner && !renamed.has(guest)) { throw new Error(`sub_${guest} from hook ${hook} collides with ${owner}; add it to the hook renames`); }
      entries.push({ hook, address: guest, renamed: Boolean(owner) });
    }
  }
  return entries;
};

// Game hooks live in src/game; CRT fixes the native build links from src/xdk are selected the same way.
export const hookSource = (root, name) => {
  const source = ['game', 'xdk'].map((directory) => `src/${directory}/${name}.cpp`).find((path) => existsSync(resolve(root, path)));
  if (!source) { throw new Error(`hook ${name} has no source in src/game or src/xdk`); }
  return source;
};

const hookHeaders = ['xdk/hle.h', 'xdk/d3d8.h', 'gpu/state.h', 'app/app.h', 'core/log.h', 'game/magma.h', 'core/input.h', 'game/console.h',
  'game/unreal.h', 'app/translations.h', 'gpu/backend.h', 'game/loading.h', 'core/gyro.h'];

export const validateHookIncludes = ({ hooks, shadowed }) => {
  const allowed = new Set([...hookHeaders, ...shadowed]);
  for (const [hook, text] of Object.entries(hooks)) {
    for (const [, header] of text.matchAll(/^\s*#\s*include\s+"([^"]+)"/gm)) {
      if (!allowed.has(header)) { throw new Error(`hook ${hook} includes ${header}, which the browser does not provide`); }
    }
  }
  return Object.keys(hooks).length;
};

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const [root, output, toolchain, configuration, generationRecord, optimizerTool] = process.argv.slice(2);
  const actualTailCalls = configuration.split(';').includes('actual-tail-calls=ON');
  if (actualTailCalls && !configuration.split(';').includes('boot=ON')) {
    throw new Error('Actual tail calls require boot mode.');
  }
  const simd = configuration.split(';').includes('wasm-simd=ON');
  if (simd && !configuration.split(';').includes('mission=ON')) {
    throw new Error('WebAssembly SIMD requires mission mode.');
  }
  const wasmOptimization = actualTailCalls ? optimizerIdentity(optimizerTool, { simd }) : { mode: 'off', flags: [] };
  const wasmRequirements = [...(actualTailCalls ? ['tail-call'] : []), ...(simd ? ['simd128'] : [])];
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
    paths.push(...['boot-worker', 'boot-session', 'menu-state-report', 'boot-controller', 'boot-runtime', 'boot-shell', 'wasm-capabilities', 'boot-worker-broker', 'boot-worker-host', 'iso-reader', 'iso-mailbox', 'iso-wasm', 'iso-io-service', 'iso-io-worker', 'host-settings', 'keyboard-input', 'input-actions', 'host-messages'].map((name) => `src/browser/${name}.mjs`));
    paths.push('src/browser/boot.html');
    paths.push('src/browser/platform-profile.cpp', 'src/browser/platform-profile.h');
    paths.push('src/browser/shutdown-provider.cpp', 'src/browser/shutdown-provider.h');
    paths.push('src/browser/dpc-provider.cpp', 'src/browser/dpc-provider.h');
    paths.push('src/browser/timer-provider.cpp', 'src/browser/timer-provider.h');
    paths.push('src/browser/sdk-provider.cpp', 'src/browser/sdk-provider.h',
      'src/browser/diagnostic-graphics.cpp', 'src/browser/diagnostic-graphics.h', 'src/browser/hook-graphics.cpp', 'src/browser/hook-graphics.h', 'src/browser/diagnostic-shaders.cpp', 'src/browser/diagnostic-shaders.h', 'src/browser/push-state.cpp', 'src/browser/push-state.h', 'src/gpu/backend.h', 'src/gpu/state.h',
      'src/browser/silent-audio.cpp', 'src/browser/silent-audio.h', 'src/audio/output.h', 'src/browser/audio-transport.mjs',
      'src/browser/audio-entries.h', 'src/browser/audio-provider.h', 'src/browser/audio-provider.cpp',
      'src/browser/audio-core.h', 'src/browser/audio-core.cpp', 'src/browser/audio-effects.h', 'src/browser/audio-effects.cpp',
      'tests/browser/audio-core.cpp', 'tests/browser/audio-core.test.mjs', 'tests/browser/audio-effects.cpp', 'tests/browser/audio-effects.test.mjs',
      'tests/browser/audio-provider.cpp', 'tests/browser/audio-provider.test.mjs', 'tests/browser/audio-boot.test.mjs');
    if (configuration.includes('menu=ON') || configuration.includes('menu-capture=ON')) {
      paths.push('src/browser/av-provider.h', 'src/browser/av-provider.cpp', 'src/browser/delay-provider.h', 'src/browser/delay-provider.cpp', 'src/browser/menu-input.h', 'src/browser/menu-input.cpp', 'src/browser/menu-launch.h', 'src/browser/menu-launch.cpp',
        'src/browser/menu-route.h', 'src/browser/menu-route.cpp');
    }
    if (configuration.includes('menu=ON')) {
      if (configuration.split(';').includes('mission=ON')) { paths.push('src/browser/webgpu-pthreads.js'); }
      if (configuration.split(';').includes('menu-trace-aborts=ON')) {
        paths.push('src/browser/abort-diagnostic.cpp', 'src/browser/abort-import.js');
      }
      paths.push('src/browser/render-snapshot.h', 'src/browser/render-snapshot.cpp', 'src/browser/menu-backend.h', 'src/browser/menu-backend.cpp', 'src/browser/shader-inventory.h',
        'src/browser/menu-shaders.h', 'src/browser/menu-shaders.cpp', 'src/browser/menu-pixel-shaders.h', 'src/browser/menu-pixel-shaders.cpp', 'src/browser/menu-texture.h', 'src/browser/menu-texture.cpp',
        'src/browser/menu-input.mjs', 'src/browser/gamepad-input.mjs', 'src/browser/input-bindings.mjs', 'src/browser/frame-shape.mjs', 'src/browser/console-model.mjs', 'src/browser/browser-audio.mjs', 'src/browser/audio-worklet.mjs', 'src/browser/webgpu-transport.h', 'src/browser/webgpu-transport.cpp', 'src/browser/webgpu-transport.mjs',
        'src/browser/webgpu-service.cpp', 'src/browser/webgpu-service.mjs', 'src/gpu/smaa_wgsl.h', 'src/gpu/smaa_textures.h', 'src/browser/menu-runtime.mjs', 'src/browser/menu-shell.mjs', 'src/browser/settings.mjs',
        'src/browser/touch-controls.mjs', 'src/browser/save-format.mjs', 'src/browser/save-store.mjs', 'src/browser/save-store-worker.mjs', 'src/browser/save-service.mjs', 'src/browser/manifest.webmanifest', 'src/browser/icon.svg', 'src/browser/icon-180.png', 'src/browser/icon-192.png',
        'src/browser/icon-512.png', 'src/browser/menu.html', 'src/gpu/shaders.h', 'src/gpu/shaders.cpp', 'src/gpu/vertex.h');
    }
    paths.push('src/browser/virtual-memory.cpp', 'src/browser/virtual-memory.h');
    paths.push('src/browser/contiguous-memory.cpp', 'src/browser/contiguous-memory.h');
    paths.push('src/browser/string-provider.cpp', 'src/browser/string-provider.h');
    paths.push('src/browser/section-provider.cpp', 'src/browser/section-provider.h');
    paths.push('src/browser/file-provider.cpp', 'src/browser/file-provider.h', 'src/browser/temporary-filesystem.cpp', 'src/browser/temporary-filesystem.h', 'src/browser/storage-publisher.cpp', 'src/browser/storage-publisher.h');
    paths.push('src/browser/raw-cache.cpp', 'src/browser/raw-cache.h', 'src/browser/cache-fatx.cpp', 'src/browser/cache-fatx.h');
    paths.push('src/browser/kernel-provider.cpp', 'src/browser/kernel-provider.h', 'src/browser/thread-provider.cpp', 'src/browser/thread-provider.h', 'src/kernel/crypto.cpp', 'src/kernel/crypto.h');
    for (const source of ['synchronization-provider', 'dispatcher', 'dispatcher-handles', 'dispatcher-object-abi', 'guest-wait']) {
      paths.push(`src/browser/${source}.cpp`, `src/browser/${source}.h`);
    }
    paths.push('src/browser/boot-wasm.cpp', 'src/browser/disc-wasm.cpp', 'src/browser/image.cpp', 'src/browser/image.h', 'src/kernel/disc_index.cpp', 'src/kernel/disc_index.h', 'src/browser/iso-validation.mjs', 'src/browser/iso-image.mjs');
  }
  paths.push('tools/browser/provenance.mjs', 'tools/browser/tail-call-postlink.mjs');
  const discIdentity = generationRecord ? verifyGeneration(JSON.parse(readFileSync(generationRecord, 'utf8')), read) : {
    image: 'default.xbe', xbeSha256: null, xbeSize: null, requiredStoryFiles: null,
    provenance: 'Generated input hashes captured. Trusted generation XBE identity is absent; real boot is blocked. Never derive trust from the selected ISO.',
  };
  if (configuration.split(';').includes('mission=ON')) {
    paths.push('src/browser/loading-io.cpp', 'src/browser/mission-input.h', 'src/browser/mission-input.cpp', 'src/browser/mission-controls.mjs', 'src/browser/mission-metrics.mjs', 'src/browser/mission-clock.h', 'src/browser/mission-stepper.h');
    if (discIdentity.requiredStoryFiles) {
      discIdentity.requiredStoryFiles.push({ path: 'Default/DataXb/LMaps/x01_Iceland_a/x01_Iceland_a.lin', minBytes: 13681301 });
    }
  }
  const option = (name) => configuration.split(';').find((entry) => entry.startsWith(`${name}=`))?.slice(name.length + 1) ?? '';
  const hooks = option('hooks').split(',').filter(Boolean);
  let hookEntries = [];
  if (configuration.split(';').includes('menu=ON')) {
    const compat = readdirSync(resolve(root, 'src/browser/hook-compat'), { recursive: true }).map(String).filter((name) => name.endsWith('.h')).sort();
    paths.push(...compat.map((name) => `src/browser/hook-compat/${name}`), 'src/browser/hooks.h', 'src/browser/hook-compat.cpp', 'src/browser/hook-log.cpp',
      'src/browser/hook-d3d.cpp', 'src/browser/host-settings.h', 'src/browser/host-settings.cpp',
      'src/browser/input-core.h', 'src/browser/input-core.cpp', 'src/browser/gyro.cpp', 'src/core/gyro.h', 'src/browser/input-update.cpp', 'src/browser/keyboard-input.h',
      'src/browser/keyboard-input.mjs', 'src/browser/input-actions.mjs', 'src/browser/settings-rows.cpp', 'src/browser/host-messages.h',
      'src/browser/host-messages.cpp', 'src/browser/host-messages.mjs', 'src/browser/game-stubs.cpp', 'src/browser/loading-screen.cpp',
      'src/browser/loading-input.cpp', 'src/browser/exit-menu.cpp', 'src/browser/render-scale.h',
      'src/game/loading.h', 'src/gpu/backend.h',
      'src/app/translations.h', 'src/app/translations.cpp', 'src/core/input.h', 'src/game/console.h', 'src/game/unreal.h',
      'src/xdk/hle.h', 'src/xdk/d3d8.h', 'src/app/app.h', 'src/core/log.h');
    if (hooks.length) {
      const sources = Object.fromEntries(hooks.map((name) => [name, read(hookSource(root, name)).toString()]));
      validateHookIncludes({ hooks: sources, shadowed: compat });
      const browserSources = Object.fromEntries([...new Set(paths)].filter((path) => /^src\/browser\/[^/]+\.(cpp|h)$/.test(path)).map((path) => [path, read(path).toString()]));
      hookEntries = validateHookOverrides({ browser: browserSources, hooks: sources, renames: option('hook-renames').split(',').filter(Boolean),
        symbols: read('generated/game/image_symbols.h').toString() });
      paths.push(...hooks.map((name) => hookSource(root, name)), 'src/game/magma.h');
    }
  } else if (hooks.length) {
    throw new Error('Game hooks require menu mode.');
  }
  if (generationRecord) { paths.push(...generationInputs, ...generationOutputs); }
  const inputs = Object.fromEntries([...new Set(paths)].map((path) => [path, hash(read(path))]));
  const manifest = { schema: 1, wasmOptimization, wasmRequirements, compatibility: (configuration.includes('full-cpu=ON') || configuration.includes('boot=ON')) ? 'wasm-compact-leased-v1' : 'wasm-weak-alias-v1', toolchain, configuration, partitionCount: count, functionCount, namedSymbolCount, ranges, ...discIdentity, ...(kernelExports ? { kernelExports } : {}), inputs };
  const identity = hash(JSON.stringify(manifest));
  const emit = (name, content) => {
    const path = resolve(output, name);
    if (!existsSync(path) || readFileSync(path, 'utf8') !== content) { writeFileSync(path, content); }
  };
  emit('input-manifest.json', JSON.stringify({ identity, ...manifest }, null, 2) + '\n');
  emit('wasm-postlink.json', JSON.stringify({ ...wasmOptimization, ...(actualTailCalls ? { tool: resolve(optimizerTool) } : {}) }, null, 2) + '\n');
  if (configuration.includes('boot=ON')) {
    if (!generationRecord) { throw new Error('Boot requires trusted generation evidence'); }
    emit('boot-manifest.mjs', `export default ${JSON.stringify({ identity, ...manifest })};\n`);
  }
  emit('hook_manifest.h', `#pragma once\n#define BROWSER_GAME_HOOKS(X)${hookEntries.map(({ hook, address }) => ` \\\n  X(${hook}, ${address})`).join('')}\n`);
  emit('build_identity.h',`#pragma once\n#define BROWSER_BUILD_ID "${identity}"\n#define BROWSER_FUNCTION_COUNT ${functionCount}u\n#define BROWSER_TOOLCHAIN "${toolchain}"\n`);
}
