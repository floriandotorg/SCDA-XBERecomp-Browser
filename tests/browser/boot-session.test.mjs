import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createBootSession } from '../../src/browser/boot-session.mjs';
import { createIsoMailbox } from '../../src/browser/iso-mailbox.mjs';

const buildIdentity = 'a1'.repeat(32);

const fixture = () => {
  const ownership = new Int32Array(new SharedArrayBuffer(4));
  Atomics.store(ownership, 0, 1);
  const messages = [];
  const calls = [];
  const manifest = { identity: buildIdentity, ranges: {}, kernelExports: [
    { ordinal: 277, name: 'RtlEnterCriticalSection', kind: 'function', source: 'src/kernel/xboxkrnl.cpp' },
  ] };
  const module = {
    HEAPU8: new Uint8Array(new SharedArrayBuffer(128)),
    _image_progress_address: () => 4,
    _image_sdk_trace_count: () => 0,
    _image_sdk_trace_word: () => 0,
    _boot_build_identity: () => 32,
    _boot_build_identity_size: () => 64,
    _mount_disc: () => { calls.push('mount'); return 1; },
    _image_bind: () => { calls.push('bind'); return 0; },
    _image_reset: () => { calls.push('reset'); },
    _image_declare_import: () => 1,
    _image_coverage_count: () => 1,
    _image_coverage_slot: () => 0x12040,
    _image_coverage_ordinal: () => 277,
    _image_coverage_kind: () => 0,
    _image_coverage_target: () => 0,
    _image_fault_ordinal: () => 277,
    _image_entry_mapped: () => 1,
    _image_bound: () => 0,
    _image_active_workers: () => 0,
    _image_backing: () => 1234,
    _image_disc_reads_words: () => 8,
    _image_disc_reads_word: () => 0,
    _image_audio_info: field => [48000, 6, 48000, 0, 0, 0, 0, 0, 0, 0][field],
    _image_graphics_checkpoint: () => 0,
    _image_graphics_info: () => 0,
    _image_graphics_event_word: () => 0,
    _image_graphics_resource_word: () => 0,
  };
  module.HEAPU8.set(new TextEncoder().encode(buildIdentity), 32);
  const options = {
    manifest, postMessage: (message) => messages.push(message),
    create: async () => { calls.push('create'); return module; },
    prepare: async ({ manifest: expected }) => { assert.equal(expected, manifest); calls.push('prepare'); return { imports: 1 }; },
  };
  const data = {
    port: { close: () => calls.push('close') }, mailbox: createIsoMailbox(), generation: 1, ownership, size: 4096,
    manifest: { identity: 'player-override' },
  };
  return { ownership, messages, calls, module, options, data };
};

const executedFixture = () => {
  const result = fixture();
  Object.assign(result.module, {
    _image_bind: () => 1, _image_execute: () => 1, _image_entered: () => 1, _image_returned: () => 1,
    _image_tls_callbacks: () => 0, _image_thread_id: () => 16, _image_fault_address: () => 0,
    _image_fault_message: () => 0, _image_fault_message_size: () => 0, _image_register: () => 0,
  });
  return result;
};

test('caught guest faults preserve bounded stderr without changing the guest error', async () => {
  const { options, data, module, messages } = executedFixture();
  options.create = async ({ printErr }) => {
    module._image_fault_ordinal = () => 0;
    module._image_execute = () => {
      for (let n = 0; n < 20; ++n) { printErr(`[browser-memory-fault] ${n}:` + 's'.repeat(600)); }
      return 0;
    };
    return module;
  };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'GUEST_FAULT');
  assert.equal(messages[0].wasmDiagnostics.stderr.length, 16);
  assert.match(messages[0].wasmDiagnostics.stderr[0], /^\[browser-memory-fault\] 4:/);
  assert.ok(messages[0].wasmDiagnostics.stderr.every(line => line.length === 512));
  assert.equal(messages[0].wasmDiagnostics.stderrTruncated, true);
});

