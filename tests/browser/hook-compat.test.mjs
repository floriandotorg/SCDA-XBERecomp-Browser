import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { buildHookContract, hookDirectory } from './hook-contract.mjs';

const symbols = `${hookDirectory}/compat`;
mkdirSync(symbols, { recursive: true });
writeFileSync(`${symbols}/image_symbols.h`, '#pragma once\n#define SYM_TestAdd 00020000\n#define SYM_TestScale 00020010\n');

for (const platform of ['native', 'wasm']) {
  for (const memory of ['compact', 'flat']) {
    test(`${platform} ${memory} hook helpers keep native names over checked browser memory`, () => {
      const run = buildHookContract({ name: 'hook-compat', platform, memory, sources: ['tests/browser/hook-compat.cpp'], includes: [symbols] });
      assert.equal(run.status, 0, run.stdout + run.stderr);
      assert.match(run.stdout, /hook-compat-contracts-ok/);
    });
  }
}
