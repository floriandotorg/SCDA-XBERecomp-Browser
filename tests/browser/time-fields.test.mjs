import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/time-fields-tests');
mkdirSync(directory, { recursive: true });
const source = readFileSync('src/kernel/xboxkrnl.cpp', 'utf8');
const body = source.match(/KERNEL_EXPORT\(305, RtlTimeToTimeFields, 2\) \{([\s\S]*?)\n\}/)?.[1];
assert.ok(body, 'native ordinal 305 reference body is present');
writeFileSync(`${directory}/native-time-fields.h`, `#pragma once
inline uint32_t NativeTimeToTimeFields(uint64_t value, std::array<int16_t, 8>& fields) {
  const std::array<uint32_t, 2> a = {0, 0};
  const auto R64 = [&](uint32_t) { return value; };
  const auto W16 = [&](uint32_t address, uint16_t field) { fields.at(address / 2) = std::bit_cast<int16_t>(field); };
${body}
}
`);
writeFileSync(`${directory}/run.mjs`, "import create from './time-fields.mjs'; await create();\n");
for (const platform of ['native', 'wasm']) {
  test(`${platform} checked ordinal 305 converts supplied signed NT time to all UTC fields`, () => {
    const wasm = platform === 'wasm';
    const output = `${directory}/${wasm ? 'time-fields.mjs' : 'native'}`;
    const flags = wasm
      ? ['-fwasm-exceptions', '-sMODULARIZE=1', '-sEXPORT_ES6=1', '-sENVIRONMENT=node', '-sFILESYSTEM=0', '-sINITIAL_MEMORY=268435456', '-sPTHREAD_POOL_SIZE=3', '-sPTHREAD_POOL_SIZE_STRICT=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1']
      : ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'];
    const build = spawnSync(wasm ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-g', '-pthread',
      '-DX86_PORTABLE_RUNTIME', '-DX86_COMPACT_MEMORY', '-Isrc', `-I${directory}`, 'tests/browser/time-fields.cpp',
      'src/browser/timer-provider.cpp', 'src/browser/dpc-provider.cpp', 'src/browser/image.cpp', 'src/browser/cpu_runtime.cpp',
      ...flags, '-o', output], { encoding: 'utf8', timeout: 120000 });
    assert.equal(build.status, 0, build.stderr);
    for (const timezone of ['UTC', 'America/Los_Angeles', 'Asia/Kolkata']) {
      const run = spawnSync(wasm ? process.execPath : output, wasm ? [`${directory}/run.mjs`] : [], {
        encoding: 'utf8', timeout: 30000, env: { ...process.env, TZ: timezone },
      });
      assert.equal(run.status, 0, `${timezone}\n${run.stdout}\n${run.stderr}`);
      assert.match(run.stdout, /time-fields-contracts-ok/);
    }
  });
}