test('factory abort hooks preserve the real abort reason behind an opaque runtime failure', async () => {
  const { options, data, messages, calls } = fixture();
  options.create = async ({ onAbort, printErr }) => {
    printErr('Assertion failed: a runtime invariant was violated');
    onAbort('Assertion failed: a runtime invariant was violated');
    throw Object.assign(new WebAssembly.RuntimeError('unreachable'), { code: 'OPAQUE_TRAP' });
  };
  await createBootSession(options)({ data });
  assert.deepEqual(messages, [{
    ok: false, generation: 1, code: 'WASM_ABORT', message: 'Assertion failed: a runtime invariant was violated',
    wasmDiagnostics: {
      abortReason: 'Assertion failed: a runtime invariant was violated', caughtMessage: 'unreachable',
      stderr: ['Assertion failed: a runtime invariant was violated'], stderrTruncated: false,
    },
  }]);
  assert.deepEqual(calls, ['close']);
});

test('an observed abort cannot proceed into guest APIs even if the factory does not throw', async () => {
  const { options, data, module, messages, calls } = fixture();
  options.create = async ({ onAbort }) => { onAbort('factory aborted without throwing'); return module; };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'WASM_ABORT');
  assert.equal(messages[0].message, 'factory aborted without throwing');
  assert.deepEqual(calls, ['reset', 'close']);
});

test('abort diagnostics bound reason, caught context, stderr entry sizes and retained entry count', async () => {
  const { options, data, messages } = fixture();
  options.create = async ({ onAbort, printErr }) => {
    for (let n = 0; n < 100; ++n) { printErr(`${n}:` + 's'.repeat(10000)); }
    onAbort('r'.repeat(100000));
    onAbort('a later abort must not replace the first reason');
    throw new WebAssembly.RuntimeError('e'.repeat(100000));
  };
  await createBootSession(options)({ data });
  const result = messages[0];
  assert.equal(result.code, 'WASM_ABORT');
  assert.equal(result.message, 'r'.repeat(2048));
  assert.equal(result.wasmDiagnostics.abortReason, result.message);
  assert.equal(result.wasmDiagnostics.caughtMessage, 'e'.repeat(2048));
  assert.equal(result.wasmDiagnostics.stderr.length, 16);
  assert.ok(result.wasmDiagnostics.stderr.every((line) => line.length <= 512));
  assert.match(result.wasmDiagnostics.stderr[0], /^84:/);
  assert.match(result.wasmDiagnostics.stderr.at(-1), /^99:/);
  assert.equal(result.wasmDiagnostics.stderrTruncated, true);
  assert.ok(JSON.stringify(result).length < 15000);
});

test('primary pthread crash records survive later stderr eviction without reclassifying the abort', async () => {
  const { options, data, messages } = fixture();
  options.create = async ({ onAbort, printErr }) => {
    for (let n = 0; n < 6; ++n) { printErr(`[browser-pthread-crash] ${n}:original stack ${n}`); }
    for (let n = 0; n < 30; ++n) { printErr(`secondary ${n}`); }
    onAbort('');
    throw new WebAssembly.RuntimeError('unreachable');
  };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'WASM_ABORT');
  assert.equal(messages[0].wasmDiagnostics.pthreadFailure.length, 6);
  assert.equal(messages[0].wasmDiagnostics.pthreadFailure[0], '[browser-pthread-crash] 0:original stack 0');
  assert.ok(messages[0].wasmDiagnostics.stderr.every(line => line.startsWith('secondary')));
});

test('factory callbacks accept only primitive strings without inspecting or serializing module objects', async () => {
  const { options, data, messages } = fixture();
  const { proxy, revoke } = Proxy.revocable({}, {});
  revoke();
  options.create = async ({ onAbort, printErr }) => {
    assert.doesNotThrow(() => printErr(proxy, { key: 'private-key', digest: 'private-digest', HEAPU8: [1, 2, 3] }));
    assert.doesNotThrow(() => onAbort(proxy));
    throw new WebAssembly.RuntimeError('unreachable');
  };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'WASM_ABORT');
  assert.equal(messages[0].message, 'The WebAssembly runtime aborted.');
  assert.equal(messages[0].wasmDiagnostics.abortReason, undefined);
  assert.deepEqual(messages[0].wasmDiagnostics.stderr, []);
  assert.doesNotMatch(JSON.stringify(messages), /private-key|private-digest|HEAPU8/);
});

