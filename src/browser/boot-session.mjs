import { createWasmIsoReader } from './iso-wasm.mjs';
import { prepareDiscImage } from './iso-image.mjs';
import { readMenuStateDiagnostic, readMenuFileDiagnostics } from './menu-state-report.mjs';

const requireBuildIdentity = (module, manifest) => {
  const reject = () => {
    throw Object.assign(new Error('Boot module and manifest build identities do not match. Rebuild and deploy the complete boot artifact set.'), { code: 'BUILD_IDENTITY' });
  };
  if (typeof manifest.identity !== 'string' || !/^[a-f0-9]{64}$/.test(manifest.identity) ||
      typeof module._boot_build_identity !== 'function' || typeof module._boot_build_identity_size !== 'function') {
    reject();
  }
  const address = module._boot_build_identity();
  const size = module._boot_build_identity_size();
  if (!Number.isInteger(address) || address <= 0 || address > 0xffffffff || size !== 64 || !(module.HEAPU8 instanceof Uint8Array)) { reject(); }
  const start = address >>> 0;
  if (start + size > module.HEAPU8.length ||
      new TextDecoder().decode(module.HEAPU8.slice(start, start + size)) !== manifest.identity) {
    reject();
  }
};

const threadSession = (module, signal) => {
  if (typeof module._image_thread_session_word !== 'function') { return null; }
  const reject = () => {
    throw Object.assign(new Error('Invalid bounded provider child thread session snapshot.'), { code: 'THREAD_SESSION_PROTOCOL' });
  };
  const words = Array.from({ length: 9 }, (_, field) => {
    signal.throwIfAborted();
    const value = module._image_thread_session_word(field);
    signal.throwIfAborted();
    if (!Number.isInteger(value) || value < -0x80000000 || value > 0xffffffff) { reject(); }
    return value >>> 0;
  });
  const [childrenCreated, childrenCompleted, handlesClosed, childrenSuspended, objectReferences] = words;
  const wide = field => BigInt(words[field]) | (BigInt(words[field + 1]) << 32n);
  const requestedStackBytes = wide(5), committedBytes = wide(7);
  if (childrenCreated > 127 || childrenCompleted > childrenCreated || handlesClosed > childrenCreated ||
      childrenSuspended > childrenCreated || objectReferences > 130048 ||
      requestedStackBytes > 112n * 1024n * 1024n || committedBytes > 112n * 1024n * 1024n) { reject(); }
  return { scope: 'provider-children-excluding-initial-thread', childrenCreated, childrenCompleted, handlesClosed, childrenSuspended, objectReferences,
    requestedStackBytes: requestedStackBytes.toString(), committedBytes: committedBytes.toString() };
};

const kernelCalls = (module) => {
  const count = module._image_trace_count?.() ?? 0;
  const reject = () => {
    throw Object.assign(new Error('Invalid bounded kernel call trace.'), { code: 'KERNEL_TRACE' });
  };
  if (!Number.isInteger(count) || count < 0 || count > 64 || (count && typeof module._image_trace_word !== 'function')) { reject(); }
  return Array.from({ length: count }, (_, index) => {
    const words = Array.from({ length: 12 }, (_, field) => module._image_trace_word(index, field) >>> 0);
    const identifier = (offset) => (BigInt(words[offset]) | (BigInt(words[offset + 1]) << 32n)).toString();
    const phase = ['before', 'after', 'fault'][words[11]];
    if (!phase) { reject(); }
    return { sequence: identifier(0), callId: identifier(2), workerId: identifier(4),
      ordinal: words[6], fsBase: words[7], eax: words[8], esp: words[9], lastCall: words[10], phase };
  });
};

