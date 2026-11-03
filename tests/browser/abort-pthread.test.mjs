import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';

const directory = resolve('out/m4-current/abort-pthread-tests');
mkdirSync(`${directory}/tmp`, { recursive: true });
const environment = { ...process.env, TMPDIR: `${directory}/tmp` };

test('actual SDK pthread trap precedes crash-signal main Join unwind and is preserved before unchanged abort', () => {
  for (const traced of [false, true]) {
    const name = traced ? 'traced' : 'opaque';
    const output = `${directory}/${name}.mjs`;
    const build = spawnSync('em++', ['-std=c++20', '-O3', '-pthread', '-fwasm-exceptions', '-Isrc', '-DX86_PORTABLE_RUNTIME',
      'tests/browser/abort-pthread.cpp', 'src/browser/cpu_runtime.cpp', '--no-entry', '-sPTHREAD_POOL_SIZE=1', '-sENVIRONMENT=node', '-sINITIAL_MEMORY=134217728', '-sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32',
      ...(traced ? ['-DBROWSER_MENU_TRACE_ABORTS=1', 'src/browser/abort-diagnostic.cpp', '-Wl,--wrap=abort', '--profiling-funcs', '--pre-js', 'src/browser/abort-import.js'] : []), '-o', output],
    { encoding: 'utf8', timeout: 120000, env: environment });
    assert.equal(build.status, 0, build.stderr);
    const runner = `${directory}/${name}-run.mjs`;
    writeFileSync(runner, `import create from './${name}.mjs';\nconst module = await create();\nmodule._crash_while_main_joins();\nprocess.exit(79);\n`);
    const run = spawnSync(process.execPath, [runner], { encoding: 'utf8', timeout: 20000, env: environment });
    const log = `${run.stdout ?? ''}${run.stderr ?? ''}`;
    writeFileSync(`${directory}/${name}.log`, log);
    assert.notEqual(run.status, 0, log);
    assert.notEqual(run.status, 79, log);
    assert.match(log, /Aborted\(\)/);
    if (traced) {
      assert.match(log, /_emscripten_exit_with_live_runtime runtime=main message=unwind/);
      assert.match(log, /phase=destroy-joinable .*done=0 uncaught=0 caught=0/);
      const primary = log.split('\n').filter((line) => line.startsWith('[browser-pthread-crash]'));
      assert.ok(primary.length > 0 && primary.length <= 6, log);
      assert.ok(primary.every((line) => line.length <= 511));
      const trace = primary.map((line, n) => line.slice(`[browser-pthread-crash] ${n}:`.length)).join('');
      assert.match(trace, /RuntimeError: unreachable/);
      assert.match(trace, /PublicPthreadCrashRoot/);
      const contexts = log.split('\n').filter(line => line.startsWith('[browser-pthread-context]'));
      assert.ok(contexts.length > 0 && contexts.length <= 6, log);
      assert.ok(contexts.every(line => line.length <= 511));
      assert.match(contexts.join(''), /target=1000 esp=1fe00 ecx=20/);
      const diagnostics = log.split('\n').filter((line) => line.startsWith('[browser-') || line === 'Aborted()');
      assert.ok(diagnostics.slice(-16).some((line) => line.startsWith('[browser-pthread-crash]')), log);
      const isolation = `${directory}/isolation-run.mjs`;
      writeFileSync(isolation, `import assert from 'node:assert/strict';\nimport create from './traced.mjs';\nconst first = await create();\nconst second = await create();\nconst a = first._browser_abort_thread_failure();\nconst b = second._browser_abort_thread_failure();\nassert.notEqual(first.HEAPU8.buffer, second.HEAPU8.buffer);\nassert.equal(Atomics.load(first.HEAP32, a / 4), 0);\nassert.equal(Atomics.load(second.HEAP32, b / 4), 0);\nAtomics.store(first.HEAP32, a / 4, 1);\nassert.equal(Atomics.load(second.HEAP32, b / 4), 0);\nprocess.exit(0);\n`);
      const isolated = spawnSync(process.execPath, [isolation], { encoding: 'utf8', timeout: 20000, env: environment });
      assert.equal(isolated.status, 0, isolated.stderr);
    } else {
      assert.doesNotMatch(log, /\[browser-(pthread-crash|worker-failure|import-throw|abort-stack)\]/);
    }
  }
});