test('stderr alone never reclassifies caught failures as a Wasm abort', async () => {
  for (const code of [undefined, 'READ_FAILED']) {
    const { options, data, messages } = fixture();
    options.create = async ({ printErr }) => {
      printErr('runtime context');
      throw Object.assign(new WebAssembly.RuntimeError('unreachable'), { code });
    };
    await createBootSession(options)({ data });
    assert.equal(messages[0].code, code ?? 'BOOT');
    assert.equal(messages[0].message, 'unreachable');
    assert.deepEqual(messages[0].wasmDiagnostics, {
      caughtMessage: 'unreachable', stderr: ['runtime context'], stderrTruncated: false,
    });
  }
});

test('an abort during guest execution survives reset failure and still closes the port', async () => {
  const { options, data, module, messages, calls } = executedFixture();
  options.create = async ({ onAbort, printErr }) => {
    module._image_execute = () => {
      printErr('runtime stopped in the guest');
      onAbort('observed guest runtime abort');
      throw new WebAssembly.RuntimeError('unreachable');
    };
    return module;
  };
  module._image_reset = () => { calls.push('reset'); throw new WebAssembly.RuntimeError('cleanup trap'); };
  await assert.doesNotReject(createBootSession(options)({ data }));
  assert.equal(messages.length, 1);
  assert.equal(messages[0].code, 'WASM_ABORT');
  assert.equal(messages[0].message, 'observed guest runtime abort');
  assert.equal(messages[0].entryReturned, undefined);
  assert.equal(messages[0].activeWorkers, undefined);
  assert.deepEqual(calls.slice(-2), ['reset', 'close']);
});

test('reset failures cannot obscure a non-abort primary result or prevent port closure', async () => {
  const { options, data, module, messages, calls } = fixture();
  options.prepare = async () => { throw Object.assign(new Error('preparation failed'), { code: 'PREPARE' }); };
  module._image_reset = () => { calls.push('reset'); throw new Error('reset failed'); };
  await assert.doesNotReject(createBootSession(options)({ data }));
  assert.equal(messages[0].code, 'PREPARE');
  assert.equal(messages[0].message, 'preparation failed');
  assert.deepEqual(calls.slice(-2), ['reset', 'close']);
});

test('ownership retirement discards abort and stderr context, including the old primary message', async () => {
  for (const phase of ['initialization', 'execution']) {
    const { options, data, module, ownership, messages, calls } = executedFixture();
    let callbacks;
    options.create = async (config) => {
      callbacks = config;
      const abort = () => {
        config.printErr('old owner stderr');
        config.onAbort('old owner abort');
        Atomics.store(ownership, 0, 2);
        config.printErr('retired owner stderr');
        config.onAbort('retired owner abort');
        throw new WebAssembly.RuntimeError('old owner unreachable');
      };
      if (phase === 'initialization') { abort(); }
      module._image_execute = abort;
      return module;
    };
    await createBootSession(options)({ data });
    assert.deepEqual(messages, [{ ok: false, generation: 1, code: 'CANCELLED', message: 'Boot generation was retired.' }]);
    assert.doesNotThrow(() => callbacks.onAbort('late abort'));
    assert.equal(calls.at(-1), 'close');
  }
});

test('normal guest classification ignores stderr and late callbacks cannot mutate published results', async () => {
  const { options, data, module, messages } = executedFixture();
  let callbacks;
  options.create = async (config) => { callbacks = config; config.printErr('nonfatal warning'); return module; };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'ENTRY_RETURNED');
  assert.equal(messages[0].ok, false);
  assert.equal(messages[0].entryReturned, true);
  assert.equal(messages[0].wasmDiagnostics, undefined);
  const published = JSON.stringify(messages);
  callbacks.printErr('late stderr');
  callbacks.onAbort('late abort');
  assert.equal(JSON.stringify(messages), published);
});

test('captured failure stderr is an owned snapshot that late callbacks cannot change', async () => {
  const { options, data, messages } = fixture();
  let callbacks;
  options.create = async (config) => {
    callbacks = config;
    config.printErr('original stderr');
    throw new Error('failed');
  };
  await createBootSession(options)({ data });
  const published = JSON.stringify(messages);
  callbacks.printErr('late stderr');
  callbacks.onAbort('late abort');
  assert.deepEqual(messages[0].wasmDiagnostics.stderr, ['original stderr']);
  assert.equal(JSON.stringify(messages), published);
});

