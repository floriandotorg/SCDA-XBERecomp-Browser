import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { buildHookContract, hookDirectory } from './hook-contract.mjs';
import { hookSource } from '../../tools/browser/inventory.mjs';

const symbols = `${hookDirectory}/glow`;
mkdirSync(symbols, { recursive: true });
writeFileSync(`${symbols}/image_symbols.h`, '#pragma once\n');

for (const memory of ['compact', 'flat']) {
  test(`native ${memory} glow hook moves the chain levels half a texel as native does, the screen copy stays`, () => {
    const run = buildHookContract({ name: 'glow-hook', platform: 'native', memory, includes: [symbols],
      sources: ['tests/browser/glow-hook.cpp', hookSource('.', 'glow'), 'src/browser/hook-d3d.cpp', 'src/browser/hook-graphics.cpp'] });
    assert.equal(run.status, 0, run.stdout + run.stderr);
    assert.match(run.stdout, /glow-hook-contracts-ok/);
  });
}
