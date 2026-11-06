import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { buildHookContract, hookDirectory } from './hook-contract.mjs';
import { hookSource } from '../../tools/browser/inventory.mjs';

const symbols = `${hookDirectory}/high-fps`;
mkdirSync(symbols, { recursive: true });
writeFileSync(`${symbols}/image_symbols.h`, '#pragma once\n');
const cases = ['camera-shake', 'root-motion', 'texture-tick', 'jba-glitch', 'interference', 'grain', 'cloth'];

for (const platform of ['native', 'wasm']) {
  for (const memory of ['compact', 'flat']) {
    test(`${platform} ${memory} native high FPS hooks step camera effects, cloth, textures and grain at the console's 30 Hz`, () => {
      const run = buildHookContract({ name: 'high-fps-hooks', platform, memory, includes: [symbols],
        sources: ['tests/browser/high-fps-hooks.cpp', ...['fpsfixes', 'camera_shake', 'root_motion'].map(name => hookSource('.', name))] });
      for (const name of cases) { assert.match(run.stdout, new RegExp(`case ${name} ok`), run.stdout + run.stderr); }
      assert.equal(run.status, 0, run.stdout + run.stderr);
      assert.match(run.stdout, /high-fps-hooks-contracts-ok/);
    });
  }
}