test('input shared storage is published before genuine entry execution and the GPU mailbox reaches module initialization', async () => {
  const { options, data, module, messages, calls } = executedFixture();
  const mailbox = new SharedArrayBuffer(64);
  data.webgpuMailbox = mailbox;
  options.create = async (config) => { assert.equal(config.webgpuMailbox, mailbox); return module; };
  module._image_input_address = () => 112;
  module._image_input_words = () => 2;
  options.onProgress = (message) => { messages.push(message); calls.push(message.phase); };
  module._image_execute = () => { assert.equal(calls.at(-1), 'input'); return 1; };
  await createBootSession(options)({ data });
  const input = messages.find((message) => message.phase === 'input');
  assert.equal(input.buffer, module.HEAPU8.buffer);
  assert.equal(input.byteOffset, 112);
  assert.equal(input.words, 2);
});

test('malformed input shared storage is rejected before entry', async () => {
  for (const [address, words] of [[113, 2], [124, 2], [112, 3], [0, 2], [4, 2]]) {
    const { options, data, module, messages } = executedFixture();
    module._image_input_address = () => address;
    module._image_input_words = () => words;
    module._image_execute = () => { throw new Error('entry must not run'); };
    await createBootSession(options)({ data });
    assert.equal(messages[0].code, 'INPUT_PROTOCOL');
  }
});

test('a copied launch request is reported only as an unclassified runtime boundary', async () => {
  const { options, data, module, messages } = executedFixture();
  const path = new TextEncoder().encode('default.xbe');
  module._image_execute = () => 0;
  module._image_launch_word = (field, index) => field === 0 ? path.length : field === 1 ? path[index] : field === 2 ? 1 : index & 255;
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'LAUNCH_BOUNDARY_UNCLASSIFIED');
  assert.deepEqual(messages[0].launchRequest, { path: 'default.xbe', hasData: true, data: Uint8Array.from({ length: 3072 }, (_, n) => n & 255) });
  assert.equal(messages[0].ok, false);
});

test('only the latest verified Solo request identifies the mission launch boundary', async () => {
  for (const verified of [true, false]) {
    const { options, data, module, messages } = executedFixture();
    const path = new TextEncoder().encode('default.xbe');
    module._image_execute = () => 0;
    module._image_launch_word = (field, index) => field === 0 ? path.length : field === 1 ? path[index] : field === 2 ? 0 : 0;
    module._image_mission_count = () => 2;
    module._image_mission_total_word = (field) => field === 0 ? 2 : 0;
    module._image_mission_word = (index, field) => [index + 1, 0, 2, 0, index === 0 || verified ? 3 : 2, 1, 2, 1, 1, 80, 12, 9][field];
    await createBootSession(options)({ data });
    assert.equal(messages[0].code, verified ? 'SINGLE_PLAYER_LAUNCH_REQUEST' : 'LAUNCH_BOUNDARY_UNCLASSIFIED');
    assert.equal(messages[0].ok, false);
    assert.equal(messages[0].launchRequest.path, 'default.xbe');
    assert.equal(messages[0].mission.events.at(-1).requestObserved, verified);
  }
});

test('WebGPU snapshots report only completed genuine presents rather than diagnostic zero frames', async () => {
  const { options, data, module, messages } = executedFixture();
  module._image_graphics_rendering = () => 1;
  module._image_renderer_info = (field) => [2, 3, 1228800, 0, 12, 0, 11, 0, 7, 0][field];
  await createBootSession(options)({ data });
  assert.equal(messages[0].graphics.mode, 'webgpu');
  assert.equal(messages[0].graphics.renderedFrames, '7');
  assert.equal(messages[0].graphics.pipelineCount, 2);
  assert.equal(messages[0].graphics.completedCommands, '11');
});

test('malformed WebGPU completion snapshots fail closed', async () => {
  const { options, data, module, messages } = executedFixture();
  module._image_graphics_rendering = () => 1;
  module._image_renderer_info = (field) => [2, 3, 1228800, 0, 12, 0, 13, 0, 7, 0][field];
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'GRAPHICS_PROTOCOL');
});

test('genuine graphics request stops diagnostically without reporting device or game success', async () => {
  const { options, data, module, messages } = executedFixture();
  module._image_execute = () => 0;
  module._image_graphics_checkpoint = () => 1;
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'GRAPHICS_CHECKPOINT');
  assert.equal(messages[0].ok, false);
  assert.equal(messages[0].entryEntered, true);
  assert.equal(messages[0].graphics.initialized, false);
  assert.equal(messages[0].graphics.renderedFrames, 0);
  assert.match(messages[0].message, /no device completion or rendered frame/i);
});

