import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { buildHookContract, hookDirectory } from './hook-contract.mjs';
import { hookSource } from '../../tools/browser/inventory.mjs';

const symbols = `${hookDirectory}/crt-math`;
mkdirSync(symbols, { recursive: true });
writeFileSync(`${symbols}/image_symbols.h`, '#pragma once\n#define SYM__CIacos 0036872C\n#define SYM__CIasin 003687F8\n#define SYM_crt_pow 00368348\n');

test('browser hooks resolve from src/game first, then src/xdk', () => {
  assert.equal(hookSource('.', 'magma_time'), 'src/game/magma_time.cpp');
  assert.equal(hookSource('.', 'crt_math'), 'src/xdk/crt_math.cpp');
  assert.throws(() => hookSource('.', 'missing_hook'), /missing_hook/);
});

for (const platform of ['native', 'wasm']) {
  for (const memory of ['compact', 'flat']) {
    test(`${platform} ${memory} crt_math replaces the recompiled acos, asin and pow with host math`, () => {
      const run = buildHookContract({ name: 'crt-math', platform, memory, includes: [symbols],
        sources: ['tests/browser/crt-math.cpp', hookSource('.', 'crt_math')] });
      assert.equal(run.status, 0, run.stdout + run.stderr);
      assert.match(run.stdout, /crt-math-contracts-ok/);
    });
  }
}
