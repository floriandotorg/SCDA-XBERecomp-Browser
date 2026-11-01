import { test } from 'node:test';
import assert from 'node:assert/strict';
import { validateImageSymbols, validateSymbols } from '../../tools/browser/inventory.mjs';

test('symbol inventory rejects omitted bodies, duplicates and mismatched map entries', () => {
  const declarations = 'X86_EXTERN_FUNC(sub_00011000);\n';
  const body = 'X86_FUNC(__imp__sub_00011000) {}\nX86_WEAK_FUNC(sub_00011000, __imp__sub_00011000)\n';
  const mapping = '{0x11000u, sub_00011000},\n';
  assert.equal(validateSymbols(declarations, [body], mapping), 1);
  assert.throws(() => validateSymbols(declarations, [], mapping), /body/);
  assert.throws(() => validateSymbols(declarations, [body, body], mapping), /duplicate/);
  assert.throws(() => validateSymbols(declarations, [body], '{0x11000u, sub_00011001},'), /mapping/);
  assert.throws(() => validateSymbols(declarations, [body], mapping + mapping), /duplicate/);
  assert.throws(() => validateSymbols(declarations, [body], '{0x11001u, sub_00011000},'), /address/);
});

test('configuration image identity and named addresses agree with generated metadata', () => {
  const config = '[xbe]\npath = "assets/default.xbe"\n[symbols]\nExample = 0x11000\nMissing = 0\n[data]\n';
  const header = '#define IMAGE_XBE "default.xbe"\n#define SYM_Example 00011000\n#define HAS_Example 1\n#define SYM_Missing FFFF0000\n#define HAS_Missing 0\n';
  assert.equal(validateImageSymbols(config, header), 2);
  assert.throws(() => validateImageSymbols(config, header.replace('default.xbe', 'sc4_adv.xbe')), /image/);
  assert.throws(() => validateImageSymbols(config, header.replace('00011000', '00011001')), /Example/);
  assert.throws(() => validateImageSymbols(config, header.replace('#define HAS_Missing 0', '')), /Missing/);
});