test('silent audio protocol failures cannot publish successful execution metadata', async () => {
  for (const getter of [undefined, () => 0]) {
    const { options, data, module, messages } = executedFixture();
    module._image_audio_info = getter;
    await createBootSession(options)({ data });
    assert.equal(messages[0].code, 'AUDIO_PROTOCOL');
    assert.equal(messages[0].audio, undefined);
    assert.equal(messages[0].entryEntered, undefined);
  }
});

test('retirement during audio snapshot suppresses stale execution results', async () => {
  const { options, data, module, ownership, messages, calls } = executedFixture();
  const getter = module._image_audio_info;
  module._image_audio_info = field => { Atomics.store(ownership, 0, 2); return getter(field); };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'CANCELLED');
  assert.equal(messages[0].audio, undefined);
  assert.deepEqual(calls.slice(-2), ['reset', 'close']);
});

test('completed entry and graphics checkpoint flags cannot contradict each other', async () => {
  const { options, data, module, messages } = executedFixture();
  module._image_graphics_checkpoint = () => 1;
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'GRAPHICS_PROTOCOL');
  assert.equal(messages[0].entryEntered, undefined);
});

test('guest disc delivery counters retain exact 64 bit provenance without counting bootstrap reads', async () => {
  const { options, data, module, messages } = executedFixture();
  const words = [0xffffffff, 0x200000, 2, 0, 123, 4, 1, 0];
  module._image_disc_reads_word = (index) => words[index];
  await createBootSession(options)({ data });
  assert.deepEqual(messages[0].discReads, {
    assetBytes: '9007203549708287', assetOperations: '2', executableBytes: '17179869307', executableOperations: '1',
  });
});

test('missing or malformed guest disc delivery metadata refuses execution claims', async () => {
  for (const invalid of [undefined, -1, 7, 9, 8.5, 8]) {
    const { options, data, module, messages } = executedFixture();
    module._image_disc_reads_words = invalid === undefined ? undefined : () => invalid;
    module._image_disc_reads_word = () => NaN;
    await createBootSession(options)({ data });
    assert.equal(messages[0].code, 'DISC_READ_PROTOCOL');
    assert.equal(messages[0].discReads, undefined);
    assert.equal(messages[0].entryReturned, undefined);
  }
});

test('retirement during guest disc delivery snapshot suppresses stale execution results', async () => {
  const { options, data, module, ownership, messages, calls } = executedFixture();
  module._image_disc_reads_word = () => { Atomics.store(ownership, 0, 2); return 0; };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'CANCELLED');
  assert.equal(messages[0].discReads, undefined);
  assert.equal(messages[0].entryReturned, undefined);
  assert.deepEqual(calls.slice(-2), ['reset', 'close']);
});

test('one worker binds only its build manifest and reports complete unresolved coverage before reset', async () => {
  const { options, data, messages, calls } = fixture();
  const run = createBootSession(options);
  await run({ data });
  assert.equal(messages[0].identity, buildIdentity);
  assert.equal(messages[0].code, 'EXPORT_UNAVAILABLE');
  assert.deepEqual(messages[0].coverage, [{
    ...options.manifest.kernelExports[0], slot: 0x12040, target: 0, resolution: 'missing',
  }]);
  await run({ data });
  assert.equal(messages[1].code, 'SESSION');
  assert.deepEqual(calls, ['create', 'mount', 'prepare', 'bind', 'reset', 'close', 'close']);
});

test('shared Wasm identity and fault text are copied before browser decoding', async (context) => {
  const Decoder = TextDecoder;
  const decode = Decoder.prototype.decode;
  context.mock.method(Decoder.prototype, 'decode', (input, options) => {
    assert.ok(!(input?.buffer instanceof SharedArrayBuffer), 'browser decoder received shared Wasm bytes');
    return Reflect.apply(decode, new Decoder(), [input, options]);
  });
  const { options, data, module, messages } = fixture();
  module.HEAPU8 = new Uint8Array(new SharedArrayBuffer(256));
  module.HEAPU8.set(new TextEncoder().encode(buildIdentity), 32);
  const reason = 'Observed unsupported guest service.';
  module.HEAPU8.set(new TextEncoder().encode(reason), 128);
  module._image_bind = () => 1;
  module._image_execute = () => 0;
  module._image_entered = () => 1;
  module._image_returned = () => 0;
  module._image_tls_callbacks = () => 0;
  module._image_thread_id = () => 16;
  module._image_fault_address = () => 277;
  module._image_fault_message = () => 128;
  module._image_fault_message_size = () => reason.length;
  module._image_register = () => 0;
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'UNSUPPORTED_EXPORT');
  assert.equal(messages[0].message, reason);
});