const sdkCalls = (module) => {
  const reject = () => {
    throw Object.assign(new Error('Invalid bounded SDK entry trace.'), { code: 'SDK_TRACE' });
  };
  if (typeof module._image_sdk_trace_count !== 'function' || typeof module._image_sdk_trace_word !== 'function') { reject(); }
  const count = module._image_sdk_trace_count();
  if (!Number.isInteger(count) || count < 0 || count > 64) { reject(); }
  let previous = 0n;
  return Array.from({ length: count }, (_, index) => {
    const words = Array.from({ length: 31 }, (_, field) => {
      const value = module._image_sdk_trace_word(index, field);
      if (!Number.isInteger(value) || value < -0x80000000 || value > 0xffffffff) { reject(); }
      return value >>> 0;
    });
    const identifier = (offset) => BigInt(words[offset]) | (BigInt(words[offset + 1]) << 32n);
    const sequence = identifier(0);
    const workerId = identifier(2);
    const id = words[4];
    const signature = id >= 69 && id <= 72;
    const name = signature ? (id <= 70 ? 'XCalculateSignatureBegin' : 'XCalculateSignatureEnd') :
      [null, 'XNetStartup', 'WSAStartup', 'Direct3D_CreateDevice'][id];
    const behavior = ['Original', 'Unavailable', 'Diagnostic'][words[10]];
    const argumentCount = signature ? (id % 2 ? 1 : 2) : [0, 1, 2, 6][id];
    const presentationCount = id === 3 ? 13 : 0;
    if (!name || !behavior || (words[10] === 1 && id !== 1) || (words[10] === 2 && id !== 3) || sequence <= previous || !workerId ||
        words[9] !== argumentCount || words[11] !== presentationCount ||
        words.slice(12 + argumentCount, 18).some(Boolean) || words.slice(18 + presentationCount).some(Boolean)) { reject(); }
    previous = sequence;
    const event = { sequence: sequence.toString(), workerId: workerId.toString(), id, name, behavior,
      fsBase: words[5], threadHandle: words[6], esp: words[7], returnAddress: words[8], arguments: words.slice(12, 12 + argumentCount) };
    if (signature) { event.phase = id % 2 ? 'entry' : 'return'; }
    if (id === 3) {
      const pp = words.slice(18);
      event.presentation = { width: pp[0], height: pp[1], format: pp[2], count: pp[3], autoDepth: pp[8], depthFmt: pp[9], flags: pp[10], interval: pp[12], raw: pp };
    }
    return event;
  });
};

export const readSilentAudioDiagnostic = (module) => {
  const reject = () => {
    throw Object.assign(new Error('Invalid bounded silent audio snapshot.'), { code: 'AUDIO_PROTOCOL' });
  };
  if (typeof module._image_audio_info !== 'function') { reject(); }
  const words = Array.from({ length: 10 }, (_, field) => {
    const value = module._image_audio_info(field);
    if (!Number.isInteger(value) || value < -0x80000000 || value > 0xffffffff) { reject(); }
    return value >>> 0;
  });
  if (words[0] !== 48000 || words[1] !== 6 || words[2] < 1 || words[2] > 48000 || words[3] > 1) { reject(); }
  const counter = field => BigInt(words[field]) | (BigInt(words[field + 1]) << 32n);
  const submitted = counter(4), consumed = counter(6), queued = counter(8);
  if (consumed > submitted || submitted - consumed !== queued || queued > BigInt(words[2])) { reject(); }
  return { mode: 'silent-timed', sampleRate: words[0], channels: words[1], capacityFrames: words[2], paused: words[3] === 1,
    submittedFrames: submitted.toString(), consumedFrames: consumed.toString(), queuedFrames: queued.toString(), audibleFrames: 0 };
};

