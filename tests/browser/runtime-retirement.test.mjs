import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/m4-current/runtime-retirement-tests');
mkdirSync(`${directory}/tmp`, { recursive: true });
const environment = { ...process.env, TMPDIR: `${directory}/tmp` };

for (const platform of ['native', 'native-asan', 'wasm']) {
  test(`${platform} join failure diagnostics preserve original failure and joinable destructor abort`, () => {
    for (const traced of [false, true]) {
      const name = `${platform}-${traced ? 'traced' : 'opaque'}`;
      const output = `${directory}/${name}${platform === 'wasm' ? '.mjs' : ''}`;
      const flags = platform === 'wasm' ? ['-fwasm-exceptions', '-Wl,--wrap=pthread_join', '-sPTHREAD_POOL_SIZE=2', '-sPROXY_TO_PTHREAD=1', '-sEXIT_RUNTIME=1', '-sENVIRONMENT=node', '-sINITIAL_MEMORY=134217728'] : platform === 'native-asan' ? ['-fsanitize=address', '-fno-omit-frame-pointer'] : [];
      const build = spawnSync(platform === 'wasm' ? 'em++' : 'clang++', ['-std=c++20', '-O1', '-pthread', '-Isrc', '-DX86_PORTABLE_RUNTIME', ...(traced ? ['-DBROWSER_MENU_TRACE_ABORTS=1'] : []), 'tests/browser/runtime-retirement.cpp', 'src/browser/cpu_runtime.cpp', ...flags, '-o', output], { encoding: 'utf8', timeout: 120000, env: environment });
      assert.equal(build.status, 0, build.stderr);
      for (const phase of ['join', 'launch']) {
        const runner = `${directory}/${name}-${phase}-run.mjs`;
        if (platform === 'wasm') {
          writeFileSync(runner, `import create from './${name}.mjs'; await create({ arguments: ['${phase}'] });\n`);
        }
        const run = spawnSync(platform === 'wasm' ? process.execPath : output, platform === 'wasm' ? [runner] : [phase], { encoding: 'utf8', timeout: 20000, env: environment });
        const log = `${run.stdout ?? ''}${run.stderr ?? ''}`;
        writeFileSync(`${directory}/${name}-${phase}.log`, log);
        assert.notEqual(run.status, 0, log);
        if (traced) {
          assert.match(log, new RegExp(`\\[browser-worker-failure\\] phase=${phase === 'join' ? 'join' : 'launch-retire'} .*what=.{1,150}`));
          assert.match(log, /uncaught=0 caught=1/);
          if (phase === 'join') {
            assert.match(log, /phase=destroy-joinable .*uncaught=1 caught=0/);
          } else {
            assert.match(log, /\[retirement-original\].+/);
            assert.equal(run.status, 73, log);
          }
          const diagnostics = log.split('\n').filter((line) => line.startsWith('[browser-worker-failure]'));
          assert.ok(diagnostics.length <= 12);
          assert.ok(diagnostics.every((line) => line.length <= 511));
        } else {
          assert.doesNotMatch(log, /\[browser-worker-failure\]/);
        }
      }
    }
  });
}
