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

test('a guest function overridden twice across browser and hook sources fails unless the hook is renamed', async () => {
  const { validateHookOverrides } = await import('../../tools/browser/inventory.mjs');
  assert.equal(typeof validateHookOverrides, 'function');
  const symbols = '#define SYM_D3DDevice_Swap 0049CC50\n#define SYM_XNetStartup 00F11000\n';
  const browser = {
    'src/browser/mission-input.cpp': 'X86_FUNC(__imp__sub_00039AD4);\nX86_FUNC(sub_00039AD4) {\n}\n',
    'src/browser/sdk-provider.cpp': 'X86_FUNC(SDK_GUEST(XNetStartup)) {\n}\n',
    'src/browser/diagnostic-graphics.cpp': 'DG_BOUNDARY(D3DDevice_Swap, 16)\n',
  };
  const intro = 'X86_EXTERN_FUNC(__imp__sub_00039AD4);\nX86_FUNC(sub_00039AD4) {\n}\nX86_FUNC(sub_00037274) {\n}\n';
  assert.throws(() => validateHookOverrides({ browser, hooks: { intro }, renames: [], symbols }), /sub_00039AD4.*intro/);
  assert.deepEqual(validateHookOverrides({ browser, hooks: { intro }, renames: ['00039AD4'], symbols }),
    [{ hook: 'intro', address: '00037274', renamed: false }, { hook: 'intro', address: '00039AD4', renamed: true }]);
  assert.throws(() => validateHookOverrides({ browser, hooks: { swap: 'HLE_STDCALL(D3DDevice_Swap, 1) {\n}\n' }, renames: [], symbols }), /sub_0049CC50.*swap/);
  assert.throws(() => validateHookOverrides({ browser, hooks: { net: 'X86_FUNC(GUEST(XNetStartup)) {\n}\n' }, renames: [], symbols }), /sub_00F11000.*net/);
  assert.throws(() => validateHookOverrides({ browser, hooks: { a: 'X86_FUNC(sub_00011000) {}\n', b: 'X86_FUNC(sub_00011000) {}\n' }, renames: ['00011000'], symbols }), /sub_00011000/);
  assert.throws(() => validateHookOverrides({ browser, hooks: { a: 'X86_FUNC(GUEST(Missing)) {}\n' }, renames: [], symbols }), /Missing/);
});

test('a selected game hook may only include headers the browser provides', async () => {
  const { validateHookIncludes } = await import('../../tools/browser/inventory.mjs');
  assert.equal(typeof validateHookIncludes, 'function');
  const allowed = '#include <cmath>\n#include "xdk/hle.h"\n#include "app/app.h"\n#include "core/log.h"\n#include "game/magma.h"\n#include "gpu/state.h"\n#include "xdk/d3d8.h"\n#include "kernel/kernel.h"\n';
  assert.equal(validateHookIncludes({ hooks: { intro: allowed }, shadowed: ['kernel/kernel.h', 'xdk/d3d8.h'] }), 1);
  assert.throws(() => validateHookIncludes({ hooks: { mouse: '#include "core/input.h"\n' }, shadowed: [] }), /mouse.*core\/input\.h/);
  assert.throws(() => validateHookIncludes({ hooks: { d3d: '#include "xdk/d3d8.h"\n#include "kernel/memory.h"\n' }, shadowed: ['xdk/d3d8.h'] }), /kernel\/memory\.h/);
});