export const readGraphicsDiagnostic = (module) => {
  const reject = (field, value) => {
    throw Object.assign(new Error(`Invalid diagnostic graphics snapshot: ${field}=${String(value)}.`), { code: 'GRAPHICS_PROTOCOL' });
  };
  if (typeof module._image_graphics_info !== 'function' || typeof module._image_graphics_event_word !== 'function' ||
      typeof module._image_graphics_resource_word !== 'function') { reject('exports', 'missing'); }
  const word = (value, field) => {
    if (!Number.isInteger(value) || value < -0x80000000 || value > 0xffffffff) { reject(field, value); }
    return value >>> 0;
  };
  const wide = (low, high) => BigInt(low) | (BigInt(high) << 32n);
  const infoFields = ['initialized', 'total.low', 'total.high', 'eventCount', 'resourceCount', 'commandWords'];
  const info = infoFields.map((field, index) => word(module._image_graphics_info(index), field));
  const [initialized, low, high, count, resourceCount, commandWords] = info;
  const total = wide(low, high);
  if (initialized > 1) { reject('initialized', initialized); }
  if (count > 64) { reject('eventCount', count); }
  if (resourceCount > 6) { reject('resourceCount', resourceCount); }
  if (commandWords > 16384) { reject('commandWords', commandWords); }
  if (total < BigInt(count)) { reject('total', total); }
  const requests = ['Init', 'Resource', 'Commands', 'Clear', 'Draw', 'Copy', 'Present', 'ReadUnavailable', 'Flush', 'Gamma',
    'Occlusion', 'FilterShadows', 'ApplyShadows', 'Fence', 'WaitUnavailable', 'Unsupported', 'CpuClear', 'CpuSwap', 'PersistDisplay'];
  let previous = 0n;
  const events = Array.from({ length: count }, (_, index) => {
    const words = Array.from({ length: 9 }, (_, field) => word(module._image_graphics_event_word(index, field), `events[${index}].words[${field}]`));
    const sequence = wide(words[0], words[1]);
    const request = requests[words[2]];
    if (!request) { reject(`events[${index}].request`, words[2]); }
    if (sequence <= previous || sequence > total) { reject(`events[${index}].sequence`, sequence); }
    previous = sequence;
    return { sequence: sequence.toString(), request, arguments: words.slice(3) };
  });
  const resources = Array.from({ length: resourceCount }, (_, index) => {
    const [header, data, bytes, format, size, unavailable] = Array.from({ length: 6 }, (_, field) =>
      word(module._image_graphics_resource_word(index, field), `resources[${index}].words[${field}]`));
    if (!header) { reject(`resources[${index}].header`, header); }
    if (!data) { reject(`resources[${index}].data`, data); }
    if (!bytes || bytes > 0x4000000) { reject(`resources[${index}].bytes`, bytes); }
    if (unavailable > 1) { reject(`resources[${index}].unavailable`, unavailable); }
    return { header, data, bytes, format, size, unavailable: unavailable === 1 };
  });
  const rendering = module._image_graphics_rendering?.() ?? 0;
  if (rendering !== 0 && rendering !== 1) { reject('rendering', rendering); }
  let renderer;
  if (rendering) {
    if (typeof module._image_renderer_info !== 'function') { reject('renderer.export', 'missing'); }
    const words = Array.from({ length: 10 }, (_, field) => word(module._image_renderer_info(field), `renderer.words[${field}]`));
    const submitted = wide(words[4], words[5]);
    const completed = wide(words[6], words[7]);
    const presented = wide(words[8], words[9]);
    const resident = wide(words[2], words[3]);
    if (words[0] > 256) { reject('renderer.pipelineCount', words[0]); }
    if (words[1] > 4096) { reject('renderer.resourceCount', words[1]); }
    if (resident > 0x4000000n) { reject('renderer.residentBytes', resident); }
    if (completed > submitted) { reject('renderer.completedCommands', `${completed}>${submitted}`); }
    if (presented > submitted) { reject('renderer.renderedFrames', `${presented}>${submitted}`); }
    renderer = { pipelineCount: words[0], resourceCount: words[1], residentBytes: resident.toString(),
      submittedCommands: submitted.toString(), completedCommands: completed.toString(), renderedFrames: presented.toString() };
  }
  let persistedDisplay;
  if (typeof module._image_persisted_display_word === 'function') {
    const words = Array.from({ length: 4 }, (_, field) => word(module._image_persisted_display_word(field), `persistedDisplay[${field}]`));
    if (words.some(Boolean)) {
      const pitch = ((words[2] >>> 24) + 1) * 64;
      const height = ((words[2] >>> 12) & 0xfff) + 1;
      if (!rendering || (words[0] & 0x70000) !== 0x50000 || !words[1] || !words[3] || words[3] > 0x800000 || words[3] !== pitch * height) {
        reject('persistedDisplay.bytes', words[3]);
      }
      persistedDisplay = { common: words[0], format: words[1], size: words[2], bytes: words[3] };
    }
  }
  return { mode: rendering ? 'webgpu' : 'diagnostic-cpu', initialized: initialized === 1, renderedFrames: 0, ...renderer,
    ...(persistedDisplay ? { persistedDisplay } : {}),
    ...(rendering ? { state: readMenuStateDiagnostic(module), fileOpens: readMenuFileDiagnostics(module) } : {}), totalEvents: total.toString(),
    droppedEvents: (total - BigInt(count)).toString(), commandWords, resources, events };
};

