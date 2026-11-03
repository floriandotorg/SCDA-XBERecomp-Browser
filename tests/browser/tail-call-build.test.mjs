import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, renameSync, statSync, utimesSync, writeFileSync } from 'node:fs';
import { join, resolve } from 'node:path';
import { optimizerIdentity } from '../../tools/browser/tail-call-postlink.mjs';

const cmake = readFileSync('tools/browser/CMakeLists.txt', 'utf8');
const parent = resolve('out/m4-current/tail-build-tests');
mkdirSync(parent, { recursive: true });
const run = (command, args) => {
  const result = spawnSync(command, args, { encoding: 'utf8', timeout: 120000 });
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  return result;
};

test('tail optimization is opt-in, requires boot, and tracks actual tool/configuration inputs', () => {
  const start = cmake.indexOf('option(BROWSER_ACTUAL_TAIL_CALLS ');
  const end = cmake.indexOf('\noption(BROWSER_MENU_TRACE_ABORTS ', start);
  assert.ok(start >= 0 && end > start);
  const block = cmake.slice(start, end);
  const script = join(parent, 'options.cmake');
  writeFileSync(script, `${block}\nmessage(STATUS "tail=\${BROWSER_ACTUAL_TAIL_CALLS}")\n`);
  assert.match(run('cmake', ['-P', script]).stdout, /tail=OFF/);
  const invalid = spawnSync('cmake', ['-DBROWSER_ACTUAL_TAIL_CALLS=ON', '-P', script], { encoding: 'utf8' });
  assert.notEqual(invalid.status, 0);
  assert.match(invalid.stderr, /requires BROWSER_BOOT/);
  assert.match(cmake, /actual-tail-calls=\$\{BROWSER_ACTUAL_TAIL_CALLS\}/);
  assert.match(cmake, /CMAKE_CONFIGURE_DEPENDS.*\$\{BROWSER_WASM_OPT\}/);
  assert.match(cmake, /wasm-capabilities\.mjs/);
  assert.doesNotMatch(cmake, /target_link_options\(menu_gpu[^)]*tail-call/s);
  assert.doesNotMatch(cmake, /configure_file\([^\n]*boot-manifest\.mjs[^\n]*boot-web/);
});

for (const generator of ['Ninja', 'Unix Makefiles']) {
  test(`${generator} production graph preserves complete publications through failed toggles and recovers missing outputs`, () => {
    const start = cmake.indexOf('function(browser_publish_boot)');
    const end = cmake.indexOf('\nendfunction()', start);
    assert.ok(start >= 0 && end > start, 'production postlink function is absent');
    const block = cmake.slice(start, end + '\nendfunction()'.length);
    assert.match(block, /add_custom_command\(OUTPUT/);
    const fixture = mkdtempSync(join(parent, 'graph-'));
    const source = join(fixture, 'source');
    const build = join(fixture, 'build');
    mkdirSync(source);
    const tool = join(source, 'wasm-opt');
    const fail = join(fixture, 'fail-optimizer');
    const writeTool = (suffix = '') => {
      writeFileSync(tool, `#!/usr/bin/env node
const fs = require('node:fs');
if (process.argv.includes('--version')) { console.log('wasm-opt version 133'); process.exit(0); }
fs.appendFileSync(${JSON.stringify(join(fixture, 'optimizer-runs'))}, 'run\\n');
fs.writeFileSync(process.argv[process.argv.indexOf('-o') + 1], Buffer.concat([fs.readFileSync(process.argv[2]), Buffer.from([0, 2, 1, 116])]));
if (fs.existsSync(${JSON.stringify(fail)})) { process.exit(7); }
${suffix}
`);
      chmodSync(tool, 0o755);
    };
    writeTool();
    const config = join(source, 'wasm-postlink.json');
    const manifest = join(source, 'boot-manifest.mjs');
    const inputs = (mode, label) => {
      writeFileSync(config, JSON.stringify(mode === 'on' ? { ...optimizerIdentity(tool), tool } : { mode: 'off' }));
      writeFileSync(manifest, `export default ${JSON.stringify({ identity: label, wasmRequirements: mode === 'on' ? ['tail-call'] : [] })};\n`);
    };
    inputs('off', 'original-off');
    writeFileSync(join(source, 'guest.cpp'), '#include <emscripten.h>\nextern "C" EMSCRIPTEN_KEEPALIVE int guest() { return 7; }\n');
    writeFileSync(join(source, 'CMakeLists.txt'), `cmake_minimum_required(VERSION 3.25)
project(tail_graph CXX)
set(NODE_EXECUTABLE "${process.execPath}")
set(root "${resolve('.')}")
set(BROWSER_WASM_OPT "${tool}")
set(BROWSER_ACTUAL_TAIL_CALLS ON)
configure_file("${config}" "\${CMAKE_CURRENT_BINARY_DIR}/wasm-postlink.json" COPYONLY)
configure_file("${manifest}" "\${CMAKE_CURRENT_BINARY_DIR}/boot-manifest.mjs" COPYONLY)
add_executable(story_boot_link guest.cpp)
target_link_options(story_boot_link PRIVATE --no-entry -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=node)
if(FAIL_LINK)
  target_link_options(story_boot_link PRIVATE -Wl,--not-a-real-linker-option)
endif()
${block}
browser_publish_boot()
`);
    const configure = (failLink = false) => run('emcmake', ['cmake', '-G', generator, '-S', source, '-B', build, `-DFAIL_LINK=${failLink ? 'ON' : 'OFF'}`]);
    const buildTarget = () => run('cmake', ['--build', build, '--target', 'story_boot']);
    const failedBuild = () => {
      const result = spawnSync('cmake', ['--build', build, '--target', 'story_boot'], { encoding: 'utf8', timeout: 120000 });
      assert.notEqual(result.status, 0, `${result.stdout}\n${result.stderr}`);
    };
    const final = join(build, 'boot-web/story_boot.wasm');
    const raw = join(build, 'boot-link/story_boot.wasm');
    const published = () => ['story_boot.wasm', 'story_boot.mjs', 'boot-manifest.mjs'].map((name) => readFileSync(join(build, 'boot-web', name)));
    const equalizePublicationTimes = () => {
      const time = statSync(join(build, 'boot-link/story_boot.mjs')).mtime;
      for (const path of ['wasm-postlink.json', 'boot-manifest.mjs', 'boot-web/story_boot.wasm', 'boot-web/story_boot.mjs', 'boot-web/boot-manifest.mjs']) {
        utimesSync(join(build, path), time, time);
      }
      assert.equal(existsSync(join(build, 'boot-publication.complete')), false);
    };
    const checkMode = (mode, label) => {
      assert.deepEqual(readFileSync(final), mode === 'on' ? Buffer.concat([readFileSync(raw), Buffer.from([0, 2, 1, 116])]) : readFileSync(raw));
      assert.equal(readFileSync(join(build, 'boot-web/boot-manifest.mjs'), 'utf8'), `export default ${JSON.stringify({ identity: label, wasmRequirements: mode === 'on' ? ['tail-call'] : [] })};\n`);
      assert.equal(readFileSync(join(build, 'boot-web/story_boot.mjs'), 'utf8'), readFileSync(join(build, 'boot-link/story_boot.mjs'), 'utf8'));
    };
    configure();
    buildTarget();
    checkMode('off', 'original-off');
    const original = published();
    inputs('on', 'tail-on');
    writeFileSync(fail, 'fail');
    configure();
    equalizePublicationTimes();
    assert.deepEqual(published(), original);
    failedBuild();
    assert.deepEqual(published(), original);
    renameSync(fail, `${fail}.disabled`);
    buildTarget();
    checkMode('on', 'tail-on');
    const optimized = published();
    const stable = statSync(final).mtimeMs;
    buildTarget();
    assert.equal(statSync(final).mtimeMs, stable);
    inputs('off', 'recovered-off');
    configure(true);
    equalizePublicationTimes();
    assert.deepEqual(published(), optimized);
    failedBuild();
    assert.deepEqual(published(), optimized);
    configure();
    buildTarget();
    checkMode('off', 'recovered-off');
    for (const name of ['story_boot.wasm', 'story_boot.mjs', 'boot-manifest.mjs']) {
      const path = join(build, 'boot-web', name);
      renameSync(path, `${path}.saved`);
      buildTarget();
      assert.equal(existsSync(path), true);
      checkMode('off', 'recovered-off');
    }
    inputs('on', 'relinked-on');
    configure();
    writeFileSync(join(source, 'guest.cpp'), '#include <emscripten.h>\nextern "C" EMSCRIPTEN_KEEPALIVE int guest() { return 8; }\n');
    buildTarget();
    checkMode('on', 'relinked-on');
    writeTool('void 1;');
    inputs('on', 'changed-tool');
    configure();
    buildTarget();
    checkMode('on', 'changed-tool');
    const runs = readFileSync(join(fixture, 'optimizer-runs'), 'utf8');
    assert.ok(runs.trim().split('\n').length >= 4);
  });
}
