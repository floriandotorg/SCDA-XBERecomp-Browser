import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { buildHookContract, hookDirectory } from './hook-contract.mjs';

const symbols = `${hookDirectory}/intro`;
mkdirSync(symbols, { recursive: true });
writeFileSync(`${symbols}/image_symbols.h`, '#pragma once\n');
const source = 'src/game/intro.cpp';

for (const platform of ['native', 'wasm']) {
  for (const memory of ['compact', 'flat']) {
    test(`${platform} ${memory} native intro hook skips the logos video and rating screen through the settings block`, () => {
      const run = buildHookContract({ name: 'intro', platform, memory, includes: [symbols], definitions: ['-Dsub_00039AD4=game_sub_00039AD4'],
        sources: ['tests/browser/intro.cpp', 'src/browser/host-settings.cpp', ...(existsSync(source) ? [source] : [])] });
      assert.equal(run.status, 0, run.stdout + run.stderr);
      assert.match(run.stdout, /intro-contracts-ok/);
      assert.match(run.stderr, /^\[game\] intro: skipped the logos video$/m);
      assert.match(run.stderr, /^\[game\] intro: skipped the rating screen$/m);
    });
  }
}