const discReads = (module) => {
  const reject = () => {
    throw Object.assign(new Error('Invalid guest disc delivery snapshot.'), { code: 'DISC_READ_PROTOCOL' });
  };
  if (typeof module._image_disc_reads_words !== 'function' || typeof module._image_disc_reads_word !== 'function' ||
      module._image_disc_reads_words() !== 8) { reject(); }
  const words = Array.from({ length: 8 }, (_, index) => {
    const value = module._image_disc_reads_word(index);
    if (!Number.isInteger(value) || value < -0x80000000 || value > 0xffffffff) { reject(); }
    return value >>> 0;
  });
  return Object.fromEntries(['assetBytes', 'assetOperations', 'executableBytes', 'executableOperations']
    .map((name, n) => [name, (BigInt(words[n * 2]) | (BigInt(words[n * 2 + 1]) << 32n)).toString()]));
};

const readInputDescriptor = (module) => {
  if (!module._image_input_address && !module._image_input_words) { return undefined; }
  const reject = () => { throw Object.assign(new Error('Invalid shared menu input storage.'), { code: 'INPUT_PROTOCOL' }); };
  if (typeof module._image_input_address !== 'function' || typeof module._image_input_words !== 'function') { reject(); }
  const byteOffset = module._image_input_address();
  const words = module._image_input_words();
  if (byteOffset === 0 && words === 0) { return undefined; }
  const buffer = module.HEAPU8?.buffer;
  const progress = module._image_progress_address?.();
  if (!(buffer instanceof SharedArrayBuffer) || !Number.isInteger(byteOffset) || byteOffset <= 0 || byteOffset % 4 !== 0 ||
      words !== 2 || byteOffset + words * 4 > buffer.byteLength ||
      (Number.isInteger(progress) && byteOffset < progress + 4 && progress < byteOffset + words * 4)) { reject(); }
  return { buffer, byteOffset, words };
};

const readLaunchRequest = (module) => {
  if (typeof module._image_launch_word !== 'function') { return undefined; }
  const word = (field, index = 0) => module._image_launch_word(field, index);
  const size = word(0);
  if (size === 0) { return undefined; }
  const reject = () => { throw Object.assign(new Error('Invalid owned runtime launch request.'), { code: 'LAUNCH_PROTOCOL' }); };
  if (!Number.isInteger(size) || size < 1 || size > 260 || ![0, 1].includes(word(2))) { reject(); }
  const byte = (field, index) => {
    const value = word(field, index);
    if (!Number.isInteger(value) || value < 0 || value > 255) { reject(); }
    return value;
  };
  const pathBytes = Uint8Array.from({ length: size }, (_, n) => byte(1, n));
  if (pathBytes.some((value) => value < 32 || value > 126)) { reject(); }
  const hasData = word(2) === 1;
  return { path: new TextDecoder().decode(pathBytes), hasData,
    data: hasData ? Uint8Array.from({ length: 3072 }, (_, n) => byte(3, n)) : undefined };
};

