import assert from 'node:assert/strict';
import { readFileSync, existsSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { spawnSync } from 'node:child_process';

const build = resolve(process.argv[2] ?? 'out/browser');
const manifest = JSON.parse(readFileSync(`${build}/tools/browser/input-manifest.json`, 'utf8'));
const objectRoot = `${build}/tools/browser/CMakeFiles/story_objects.dir/__/__/generated/game`;
const objects = [...Array.from({ length: manifest.partitionCount }, (_, n) => `${objectRoot}/x86_recomp.${n}.cpp.o`), `${objectRoot}/x86_func_mapping.cpp.o`];
for (const path of objects) {
  assert.ok(existsSync(path), `Missing object: ${path}`);
}
const nm = spawnSync(process.argv[3] ?? 'llvm-nm', ['--undefined-only', ...objects], { encoding: 'utf8', maxBuffer: 16 * 1024 * 1024 });
assert.equal(nm.status, 0, nm.stderr);
const references = [...new Set([...nm.stdout.matchAll(/^\s+U (.+)$/gm)].map((match) => match[1]))].filter((name) => !/^(?:__imp__)?sub_[\dA-F]{8}$/.test(name)).sort();
assert.ok(!references.some((name) => /GetProcAddress|LoadLibrary|Win32|WSA|XAudio|D3D12/.test(name)));
const dependencies = {
  diagnosticPlaceholders: references.filter((name) => /x86_/.test(name)),
  portableToolchain: references.filter((name) => !/x86_/.test(name)),
};
const bytes = readFileSync(`${build}/web/story_diagnostic.wasm`);
const compiled = await WebAssembly.compile(bytes);
const imports = WebAssembly.Module.imports(compiled);
assert.ok(!imports.some(({ name }) => /GetProcAddress|LoadLibrary|Win32|WSA|XAudio|D3D12/.test(name)));
const messages = [];
const { default: createDiagnostic } = await import(pathToFileURL(`${build}/web/story_diagnostic.mjs`).href);
const diagnostic = await createDiagnostic({ print: (text) => messages.push(text) });
assert.equal(diagnostic._diagnostic_mapping_count(), manifest.functionCount);
assert.equal(diagnostic._diagnostic_mapping_guest(0), manifest.ranges.X86CodeBase);
assert.ok(diagnostic._diagnostic_mapping_guest(manifest.functionCount - 1) < manifest.ranges.X86CodeEnd);
assert.equal(diagnostic._diagnostic_mapping_guest(manifest.functionCount), 0);
assert.equal(diagnostic._diagnostic_placeholder(), 1);
assert.ok(messages.some((line) => line.includes('Placeholder: x86_kernel address=0x00000123: M3 kernel ordinal unavailable')));
assert.ok(messages.some((line) => line.includes(manifest.identity)));
const report = { identity: manifest.identity, partitionObjects: objects.length - 1, retainedFunctions: manifest.functionCount, wasmBytes: bytes.length, objectHostReferences: references, dependencies, wasmImports: imports, messages };
writeFileSync(`${build}/verification.json`, JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify({ identity: manifest.identity, partitionObjects: report.partitionObjects, retainedFunctions: report.retainedFunctions, wasmBytes: report.wasmBytes, report: `${build}/verification.json` }));