test('invalid disc structure and failed local reads remain distinct before preparation', async () => {
  for (const code of ['ISO_INVALID', 'READ_FAILED']) {
    const { options, data, module, messages, calls } = fixture();
    module._mount_disc = () => { calls.push('mount'); return 0; };
    if (code === 'READ_FAILED') { module.isoError = code; }
    await createBootSession(options)({ data });
    assert.equal(messages[0].code, code);
    assert.equal(messages[0].coverage, undefined);
    assert.equal(messages[0].entryEntered, undefined);
    assert.deepEqual(calls, ['create', 'mount', 'reset', 'close']);
  }
});

test('mixed build artifacts fail before disc access, preparation or export declaration', async () => {
  const { options, data, module, messages, calls } = fixture();
  module.HEAPU8.set(new TextEncoder().encode('b2'.repeat(32)), 32);
  data.manifest = { ...options.manifest, identity: 'b2'.repeat(32) };
  module._image_declare_import = () => { calls.push('declare'); return 1; };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'BUILD_IDENTITY');
  assert.equal(messages[0].coverage, undefined);
  assert.deepEqual(calls, ['create', 'reset', 'close']);
});

test('missing and malformed compiled identity fail closed before disc access', async () => {
  const mutations = [
    (module) => { delete module._boot_build_identity; },
    (module) => { delete module._boot_build_identity_size; },
    (module) => { module._boot_build_identity_size = () => 0; },
    (module) => { module._boot_build_identity_size = () => 65; },
    (module) => { module._boot_build_identity = () => -1; },
    (module) => { module._boot_build_identity = () => 0; },
    (module) => { module._boot_build_identity = () => 0.5; },
    (module) => { module._boot_build_identity = () => 2 ** 32 + 32; },
    (module) => { module._boot_build_identity = () => 100; },
    (module) => { module.HEAPU8.fill(0, 32, 96); },
    (module, manifest) => { manifest.identity = ''; },
  ];
  for (const mutate of mutations) {
    const { options, data, module, messages, calls } = fixture();
    mutate(module, options.manifest);
    await createBootSession(options)({ data });
    assert.equal(messages[0].code, 'BUILD_IDENTITY');
    assert.deepEqual(calls, ['create', 'reset', 'close']);
  }
});

test('coverage preserves unsigned Wasm function targets without treating partial coverage as binding', async () => {
  const { options, data, module, messages } = fixture();
  module._image_coverage_kind = () => 1;
  module._image_coverage_target = () => (0xfe000000 + 277 * 4) | 0;
  await createBootSession(options)({ data });
  assert.equal(messages[0].coverage[0].target, 0xfe000000 + 277 * 4);
  assert.equal(messages[0].coverage[0].resolution, 'function');
  assert.equal(messages[0].importsBound, false);
  assert.equal(messages[0].code, 'EXPORT_UNAVAILABLE');
});

for (const phase of ['before initialization', 'initialization', 'identity validation', 'preparation', 'binding']) {
  test(`retirement during ${phase} suppresses export results and closes session resources`, async () => {
    const { options, data, ownership, messages, calls, module } = fixture();
    const retire = () => Atomics.store(ownership, 0, 2);
    if (phase === 'before initialization') { retire(); }
    if (phase === 'initialization') { options.create = async () => { retire(); return module; }; }
    if (phase === 'identity validation') { module._boot_build_identity = () => { retire(); return 32; }; }
    if (phase === 'preparation') { options.prepare = async () => { retire(); return {}; }; }
    if (phase === 'binding') { module._image_bind = () => { retire(); return 0; }; }
    await createBootSession(options)({ data });
    assert.equal(messages.length, 1);
    assert.equal(messages[0].code, 'CANCELLED');
    assert.equal(messages[0].coverage, undefined);
    assert.equal(calls.at(-1), 'close');
    assert.equal(calls.includes('reset'), phase !== 'before initialization');
    if (phase !== 'binding') { assert.ok(!calls.includes('bind')); }
  });
}

