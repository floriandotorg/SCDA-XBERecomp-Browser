import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createBootController } from '../../src/browser/boot-controller.mjs';

test('verified single player launch stops at the stage boundary and retires the runtime', async () => {
  const { controller, source, runtimes } = fixture();
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const runtime = runtimes[0];
  runtime.context.report({ generation: runtime.context.generation, ok: false, code: 'SINGLE_PLAYER_LAUNCH_REQUEST',
    message: 'Single player launch requested.', entryEntered: true, entryReturned: true, ordinal: 0, threadId: 20 });
  await flush();
  runtime.teardown.resolve();
  await boot;
  assert.equal(controller.state.phase, 'stopped');
  assert.equal(controller.state.code, 'SINGLE_PLAYER_LAUNCH_REQUEST');
  assert.equal(controller.state.source, source);
  assert.equal(runtime.terminated, 1);
});

test('graphics checkpoint is stopped diagnostics, not a running game or host failure', async () => {
  const { controller, source, runtimes } = fixture();
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const runtime = runtimes[0];
  runtime.context.report({ generation: runtime.context.generation, ok: false, code: 'GRAPHICS_CHECKPOINT',
    message: 'Diagnostic graphics request.', entryEntered: true, entryReturned: true, threadId: 20 });
  await flush();
  assert.notEqual(controller.state.phase, 'stopped');
  runtime.teardown.resolve();
  await boot;
  assert.equal(controller.state.phase, 'stopped');
  assert.equal(controller.state.code, 'GRAPHICS_CHECKPOINT');
  assert.equal(runtime.cancelled, 1);
  assert.equal(runtime.terminated, 1);
  assert.equal(controller.state.diagnostic.entryReturned, true);
});

const deferred = () => {
  let resolve;
  let reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};
const flush = async () => { for (let n = 0; n < 12; ++n) { await Promise.resolve(); } };
const fixture = (options = {}) => {
  const runtimes = [];
  const states = [];
  const timers = new Map();
  let timerId = 0;
  let clock = 0;
  const source = new Blob(['local ISO']);
  const controller = createBootController({
    validateSelection: async () => {},
    createRuntime: () => {
      const teardown = deferred();
      const runtime = {
        teardown, cancelled: 0, terminated: 0, execution: 0, ioBytes: 0,
        sampleProgress: () => ({ execution: runtime.execution, ioBytes: runtime.ioBytes }),
        start: (context) => {
          runtime.context = context;
          if (options.bootProgress !== false) { context.report({ generation: context.generation, phase: 'booting' }); }
          return options.startup?.promise;
        },
        cancelIo: () => { ++runtime.cancelled; },
        terminateAll: () => { ++runtime.terminated; return teardown.promise; },
      };
      runtimes.push(runtime);
      return runtime;
    },
    now: () => clock,
    setTimer: (callback, delay) => { const id = ++timerId; timers.set(id, { callback, delay }); return id; },
    clearTimer: (id) => timers.delete(id),
    onChange: (state) => states.push(state),
    ...options,
  });
  const tick = (milliseconds = 250) => {
    clock += milliseconds;
    const entry = [...timers.entries()].find(([, timer]) => timer.delay === 250);
    assert.ok(entry, 'independent progress observer must remain armed');
    timers.delete(entry[0]);
    entry[1].callback();
  };
  return { controller, source, runtimes, states, timers, tick };
};

test('repeated start and stop retains the source and retires ownership before I/O and full teardown', async () => {
  const { controller, source, runtimes } = fixture();
  await controller.select(source);
  for (let n = 0; n < 3; ++n) {
    const boot = controller.start();
    await flush();
    const runtime = runtimes[n];
    assert.equal(runtime.context.source, source);
    assert.equal(Atomics.load(runtime.context.ownership, 0), runtime.context.generation);
    runtime.context.report({ generation: runtime.context.generation, phase: 'running' });
    assert.equal(controller.state.phase, 'running');
    const stop = controller.stop();
    assert.equal(runtime.context.signal.aborted, true);
    assert.notEqual(Atomics.load(runtime.context.ownership, 0), runtime.context.generation);
    assert.equal(runtime.cancelled, 1);
    assert.equal(runtime.terminated, 1);
    assert.equal(controller.state.stopping, true);
    runtime.teardown.resolve();
    await stop;
    assert.equal((await boot).code, 'CANCELLED');
    assert.equal(controller.state.phase, 'stopped');
    assert.equal(controller.state.source, source);
  }
});

test('replacement waits for every worker to terminate and ignores retired and foreign reports', async () => {
  const { controller, source, runtimes } = fixture();
  await controller.select(source);
  const firstBoot = controller.start();
  await flush();
  const first = runtimes[0];
  const secondBoot = controller.start();
  await flush();
  assert.equal(runtimes.length, 1);
  first.context.report({ generation: first.context.generation, phase: 'running' });
  assert.notEqual(controller.state.phase, 'running');
  first.teardown.resolve();
  await firstBoot;
  await flush();
  const second = runtimes[1];
  second.context.report({ generation: first.context.generation, phase: 'running' });
  assert.equal(controller.state.phase, 'booting');
  const stop = controller.stop();
  second.teardown.resolve();
  await stop;
  await secondBoot;
});