export const readMissionDiagnostic = (module) => {
  if (!module._image_mission_count && !module._image_mission_total_word && !module._image_mission_word) { return undefined; }
  const reject = () => { throw Object.assign(new Error('Invalid bounded original-dispatch observation.'), { code: 'MISSION_PROTOCOL' }); };
  if (typeof module._image_mission_count !== 'function' || typeof module._image_mission_total_word !== 'function' ||
      typeof module._image_mission_word !== 'function') { reject(); }
  const word = value => {
    if (!Number.isInteger(value) || value < -0x80000000 || value > 0xffffffff) { reject(); }
    return value >>> 0;
  };
  const wide = (low, high) => BigInt(low) | (BigInt(high) << 32n);
  const count = module._image_mission_count();
  const total = wide(word(module._image_mission_total_word(0)), word(module._image_mission_total_word(1)));
  if (!Number.isInteger(count) || count < 0 || count > 8 || BigInt(count) > total) { reject(); }
  let previous = 0n;
  const events = Array.from({ length: count }, (_, n) => {
    const words = Array.from({ length: 12 }, (_, field) => word(module._image_mission_word(n, field)));
    const sequence = wide(words[0], words[1]);
    const workerId = wide(words[2], words[3]);
    const state = ['Unavailable', 'Malformed', 'ContextMismatch', 'Request'][words[4]];
    if (sequence <= previous || sequence > total || !workerId || !state || words[5] > 1 || words[6] > 255 || words[7] > 1 ||
        words[8] > 0x7fffffff || words[9] > 1024 || words[10] > 128 || words[11] > 128 ||
        (words[5] === 1 && (words[7] !== 1 || !words[9])) || (state === 'Request' && words[5] !== 1)) { reject(); }
    previous = sequence;
    return { sequence: sequence.toString(), workerId: workerId.toString(), state, requestObserved: state === 'Request',
      command: { valid: words[5] === 1, menuPage: words[6], mode: words[7], difficulty: words[8] },
      commandLength: words[9], mapLength: words[10], profileLength: words[11] };
  });
  return { kind: 'original-dispatch-observation', totalEvents: total.toString(), droppedEvents: (total - BigInt(count)).toString(), events };
};

