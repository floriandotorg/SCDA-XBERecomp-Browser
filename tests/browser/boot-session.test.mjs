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