test('disc validation remains distinct from entry launch and only trusted progress advances boot', async () => {
  const { controller, source, runtimes } = fixture({ bootProgress: false });
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const runtime = runtimes[0];
  assert.equal(controller.state.phase, 'validating');
  runtime.context.report({ generation: runtime.context.generation, phase: 'running' });
  assert.equal(controller.state.phase, 'validating');
  runtime.context.report({ generation: runtime.context.generation, phase: 'booting' });
  assert.equal(controller.state.phase, 'booting');
  const stop = controller.stop();
  runtime.teardown.resolve();
  await stop;
  await boot;
});

test('Stop cancels uncooperative validation without creating a late runtime', async () => {
  const validation = deferred();
  const { controller, source, runtimes } = fixture({ validateSelection: () => validation.promise });
  await controller.select(source);
  const boot = controller.start();
  await flush();
  assert.equal(controller.state.phase, 'validating');
  await controller.stop();
  assert.equal((await boot).code, 'CANCELLED');
  validation.resolve();
  await flush();
  assert.equal(runtimes.length, 0);
});

test('late validation rejection after Stop cannot overwrite a new generation', async () => {
  const validation = deferred();
  let validations = 0;
  const { controller, source, runtimes } = fixture({ validateSelection: () => ++validations === 1 ? validation.promise : Promise.resolve() });
  await controller.select(source);
  const firstBoot = controller.start();
  await flush();
  await controller.stop();
  await firstBoot;
  const nextBoot = controller.start();
  await flush();
  validation.reject(new Error('Late stale validation failure.'));
  await flush();
  assert.equal(controller.state.phase, 'booting');
  const stop = controller.stop();
  runtimes[0].teardown.resolve();
  await stop;
  await nextBoot;
});

test('rejected runtime initialization after Stop cannot publish a boot error', async () => {
  const startup = deferred();
  const { controller, source, runtimes } = fixture({ startup });
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const runtime = runtimes[0];
  runtime.context.report({ generation: runtime.context.generation, phase: 'running' });
  const stop = controller.stop();
  runtime.teardown.resolve();
  await stop;
  await boot;
  startup.reject(new Error('Late stale runtime initialization failure.'));
  runtime.context.report({ generation: runtime.context.generation, ok: false, code: 'STALE_FAILURE' });
  await flush();
  assert.equal(controller.state.phase, 'stopped');
  assert.equal(controller.state.code, 'CANCELLED');
});

test('a validation error is explicit and leaves the source reusable', async () => {
  const { controller, source, runtimes } = fixture({ validateSelection: async () => { throw Object.assign(new Error('Wrong disc.'), { code: 'IMAGE_HASH' }); } });
  await controller.select(source);
  assert.equal((await controller.start()).code, 'IMAGE_HASH');
  assert.equal(controller.state.phase, 'error');
  assert.equal(controller.state.source, source);
  assert.equal(runtimes.length, 0);
});

test('failed boot tears down before publishing the final outcome', async () => {
  const { controller, source, runtimes } = fixture();
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const runtime = runtimes[0];
  const report = { generation: runtime.context.generation, ok: false, code: 'UNSUPPORTED_EXPORT', message: 'Unsupported guest export.',
    entryEntered: true, entryReturned: true, threadId: 20, ordinal: 328, exportName: 'XeUnloadSection' };
  runtime.context.report(report);
  assert.equal(controller.state.stopping, true);
  assert.equal(controller.state.diagnostic, undefined);
  assert.equal(runtime.cancelled, 1);
  report.threadId = 99;
  runtime.teardown.resolve();
  assert.equal((await boot).code, 'UNSUPPORTED_EXPORT');
  assert.equal(controller.state.phase, 'error');
  assert.deepEqual(controller.state.diagnostic, { entryEntered: true, entryReturned: true, threadId: 20, ordinal: 328, exportName: 'XeUnloadSection' });
  assert.ok(Object.isFrozen(controller.state.diagnostic));
  await controller.select(source);
  assert.equal(controller.state.diagnostic, undefined);
});

test('first entry return and bare success are never interpreted as a running game', async () => {
  for (const report of [{ ok: true }, { ok: false, code: 'ENTRY_RETURNED', entryReturned: true }]) {
    const { controller, source, runtimes } = fixture();
    await controller.select(source);
    const boot = controller.start();
    await flush();
    const runtime = runtimes[0];
    runtime.context.report({ generation: runtime.context.generation, ...report });
    assert.notEqual(controller.state.phase, 'running');
    runtime.teardown.resolve();
    assert.notEqual((await boot).code, 'RUNNING');
    assert.equal(controller.state.phase, 'error');
  }
});

test('independent watchdog invalidates a hung boot and clears after teardown', async () => {
  const { controller, source, runtimes, timers } = fixture();
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const runtime = runtimes[0];
  [...timers.values()].find((timer) => timer.delay === 30000).callback();
  assert.equal(runtime.context.signal.aborted, true);
  assert.equal(runtime.terminated, 1);
  runtime.teardown.resolve();
  assert.equal((await boot).code, 'WATCHDOG');
  assert.equal(timers.size, 0);
});