test('diagnostic binding executes only the owned session and reports an actual unsupported call', async () => {
  const { options, data, module, messages, calls } = fixture();
  options.onProgress = (progress) => {
    if (progress.phase === 'progress') {
      assert.deepEqual(progress, { generation: 1, phase: 'progress', buffer: module.HEAPU8.buffer, address: 4 });
      calls.push('progress');
    } else {
      assert.deepEqual(progress, { generation: 1, phase: 'booting' });
      calls.push('booting');
    }
  };
  module._image_declare_import = (ordinal, kind) => { assert.equal(ordinal, 277); assert.equal(kind, 0); calls.push('declare'); return 1; };
  module._image_bind = () => { calls.push('bind'); return 1; };
  module._image_bound = () => 1;
  module._image_execute = () => { calls.push('execute'); return 0; };
  module._image_entered = () => 1;
  module._image_returned = () => 0;
  module._image_tls_callbacks = () => 0;
  module._image_thread_id = () => 16;
  module._image_fault_address = () => 277;
  module._image_fault_message = () => 0;
  module._image_fault_message_size = () => 0;
  module._image_register = () => 0;
  module._image_coverage_kind = () => 3;
  const trace = [
    [1, 0, 1, 0, 2, 0, 277, 0xd2000000, 0x1111, 0x1000, 0xfe000454, 0],
    [2, 0, 1, 0, 2, 0, 277, 0xd2000000, 0x1111, 0x1000, 0xfe000454, 2],
  ];
  module._image_trace_count = () => trace.length;
  module._image_trace_word = (index, field) => trace[index][field];
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'UNSUPPORTED_EXPORT');
  assert.equal(messages[0].entryEntered, true);
  assert.equal(messages[0].backingBytes, 1234);
  assert.equal(messages[0].coverage[0].resolution, 'function-trap');
  assert.deepEqual(messages[0].kernelCalls, [
    { sequence: '1', callId: '1', workerId: '2', ordinal: 277, fsBase: 0xd2000000, eax: 0x1111, esp: 0x1000, lastCall: 0xfe000454, phase: 'before' },
    { sequence: '2', callId: '1', workerId: '2', ordinal: 277, fsBase: 0xd2000000, eax: 0x1111, esp: 0x1000, lastCall: 0xfe000454, phase: 'fault' },
  ]);
  assert.deepEqual(calls, ['create', 'mount', 'prepare', 'booting', 'declare', 'bind', 'progress', 'execute', 'reset', 'close']);
});

for (const phase of ['entry metadata', 'worker metadata', 'backing metadata', 'kernel trace metadata']) {
  test(`retirement during ${phase} suppresses the final guest report`, async () => {
    const { options, data, module, ownership, messages, calls } = fixture();
    module._image_bind = () => 1;
    module._image_execute = () => 1;
    module._image_entered = () => 1;
    module._image_returned = () => 1;
    module._image_tls_callbacks = () => 0;
    module._image_thread_id = () => 16;
    module._image_fault_address = () => 0;
    module._image_fault_message = () => 0;
    module._image_fault_message_size = () => 0;
    module._image_fault_ordinal = () => 0;
    module._image_register = () => 0;
    module._image_trace_count = () => 1;
    module._image_trace_word = () => 0;
    const getter = { 'entry metadata': '_image_entry_mapped', 'worker metadata': '_image_active_workers', 'backing metadata': '_image_backing', 'kernel trace metadata': '_image_trace_word' }[phase];
    module[getter] = () => { Atomics.store(ownership, 0, 2); return 0; };
    await createBootSession(options)({ data });
    assert.equal(messages.length, 1);
    assert.equal(messages[0].code, 'CANCELLED');
    assert.equal(messages[0].coverage, undefined);
    assert.equal(messages[0].entryReturned, undefined);
    assert.deepEqual(calls.slice(-2), ['reset', 'close']);
  });
}

