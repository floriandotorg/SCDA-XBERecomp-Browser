import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { existsSync, lstatSync, mkdirSync, readFileSync, realpathSync, statSync, writeFileSync } from 'node:fs';
import { dirname, relative, resolve, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
export const defaultRenderTables = [
  { name: 'renderStates57Through165', address: 0x004a9850, count: 109, width: 4 },
  { name: 'textureStageDefaults', address: 0x004a9830, count: 32, width: 1 },
  { name: 'simpleRenderStateEncode', address: 0x004f54e8, count: 92, width: 4 },
];

const helper = `#include "xbe/xbe.h"
#include <cstdio>
#include <iostream>
#include <iterator>
int main(int argc, char** argv) {
  if (argc < 5 || (argc - 2) % 3) { return 1; }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(std::cin)), {});
  xbe::Image image;
  std::string error;
  if (!image.Parse(std::move(bytes), error)) { std::fprintf(stderr, "%s\\n", error.c_str()); return 2; }
  std::printf("[");
  for (int n = 2; n < argc; n += 3) {
    const uint64_t address = std::stoull(argv[n]);
    const uint64_t count = std::stoull(argv[n + 1]);
    const uint64_t width = std::stoull(argv[n + 2]);
    if (n != 2) { std::printf(","); }
    std::printf("[");
    for (uint64_t n = 0; n < count; ++n) {
      uint32_t value = 0;
      const uint64_t position = address + n * width;
      if (position + width > (uint64_t(1) << 32)) { return 3; }
      if (width == 4) {
        if (!image.Read32(uint32_t(position), value)) { std::fprintf(stderr, "table is not file-backed\\n"); return 4; }
      } else {
        const auto offset = image.Offset(uint32_t(position));
        if (offset < 0) { std::fprintf(stderr, "table is not file-backed\\n"); return 4; }
        value = image.file[size_t(offset)];
      }
      if (n) { std::printf(","); }
      std::printf("%u", value);
    }
    std::printf("]");
  }
  std::printf("]\\n");
}
`;

const outputInBuildDirectory = (path) => {
  const output = resolve(root, path);
  const directory = resolve(root, 'out');
  const inside = relative(directory, output);
  if (!inside || inside.startsWith(`..${sep}`) || inside === '..' || resolve(output) === directory) {
    throw new Error('Render-state extraction output must remain inside out/.');
  }
  let cursor = directory;
  for (const part of inside.split(sep)) {
    cursor = resolve(cursor, part);
    if (existsSync(cursor) && lstatSync(cursor).isSymbolicLink()) {
      throw new Error('Render-state extraction output cannot use symlinks in out/.');
    }
  }
  if (existsSync(directory) && realpathSync(directory) !== directory) {
    throw new Error('Render-state extraction requires a real out/ directory.');
  }
  return output;
};

export const extractRenderState = ({ imagePath = 'assets/default.xbe', outputPath = 'out/browser/render-state-defaults.json', expectedSha256, tables = defaultRenderTables } = {}) => {
  if (typeof expectedSha256 !== 'string' || !/^[0-9a-f]{64}$/i.test(expectedSha256)) {
    throw new Error('An independently trusted SHA-256 hash is required.');
  }
  const output = outputInBuildDirectory(outputPath);
  const source = resolve(root, imagePath);
  if (source === output || (existsSync(output) && statSync(output).dev === statSync(source).dev && statSync(output).ino === statSync(source).ino)) {
    throw new Error('Extraction cannot overwrite its original image.');
  }
  if (statSync(source).size > 64 * 1024 * 1024) { throw new Error('XBE exceeds extraction size limit.'); }
  const bytes = readFileSync(source);
  const sourceSha256 = createHash('sha256').update(bytes).digest('hex');
  if (sourceSha256 !== expectedSha256.toLowerCase()) { throw new Error('XBE does not match trusted hash.'); }
  if (!Array.isArray(tables) || !tables.length || tables.length > 16) { throw new Error('Invalid extraction table count.'); }
  const names = new Set();
  for (const table of tables) {
    if (!table || typeof table.name !== 'string' || !/^[a-zA-Z][a-zA-Z0-9]*$/.test(table.name) || names.has(table.name) ||
        !Number.isSafeInteger(table.address) || table.address < 0 || table.address > 0xffffffff ||
        !Number.isSafeInteger(table.count) || table.count < 1 || table.count > 65536 ||
        ![1, 4].includes(table.width) || table.address + table.count * table.width > 0x100000000) {
      throw new Error('Invalid extraction table extent or name.');
    }
    names.add(table.name);
  }
  const build = outputInBuildDirectory('out/render-state-extractor/parser.cpp');
  const binary = outputInBuildDirectory('out/render-state-extractor/parser');
  mkdirSync(dirname(build), { recursive: true });
  writeFileSync(build, helper);
  const compile = spawnSync('clang++', ['-std=c++20', '-O2', `-I${resolve(root, 'src')}`, build, '-o', binary], { encoding: 'utf8', timeout: 120000 });
  if (compile.status !== 0) { throw new Error(`XBE parser build failed: ${compile.stderr}`); }
  const run = spawnSync(binary, ['stdin', ...tables.flatMap((table) => [String(table.address), String(table.count), String(table.width)])], { input: bytes, encoding: 'utf8', timeout: 30000, maxBuffer: 16 * 1024 * 1024 });
  if (run.status !== 0) { throw new Error(`XBE extraction failed: ${run.stderr}`); }
  const values = JSON.parse(run.stdout);
  const record = {
    schema: 1,
    sourceSha256,
    sourceBytes: bytes.length,
    stateObserved: false,
    tables: Object.fromEntries(tables.map((table, n) => [table.name, { ...table, values: values[n] }])),
  };
  mkdirSync(dirname(output), { recursive: true });
  writeFileSync(output, `${JSON.stringify(record, null, 2)}\n`);
  return record;
};

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const [expectedSha256, outputPath, imagePath] = process.argv.slice(2);
    const record = extractRenderState({ expectedSha256, ...(outputPath ? { outputPath } : {}), ...(imagePath ? { imagePath } : {}) });
    process.stdout.write(`Extracted ${Object.keys(record.tables).length} build-time tables from verified original image; no runtime state observed.\n`);
  } catch (error) {
    process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    process.exitCode = 1;
  }
}