test('unobservable module initialization keeps the absolute bound without claiming a dead wait', async () => {
  const { controller, source, runtimes, timers, tick } = fixture({ idleWatchdogMs: 1000 });
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const runtime = runtimes[0];
  runtime.sampleProgress = () => undefined;
  tick(5000);
  assert.equal(runtime.terminated, 0);
  runtime.sampleProgress = () => ({ execution: 0, ioBytes: 0 });
  tick();
  assert.equal(runtime.terminated, 0);
  tick(1000);
  assert.equal(runtime.terminated, 1);
  runtime.teardown.resolve();
  assert.equal((await boot).code, 'NO_PROGRESS');
  assert.equal(timers.size, 0);
});

test('actual work and independent ISO completion defer idle expiry but never the absolute deadline', async () => {
  const { controller, source, runtimes, timers, tick } = fixture({ idleWatchdogMs: 1000 });
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const runtime = runtimes[0];
  for (let n = 0; n < 4; ++n) {
    ++runtime.execution;
    tick(750);
    assert.equal(runtime.terminated, 0);
  }
  runtime.ioBytes = 4096;
  tick(750);
  assert.equal(runtime.terminated, 0);
  [...timers.values()].find((timer) => timer.delay === 30000).callback();
  assert.equal(runtime.terminated, 1);
  runtime.teardown.resolve();
  assert.equal((await boot).code, 'WATCHDOG');
  assert.equal(timers.size, 0);
});

test('guest UI reports cannot hide a dead wait and idle expiry tears down the tree', async () => {
  const { controller, source, runtimes, timers, tick } = fixture({ idleWatchdogMs: 1000 });
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const runtime = runtimes[0];
  for (let n = 0; n < 5; ++n) {
    runtime.context.report({ generation: runtime.context.generation, phase: 'booting', execution: n + 1 });
    tick();
  }
  assert.equal(runtime.context.signal.aborted, true);
  assert.equal(Atomics.load(runtime.context.ownership, 0), 0);
  assert.equal(runtime.cancelled, 1);
  assert.equal(runtime.terminated, 1);
  runtime.teardown.resolve();
  assert.equal((await boot).code, 'NO_PROGRESS');
  assert.equal(timers.size, 0);
});

test('retired progress, delayed observer callbacks and counters cannot refresh a replacement', async () => {
  const { controller, source, runtimes, timers, tick } = fixture({ idleWatchdogMs: 1000 });
  await controller.select(source);
  const firstBoot = controller.start();
  await flush();
  const oldObserver = [...timers.values()].find((timer) => timer.delay === 250).callback;
  const restart = controller.start();
  runtimes[0].teardown.resolve();
  await firstBoot;
  await flush();
  ++runtimes[0].execution;
  runtimes[0].ioBytes = 8192;
  oldObserver();
  tick();
  tick(1000);
  assert.equal(runtimes[1].terminated, 1);
  runtimes[1].teardown.resolve();
  assert.equal((await restart).code, 'NO_PROGRESS');
  assert.equal(timers.size, 0);
});

test('running disarms boot watchdog but Stop remains available', async () => {
  const { controller, source, runtimes, timers } = fixture();
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const runtime = runtimes[0];
  runtime.context.report({ generation: runtime.context.generation, phase: 'running' });
  assert.equal(timers.size, 0);
  const stop = controller.stop();
  runtime.teardown.resolve();
  await stop;
  await boot;
});

test('teardown failure poisons replacement rather than assuming worker termination', async () => {
  const { controller, source, runtimes } = fixture();
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const stop = controller.stop();
  runtimes[0].teardown.reject(new Error('Worker tree termination unverified.'));
  await stop;
  assert.equal((await boot).code, 'TEARDOWN');
  assert.equal((await controller.start()).code, 'TEARDOWN');
  assert.equal(runtimes.length, 1);
  assert.equal(controller.state.phase, 'error');
});

test('selection supersedes a queued restart and discards old source authority', async () => {
  const { controller, source, runtimes } = fixture();
  await controller.select(source);
  const boot = controller.start();
  await flush();
  const restart = controller.start();
  const replacement = new Blob(['other ISO']);
  const selection = controller.select(replacement);
  runtimes[0].teardown.resolve();
  await Promise.all([boot, restart, selection]);
  assert.equal(runtimes.length, 1);
  assert.equal(controller.state.phase, 'selected');
  assert.equal(controller.state.source, replacement);
});

test('I/O cancellation failure still terminates workers and blocks unsafe replacement', async () => {
  const { controller, source, runtimes } = fixture();
  await controller.select(source);
  const boot = controller.start();
  await flush();
  runtimes[0].cancelIo = () => { throw new Error('I/O cancellation failed.'); };
  const stop = controller.stop();
  assert.equal(runtimes[0].terminated, 1);
  runtimes[0].teardown.resolve();
  await stop;
  assert.equal((await boot).code, 'TEARDOWN');
  assert.equal((await controller.start()).code, 'TEARDOWN');
});