test('malformed kernel trace metadata cannot publish unbounded or invalid guest diagnostics', async () => {
  for (const count of [-1, 65, 1.5, 1]) {
    const { options, data, module, messages, calls } = fixture();
    Object.assign(module, {
      _image_bind: () => 1, _image_execute: () => 1, _image_entered: () => 1, _image_returned: () => 1,
      _image_tls_callbacks: () => 0, _image_thread_id: () => 16, _image_fault_address: () => 0,
      _image_fault_message: () => 0, _image_fault_message_size: () => 0, _image_register: () => 0,
      _image_trace_count: () => count, _image_trace_word: () => 3,
    });
    await createBootSession(options)({ data });
    assert.equal(messages[0].code, 'KERNEL_TRACE');
    assert.equal(messages[0].kernelCalls, undefined);
    assert.equal(messages[0].entryReturned, undefined);
    assert.deepEqual(calls.slice(-2), ['reset', 'close']);
  }
});

test('ownership retirement while publishing shared progress prevents guest entry and stale results', async () => {
  const { options, data, module, ownership, messages, calls } = fixture();
  module._image_bind = () => 1;
  module._image_execute = () => { calls.push('execute'); return 1; };
  options.onProgress = (message) => {
    if (message.phase === 'progress') { Atomics.store(ownership, 0, 0); }
  };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'CANCELLED');
  assert.equal(calls.includes('execute'), false);
  assert.deepEqual(calls.slice(-2), ['reset', 'close']);
});

test('retirement during execution suppresses guest results', async () => {
  const { options, data, module, ownership, messages } = fixture();
  module._image_bind = () => 1;
  module._image_execute = () => { Atomics.store(ownership, 0, 2); return 0; };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'CANCELLED');
  assert.equal(messages[0].coverage, undefined);
});

test('malformed ownership never initializes a module or escapes structured failure', async () => {
  for (const invalid of [{ ownership: null }, { ownership: new Int32Array(1) }, { generation: 0 }, { generation: 2 ** 31 }]) {
    const { options, data, messages, calls } = fixture();
    await createBootSession(options)({ data: { ...data, ...invalid } });
    assert.equal(messages.length, 1);
    assert.equal(messages[0].code, 'REQUEST');
    assert.deepEqual(calls, ['close']);
  }
});

test('overlapping start cannot replace the initializing session', async () => {
  const { options, data, messages, calls, module } = fixture();
  let initialized;
  options.create = () => new Promise((resolve) => { initialized = resolve; });
  const run = createBootSession(options);
  const pending = run({ data });
  await run({ data: { ...data, port: { close: () => calls.push('second-close') } } });
  assert.equal(messages[0].code, 'SESSION');
  initialized(module);
  await pending;
  assert.equal(messages[1].code, 'EXPORT_UNAVAILABLE');
  assert.equal(calls.filter((call) => call === 'bind').length, 1);
  assert.equal(calls.filter((call) => call === 'reset').length, 1);
});

test('thread session reports bounded provider children after execution', async () => {
  const { options, data, module, messages } = executedFixture();
  const words = [3, 2, 1, 0, 4, 0x10000, 0, 0x36000, 0];
  module._image_thread_session_word = field => words[field];
  await createBootSession(options)({ data });
  assert.deepEqual(messages[0].threadSession, { scope: 'provider-children-excluding-initial-thread', childrenCreated: 3, childrenCompleted: 2,
    handlesClosed: 1, childrenSuspended: 0, objectReferences: 4, requestedStackBytes: '65536', committedBytes: '221184' });
});

test('thread session absence is explicit and malformed snapshots fail closed', async () => {
  const absent = executedFixture();
  await createBootSession(absent.options)({ data: absent.data });
  assert.equal(absent.messages[0].threadSession, null);
  for (const words of [[128, 0, 0, 0, 0, 0, 0, 0, 0], [1, 2, 0, 0, 0, 0, 0, 0, 0], [1, 0, 0, 0, 0, NaN, 0, 0, 0], [1, 0, 0, 0, 130049, 0, 0, 0, 0]]) {
    const { options, data, module, messages } = executedFixture();
    module._image_thread_session_word = field => words[field];
    await createBootSession(options)({ data });
    assert.equal(messages[0].code, 'THREAD_SESSION_PROTOCOL');
    assert.equal(messages[0].threadSession, undefined);
  }
});

test('retirement during thread session snapshot suppresses guest results', async () => {
  const { options, data, module, ownership, messages } = executedFixture();
  let reads = 0;
  module._image_thread_session_word = () => { ++reads; Atomics.store(ownership, 0, 2); return 0; };
  await createBootSession(options)({ data });
  assert.equal(messages[0].code, 'CANCELLED');
  assert.equal(messages[0].threadSession, undefined);
  assert.equal(reads, 1);
});