export const createBootSession = ({ create, manifest, postMessage, prepare = prepareDiscImage, onProgress = () => {} }) => {
  let started = false;
  return async ({ data: { port, mailbox, generation, ownership, size, webgpuMailbox } }) => {
    if (started) {
      port?.close();
      postMessage({ ok: false, generation, code: 'SESSION', message: 'A boot worker owns one session only.' });
      return;
    }
    started = true;
    let module;
    let execution;
    let ownershipChecked = false;
    let diagnosticsActive = true;
    let wasmAborted = false;
    let abortReason;
    const stderr = [];
    const pthreadFailure = [];
    let stderrTruncated = false;
    const boundedMessage = (value) => typeof value === 'string' ? value.slice(0, 2048) : undefined;
    const signal = {
      get aborted() { return ownershipChecked && Atomics.load(ownership, 0) !== generation; },
      throwIfAborted() {
        if (this.aborted) { throw new Error('Boot generation was retired.'); }
        if (wasmAborted) { throw new Error('The WebAssembly runtime aborted.'); }
      },
    };
    try {
      const read = createWasmIsoReader({ port, mailbox, generation, ownership, getHeap: () => module.HEAPU8 });
      ownershipChecked = true;
      signal.throwIfAborted();
      module = await create({
        readIsoBytes: read, ...(webgpuMailbox ? { webgpuMailbox } : {}),
        onAbort: (reason) => {
          if (!diagnosticsActive || signal.aborted || wasmAborted) { return; }
          wasmAborted = true;
          abortReason = boundedMessage(reason);
        },
        printErr: (text) => {
          if (!diagnosticsActive || signal.aborted || typeof text !== 'string') { return; }
          if (text.startsWith('[browser-pthread-crash] ') && pthreadFailure.length < 6) { pthreadFailure.push(text.slice(0, 512)); }
          if (text.length > 512) { stderrTruncated = true; }
          if (stderr.length === 16) { stderr.shift(); stderrTruncated = true; }
          stderr.push(text.slice(0, 512));
        },
      });
      signal.throwIfAborted();
      requireBuildIdentity(module, manifest);
      signal.throwIfAborted();
      if (module._mount_disc(size) !== 1) {
        const code = module.isoError ?? 'ISO_INVALID';
        const message = code === 'ISO_INVALID' ? 'No supported Xbox filesystem found or disc structures are corrupt.' : `Selected ISO could not be read: ${code}.`;
        throw Object.assign(new Error(message), { code });
      }
      const find = (path) => {
        const encoded = new TextEncoder().encode(path);
        if (encoded.length > 240) { throw new Error('Path budget exceeded.'); }
        module.HEAPU8.set(encoded, module._validation_path());
        const kind = module._validation_find(encoded.length);
        return kind ? { size: module._validation_size(), dir: kind === 2 } : undefined;
      };
      const disc = { find, read: (path, position, length) => {
        if (!find(path)) { throw new Error('Disc file is absent.'); }
        const completed = module._validation_read(position, length);
        const start = module._validation_buffer();
        return module.HEAPU8.slice(start, start + completed);
      } };
      const prepared = await prepare({
        disc, manifest, module, signal,
        layout: { base: manifest.ranges.X86ImageBase, end: manifest.ranges.X86ImageEnd, entry: manifest.ranges.X86EntryPoint },
      });
      signal.throwIfAborted();
      onProgress({ generation, phase: 'booting' });
      signal.throwIfAborted();
      for (const entry of manifest.kernelExports) {
        if (!['function', 'data'].includes(entry.kind) || module._image_declare_import(entry.ordinal, entry.kind === 'data' ? 1 : 0) !== 1) {
          throw new Error(`Diagnostic import declaration failed: ${entry.ordinal}`);
        }
      }
      signal.throwIfAborted();
      const bound = module._image_bind() === 1;
      signal.throwIfAborted();
      const catalog = new Map(manifest.kernelExports.map((entry) => [entry.ordinal, entry]));
      const coverage = Array.from({ length: module._image_coverage_count() }, (_, n) => {
        const ordinal = module._image_coverage_ordinal(n);
        const kind = ['missing', 'function', 'data', 'function-trap', 'data-guard'][module._image_coverage_kind(n)];
        return {
          ...catalog.get(ordinal), ordinal, slot: module._image_coverage_slot(n),
          target: module._image_coverage_target(n) >>> 0, resolution: kind,
        };
      });
      signal.throwIfAborted();
      if (bound) {
        const address = module._image_progress_address?.();
        const buffer = module.HEAPU8.buffer;
        if (!Number.isInteger(address) || address === 0 || address < -0x80000000 || address > 0xffffffff ||
            (address >>> 0) % 4 || !(buffer instanceof SharedArrayBuffer) || (address >>> 0) + 4 > buffer.byteLength) {
          throw Object.assign(new Error('A bounded shared runtime progress word is required.'), { code: 'PROGRESS_PROTOCOL' });
        }
        signal.throwIfAborted();
        onProgress({ generation, phase: 'progress', buffer, address: address >>> 0 });
        signal.throwIfAborted();
        const input = readInputDescriptor(module);
        signal.throwIfAborted();
        if (input) { onProgress({ generation, phase: 'input', ...input }); }
        signal.throwIfAborted();
        const completed = module._image_execute() === 1;
        signal.throwIfAborted();
        const start = module._image_fault_message() >>> 0;
        const length = module._image_fault_message_size();
        const checkpoint = module._image_graphics_checkpoint?.() ?? 0;
        if ((checkpoint !== 0 && checkpoint !== 1) || (checkpoint === 1 && completed)) {
          throw Object.assign(new Error('Invalid graphics checkpoint state.'), { code: 'GRAPHICS_PROTOCOL' });
        }
        const launchRequest = readLaunchRequest(module);
        execution = {
          ...(launchRequest ? { launchRequest } : {}),
          code: launchRequest ? 'LAUNCH_BOUNDARY_UNCLASSIFIED' : completed ? 'ENTRY_RETURNED' : checkpoint ? 'GRAPHICS_CHECKPOINT' : module._image_fault_ordinal() ? 'UNSUPPORTED_EXPORT' : 'GUEST_FAULT',
          message: launchRequest ? 'The guest requested a new runtime. Its launch data is unclassified; no mission or multiplayer runtime was started.'
            : completed ? 'The initial guest entry returned.' : checkpoint ?
            'Diagnostic graphics initialization requested; no device completion or rendered frame is claimed.' :
            new TextDecoder().decode(module.HEAPU8.slice(start, start + length)),
          entryEntered: module._image_entered() === 1, entryReturned: module._image_returned() === 1,
          threadId: module._image_thread_id(), tlsCallbacks: module._image_tls_callbacks(), faultAddress: module._image_fault_address() >>> 0,
          registers: Object.fromEntries(['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi', 'fsBase', 'lastCall', 'threadHandle']
            .map((name, n) => [name, module._image_register(n) >>> 0])),
          kernelCalls: kernelCalls(module),
          sdkCalls: sdkCalls(module),
          discReads: discReads(module),
          ...(stderr.some(line => line.startsWith('[browser-memory-fault] ')) ? { wasmDiagnostics: { stderr: [...stderr], stderrTruncated } } : {}),
        };
        signal.throwIfAborted();
        execution.threadSession = execution.entryEntered ? threadSession(module, signal) : null;
        execution.graphics = readGraphicsDiagnostic(module);
        execution.audio = readSilentAudioDiagnostic(module);
        const mission = readMissionDiagnostic(module);
        if (mission) { execution.mission = mission; }
        const latestMission = mission?.events.at(-1);
        if (launchRequest && latestMission?.requestObserved && latestMission.command.valid && latestMission.command.mode === 1) {
          execution.code = 'SINGLE_PLAYER_LAUNCH_REQUEST';
          execution.message = 'The original single player New Game flow requested a new runtime. Mission execution is not enabled.';
        }
      }
      signal.throwIfAborted();
      const result = {
        ok: false, generation, code: 'EXPORT_UNAVAILABLE',
        message: 'Production kernel export binding is incomplete.', ...execution,
        ordinal: module._image_fault_ordinal(), exportName: catalog.get(module._image_fault_ordinal())?.name ?? null,
        ...prepared, coverage,
        entryMapped: module._image_entry_mapped() === 1, importsBound: module._image_bound() === 1,
        activeWorkers: module._image_active_workers(), backingBytes: module._image_backing(), identity: manifest.identity,
      };
      signal.throwIfAborted();
      postMessage(result);
    } catch (error) {
      const diagnostics = !signal.aborted && !wasmAborted && error.code === 'GRAPHICS_PROTOCOL' && execution ? {
        ...execution, underlyingGuestCode: execution.code, underlyingGuestMessage: execution.message,
      } : {};
      if (diagnostics.underlyingGuestCode) {
        try {
          const fileOpens = readMenuFileDiagnostics(module);
          if (fileOpens !== undefined) { diagnostics.fileOpens = fileOpens; }
        } catch (secondary) {
          diagnostics.fileOpensError = { code: secondary.code ?? 'BOOT', message: secondary.message };
        }
      }
      const cancelled = signal.aborted;
      const caughtMessage = boundedMessage(error.message);
      const wasmDiagnostics = !cancelled && (wasmAborted || stderr.length) ? {
        ...(abortReason === undefined ? {} : { abortReason }), caughtMessage,
        stderr: [...stderr], stderrTruncated,
        ...(pthreadFailure.length ? { pthreadFailure: [...pthreadFailure] } : {}),
      } : undefined;
      postMessage({ ...(cancelled ? {} : diagnostics), ok: false, generation,
        code: cancelled ? 'CANCELLED' : wasmAborted ? 'WASM_ABORT' : error.code ?? 'BOOT',
        message: cancelled ? 'Boot generation was retired.' : wasmAborted ? abortReason || 'The WebAssembly runtime aborted.' : caughtMessage,
        ...(wasmDiagnostics ? { wasmDiagnostics } : {}),
      });
    } finally {
      diagnosticsActive = false;
      try {
        module?._image_reset();
      } catch {}
      try {
        port.close();
      } catch {}
    }
  };
};
