import { createWasmIsoReader } from './iso-wasm.mjs';
import { prepareDiscImage } from './iso-image.mjs';

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
    const name = [null, 'XNetStartup', 'WSAStartup', 'Direct3D_CreateDevice'][id];
    const behavior = ['Original', 'Unavailable', 'Diagnostic'][words[10]];
    const argumentCount = [0, 1, 2, 6][id];
    const presentationCount = id === 3 ? 13 : 0;
    if (!name || !behavior || (words[10] === 1 && id !== 1) || (words[10] === 2 && id !== 3) || sequence <= previous || !workerId ||
        words[9] !== argumentCount || words[11] !== presentationCount ||
        words.slice(12 + argumentCount, 18).some(Boolean) || words.slice(18 + presentationCount).some(Boolean)) { reject(); }
    previous = sequence;
    const event = { sequence: sequence.toString(), workerId: workerId.toString(), id, name, behavior,
      fsBase: words[5], threadHandle: words[6], esp: words[7], returnAddress: words[8], arguments: words.slice(12, 12 + argumentCount) };
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
  const reject = () => {
    throw Object.assign(new Error('Invalid diagnostic graphics snapshot.'), { code: 'GRAPHICS_PROTOCOL' });
  };
  if (typeof module._image_graphics_info !== 'function' || typeof module._image_graphics_event_word !== 'function' ||
      typeof module._image_graphics_resource_word !== 'function') { reject(); }
  const word = (value) => {
    if (!Number.isInteger(value) || value < -0x80000000 || value > 0xffffffff) { reject(); }
    return value >>> 0;
  };
  const wide = (low, high) => BigInt(low) | (BigInt(high) << 32n);
  const info = Array.from({ length: 6 }, (_, field) => word(module._image_graphics_info(field)));
  const [initialized, low, high, count, resourceCount, commandWords] = info;
  const total = wide(low, high);
  if (initialized > 1 || count > 64 || resourceCount > 6 || commandWords > 16384 || total < BigInt(count)) { reject(); }
  const requests = ['Init', 'Resource', 'Commands', 'Clear', 'Draw', 'Copy', 'Present', 'ReadUnavailable', 'Flush', 'Gamma',
    'Occlusion', 'FilterShadows', 'ApplyShadows', 'Fence', 'WaitUnavailable', 'Unsupported', 'CpuClear', 'CpuSwap'];
  let previous = 0n;
  const events = Array.from({ length: count }, (_, index) => {
    const words = Array.from({ length: 9 }, (_, field) => word(module._image_graphics_event_word(index, field)));
    const sequence = wide(words[0], words[1]);
    const request = requests[words[2]];
    if (!request || sequence <= previous || sequence > total) { reject(); }
    previous = sequence;
    return { sequence: sequence.toString(), request, arguments: words.slice(3) };
  });
  const resources = Array.from({ length: resourceCount }, (_, index) => {
    const [header, data, bytes, format, size, unavailable] = Array.from({ length: 6 }, (_, field) =>
      word(module._image_graphics_resource_word(index, field)));
    if (!header || !data || !bytes || bytes > 0x4000000 || unavailable > 1) { reject(); }
    return { header, data, bytes, format, size, unavailable: unavailable === 1 };
  });
  return { mode: 'diagnostic-cpu', initialized: initialized === 1, renderedFrames: 0, totalEvents: total.toString(),
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

export const createBootSession = ({ create, manifest, postMessage, prepare = prepareDiscImage, onProgress = () => {} }) => {
  let started = false;
  return async ({ data: { port, mailbox, generation, ownership, size } }) => {
    if (started) {
      port?.close();
      postMessage({ ok: false, generation, code: 'SESSION', message: 'A boot worker owns one session only.' });
      return;
    }
    started = true;
    let module;
    let ownershipChecked = false;
    const signal = {
      get aborted() { return ownershipChecked && Atomics.load(ownership, 0) !== generation; },
      throwIfAborted() {
        if (this.aborted) { throw new Error('Boot generation was retired.'); }
      },
    };
    try {
      const read = createWasmIsoReader({ port, mailbox, generation, ownership, getHeap: () => module.HEAPU8 });
      ownershipChecked = true;
      signal.throwIfAborted();
      module = await create({ readIsoBytes: read });
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
      let execution = {};
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
        const completed = module._image_execute() === 1;
        signal.throwIfAborted();
        const start = module._image_fault_message() >>> 0;
        const length = module._image_fault_message_size();
        const checkpoint = module._image_graphics_checkpoint?.() ?? 0;
        if ((checkpoint !== 0 && checkpoint !== 1) || (checkpoint === 1 && completed)) {
          throw Object.assign(new Error('Invalid graphics checkpoint state.'), { code: 'GRAPHICS_PROTOCOL' });
        }
        execution = {
          code: completed ? 'ENTRY_RETURNED' : checkpoint ? 'GRAPHICS_CHECKPOINT' : module._image_fault_ordinal() ? 'UNSUPPORTED_EXPORT' : 'GUEST_FAULT',
          message: completed ? 'The initial guest entry returned.' : checkpoint ?
            'Diagnostic graphics initialization requested; no device completion or rendered frame is claimed.' :
            new TextDecoder().decode(module.HEAPU8.slice(start, start + length)),
          entryEntered: module._image_entered() === 1, entryReturned: module._image_returned() === 1,
          threadId: module._image_thread_id(), tlsCallbacks: module._image_tls_callbacks(), faultAddress: module._image_fault_address() >>> 0,
          registers: Object.fromEntries(['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi', 'fsBase', 'lastCall', 'threadHandle']
            .map((name, n) => [name, module._image_register(n) >>> 0])),
          kernelCalls: kernelCalls(module),
          sdkCalls: sdkCalls(module),
          discReads: discReads(module),
          graphics: readGraphicsDiagnostic(module),
          audio: readSilentAudioDiagnostic(module),
        };
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
      postMessage({ ok: false, generation, code: signal.aborted ? 'CANCELLED' : error.code ?? 'BOOT', message: error.message });
    } finally {
      module?._image_reset();
      port.close();
    }
  };
};
