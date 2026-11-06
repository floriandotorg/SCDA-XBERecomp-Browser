import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { buildHookContract, hookDirectory } from './hook-contract.mjs';
import { hookSource } from '../../tools/browser/inventory.mjs';

const symbols = `${hookDirectory}/bink`;
mkdirSync(symbols, { recursive: true });
writeFileSync(`${symbols}/image_symbols.h`, '#pragma once\n#define SYM_BinkDoFrame 004BB1B0\n#define SYM_BinkGetSummary 004BABE0\n#define SYM_BinkNextFrame 004BB790\n#define SYM_BinkWait 004BA920\n');

for (const platform of ['native', 'wasm']) {
  for (const memory of ['compact', 'flat']) {
    test(`${platform} ${memory} native bink hook plays movies at their own rate above 30 FPS`, () => {
      const run = buildHookContract({ name: 'bink', platform, memory, includes: [symbols],
        sources: ['tests/browser/bink.cpp', hookSource('.', 'bink')] });
      for (const name of ['movie-pace', 'summary']) { assert.match(run.stdout, new RegExp(`case ${name} ok`), run.stdout + run.stderr); }
      assert.equal(run.status, 0, run.stdout + run.stderr);
      assert.match(run.stdout, /bink-contracts-ok/);
    });
  }
}
