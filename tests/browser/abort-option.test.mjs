import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { generationInputs, generationOutputs } from '../../tools/browser/provenance.mjs';

const cmake = readFileSync('tools/browser/CMakeLists.txt', 'utf8');
const directory = resolve('out/m4-current/abort-option-tests');
mkdirSync(directory, { recursive: true });
const hash = (bytes) => createHash('sha256').update(bytes).digest('hex');
const options = () => {
  const start = cmake.indexOf('option(BROWSER_MENU_TRACE_ABORTS ');
  assert.ok(start >= 0, 'trusted abort option is absent');
  const end = cmake.indexOf('\nset(BROWSER_MENU_TRACE_FRAMES ', start);
  assert.ok(end > start);
  return cmake.slice(start, end);
};

test('abort tracing defaults OFF and confines compilation diagnostics to cpu_runtime.cpp', () => {
  const script = join(directory, 'admission.cmake');
  writeFileSync(script, `${options()}\nmessage(STATUS "trace-aborts=\${BROWSER_MENU_TRACE_ABORTS}")\n`);
  for (const menu of ['OFF', 'ON']) {
    for (const value of [undefined, 'OFF', 'ON', 'TRUE', '1', 'YES']) {
      const result = spawnSync('cmake', [`-DBROWSER_MENU=${menu}`, ...(value ? [`-DBROWSER_MENU_TRACE_ABORTS=${value}`] : []), '-P', script], { encoding: 'utf8' });
      const enabled = value !== undefined && value !== 'OFF';
      assert.equal(result.status, menu === 'OFF' && enabled ? 1 : 0, result.stderr);
      if (result.status === 0) {
        assert.match(result.stdout, new RegExp(`trace-aborts=${enabled ? 'ON' : 'OFF'}`));
      } else {
        assert.match(result.stderr, /BROWSER_MENU_TRACE_ABORTS requires BROWSER_MENU/);
      }
    }
  }
  assert.match(cmake, /;menu-trace-aborts=\$\{BROWSER_MENU_TRACE_ABORTS\}/);
  const runtimeDefinition = '            set_property(SOURCE ${root}/src/browser/cpu_runtime.cpp APPEND PROPERTY COMPILE_DEFINITIONS BROWSER_MENU_TRACE_ABORTS=1)';
  assert.match(cmake, /if\(BROWSER_MENU_TRACE_ABORTS\)\s+target_sources\(story_boot_link PRIVATE \$\{root\}\/src\/browser\/abort-diagnostic\.cpp\)\s+set_property\(SOURCE \$\{root\}\/src\/browser\/cpu_runtime\.cpp APPEND PROPERTY COMPILE_DEFINITIONS BROWSER_MENU_TRACE_ABORTS=1\)\s+target_link_options\(story_boot_link PRIVATE --profiling-funcs -Wl,--wrap=abort --pre-js \$\{root\}\/src\/browser\/abort-import\.js\)\s+endif\(\)/);
  const compiles = cmake.split('\n').filter((line) => /target_compile|COMPILE_DEFINITIONS/.test(line));
  assert.equal(compiles.filter((line) => line === runtimeDefinition).length, 1);
  assert.doesNotMatch(compiles.filter((line) => line !== runtimeDefinition).join('\n'), /profiling-funcs|wrap=abort|TRACE_ABORTS/);
});

