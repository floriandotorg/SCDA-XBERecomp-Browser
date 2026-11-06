import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { buildHookContract, hookDirectory } from './hook-contract.mjs';

const symbols = `${hookDirectory}/magma`;
mkdirSync(symbols, { recursive: true });
writeFileSync(`${symbols}/image_symbols.h`, '#pragma once\n');
const source = 'src/game/magma_time.cpp';

for (const platform of ['native', 'wasm']) {
  for (const memory of ['compact', 'flat']) {
    test(`${platform} ${memory} native magma_time hook hands Magma whole milliseconds that sum to real time`, () => {
      const run = buildHookContract({ name: 'magma-time', platform, memory, includes: [symbols],
        sources: ['tests/browser/magma-time.cpp', ...(existsSync(source) ? [source] : [])] });
      assert.equal(run.status, 0, run.stdout + run.stderr);
      assert.match(run.stdout, /magma ticks=600 sum=10000/);
      assert.match(run.stdout, /magma-time-contracts-ok/);
    });
  }
}