test('real Wasm link flags preserve retained CPU objects across trace and Present-limit changes', () => {
  const fixture = mkdtempSync(join(directory, 'public-cmake-'));
  const source = join(fixture, 'source');
  const build = join(fixture, 'build');
  mkdirSync(join(source, 'src/browser'), { recursive: true });
  writeFileSync(join(source, 'src/browser/abort-diagnostic.cpp'), readFileSync('src/browser/abort-diagnostic.cpp'));
  writeFileSync(join(source, 'src/browser/abort-import.js'), readFileSync('src/browser/abort-import.js'));
  writeFileSync(join(source, 'cpu.cpp'), '#include <emscripten.h>\nextern "C" EMSCRIPTEN_KEEPALIVE int retained_cpu() { return 17; }\n');
  writeFileSync(join(source, 'src/browser/boot-wasm.cpp'), '#include "build_identity.h"\n#include <emscripten.h>\nextern "C" EMSCRIPTEN_KEEPALIVE const char* identity() { return BUILD_IDENTITY; }\n');
  const start = cmake.indexOf('    if(BROWSER_MENU)\n        if(BROWSER_MENU_TRACE_ABORTS)');
  const end = cmake.indexOf('        target_sources(story_boot_link PRIVATE ${root}/src/browser/menu-backend.cpp', start);
  assert.ok(start >= 0 && end > start);
  const integration = `${cmake.slice(start, end)}\nendif()`;
  writeFileSync(join(source, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.25)
project(abort_link_fixture CXX)
set(root "\${CMAKE_CURRENT_SOURCE_DIR}")
set(BROWSER_MENU ON)
${options()}
set(BROWSER_MENU_TRACE_FRAMES "0" CACHE STRING "Public fixture Present limit")
file(WRITE "\${CMAKE_CURRENT_BINARY_DIR}/build_identity.h" "#define BUILD_IDENTITY \\\"aborts=\${BROWSER_MENU_TRACE_ABORTS};frames=\${BROWSER_MENU_TRACE_FRAMES}\\\"\\n")
add_executable(story_boot_link cpu.cpp src/browser/boot-wasm.cpp)
target_include_directories(story_boot_link PRIVATE "\${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(story_boot_link PRIVATE -O3 -pthread -fwasm-exceptions)
target_link_options(story_boot_link PRIVATE -pthread -fwasm-exceptions --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node)
set_target_properties(story_boot_link PROPERTIES SUFFIX .mjs)
${integration}
`);
  const run = (command, arguments_) => {
    const result = spawnSync(command, arguments_, { encoding: 'utf8', timeout: 120000 });
    assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  };
  const configure = (enabled, frames) => {
    run('emcmake', ['cmake', '-G', 'Ninja', '-S', source, '-B', build, '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON', `-DBROWSER_MENU_TRACE_ABORTS=${enabled}`, `-DBROWSER_MENU_TRACE_FRAMES=${frames}`]);
    run('cmake', ['--build', build]);
  };
  configure('OFF', 0);
  const object = join(build, 'CMakeFiles/story_boot_link.dir/cpu.cpp.o');
  const cpuMtime = statSync(object).mtimeMs;
  const cpuHash = hash(readFileSync(object));
  assert.doesNotMatch(readFileSync(join(build, 'build.ninja'), 'utf8'), /--profiling-funcs|--wrap=abort|abort-diagnostic\.cpp\.o/);
  configure('ON', 0);
  assert.equal(statSync(object).mtimeMs, cpuMtime);
  assert.equal(hash(readFileSync(object)), cpuHash);
  assert.match(readFileSync(join(build, 'build.ninja'), 'utf8'), /--profiling-funcs -Wl,--wrap=abort/);
  assert.doesNotMatch(readFileSync(join(build, 'compile_commands.json'), 'utf8'), /--profiling-funcs|--wrap=abort/);
  const helper = join(build, 'CMakeFiles/story_boot_link.dir/src/browser/abort-diagnostic.cpp.o');
  const helperMtime = statSync(helper).mtimeMs;
  configure('ON', 684);
  assert.equal(statSync(object).mtimeMs, cpuMtime);
  assert.equal(hash(readFileSync(object)), cpuHash);
  assert.equal(statSync(helper).mtimeMs, helperMtime);
});

const publicInventoryFixture = () => {
  const fixture = mkdtempSync(join(directory, 'public-inputs-'));
  const put = (path, bytes) => {
    mkdirSync(dirname(join(fixture, path)), { recursive: true });
    writeFileSync(join(fixture, path), bytes);
  };
  const placeholders = (path) => {
    for (const entry of readdirSync(path, { withFileTypes: true })) {
      const name = join(path, entry.name);
      if (entry.isDirectory()) {
        placeholders(name);
      } else {
        put(name, `public fixture ${name}\n`);
      }
    }
  };
  for (const path of ['src', 'tools/browser', 'tools/recomp', 'tests/browser']) {
    placeholders(path);
  }
  for (const path of [...generationInputs, ...generationOutputs, 'assets/default.xbe']) {
    put(path, `public fixture ${path}\n`);
  }
  put('config/scda_v2.toml', '[xbe]\npath = "assets/default.xbe"\npartitions = 128\n[symbols]\nExample = 0x11000\n[data]\n');
  put('generated/game/image_symbols.h', '#define IMAGE_XBE "default.xbe"\n#define SYM_Example 00011000\n#define HAS_Example 1\n');
  put('generated/game/x86_recomp_shared.h', 'X86_EXTERN_FUNC(sub_00011000);\n');
  put('generated/game/x86_recomp.0.cpp', 'X86_FUNC(__imp__sub_00011000) {}\nX86_WEAK_FUNC(sub_00011000, __imp__sub_00011000)\n');
  put('generated/game/x86_func_mapping.cpp', '{0x11000u, sub_00011000},\n');
  const read = (path) => readFileSync(join(fixture, path));
  const record = {
    schema: 1, image: 'default.xbe', xbeSha256: hash(read('assets/default.xbe')), xbeSize: read('assets/default.xbe').length,
    inputs: Object.fromEntries(generationInputs.map((path) => [path, hash(read(path))])),
    comparison: generationOutputs.map((path) => ({ name: path.split('/').at(-1), retained: hash(read(path)), regenerated: hash(read(path)) })),
  };
  put('record.json', JSON.stringify(record));
  return { fixture, put };
};

test('real inventory CLI binds abort option and only active helper bytes to trusted boot provenance', () => {
  const { fixture, put } = publicInventoryFixture();
  const inventory = (configuration, name) => {
    const output = join(fixture, name);
    mkdirSync(output, { recursive: true });
    const result = spawnSync(process.execPath, ['tools/browser/inventory.mjs', fixture, output, 'emcc 6.0.13-git', configuration, join(fixture, 'record.json')], { encoding: 'utf8' });
    assert.equal(result.status, 0, result.stderr);
    const manifest = JSON.parse(readFileSync(join(output, 'input-manifest.json'), 'utf8'));
    assert.equal(manifest.xbeSha256, hash(readFileSync(join(fixture, 'assets/default.xbe'))));
    assert.match(readFileSync(join(output, 'build_identity.h'), 'utf8'), new RegExp(manifest.identity));
    assert.match(readFileSync(join(output, 'boot-manifest.mjs'), 'utf8'), new RegExp(manifest.identity));
    return manifest;
  };
  const config = 'Release;story=OFF;full-cpu=OFF;boot=ON;menu-capture=OFF;menu=ON;menu-trace-frames=0';
  const disabled = inventory(`${config};menu-trace-aborts=OFF`, 'disabled');
  const enabled = inventory(`${config};menu-trace-aborts=ON`, 'enabled');
  const helpers = ['src/browser/abort-diagnostic.cpp', 'src/browser/abort-import.js'];
  for (const helper of helpers) {
    assert.equal(disabled.inputs[helper], undefined);
    assert.ok(enabled.inputs[helper]);
  }
  assert.notEqual(enabled.identity, disabled.identity);
  assert.deepEqual(Object.keys(enabled.inputs).filter((path) => !(path in disabled.inputs)), helpers);
  for (const [n, helper] of helpers.entries()) {
    put(helper, 'changed public diagnostic fixture\n');
    assert.equal(inventory(`${config};menu-trace-aborts=OFF`, `disabled-changed-${n}`).identity, disabled.identity);
    assert.notEqual(inventory(`${config};menu-trace-aborts=ON`, `enabled-changed-${n}`).identity, enabled.identity);
  }
  const capture = inventory('Release;story=OFF;boot=ON;menu=OFF;menu-capture=ON;menu-trace-aborts=OFF', 'capture');
  for (const helper of helpers) { assert.equal(capture.inputs[helper], undefined); }
});
