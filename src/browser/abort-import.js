{
  const originalGetWasmImports = getWasmImports;
  const wrappers = new WeakMap();
  let lines = 0;
  const report = (name, error) => {
    const pthread = typeof ENVIRONMENT_IS_PTHREAD !== 'undefined' && ENVIRONMENT_IS_PTHREAD;
    if ((error === 'unwind' && pthread) || lines >= 8) { return; }
    try {
      const message = String(error?.message ?? error).slice(0, 300);
      const stack = String(error === 'unwind' ? new Error('main runtime unwind sentinel').stack : error?.stack ?? '').slice(0, 2600);
      const detail = `import=${name} runtime=${pthread ? 'pthread' : 'main'} message=${message} stack=${stack}`.replace(/\r?\n/g, '\\n');
      for (let n = 0; n * 450 < detail.length && lines < 8; ++n) {
        const line = `[browser-import-throw] ${lines}: ${detail.slice(n * 450, (n + 1) * 450)}`;
        ++lines;
        err(line);
      }
    } catch {}
  };
  let entryWrapped = false;
  const captureThreadFailure = (entry, argument, error) => {
    if (error === 'unwind') { return; }
    try {
      const address = Module['_browser_abort_thread_failure']?.();
      if (!Number.isInteger(address) || address <= 0 || address % 4 || address + 4096 > HEAPU8.length ||
          !(HEAPU8.buffer instanceof SharedArrayBuffer)) { return; }
      const words = new Int32Array(HEAPU8.buffer, address, 1);
      if (Atomics.compareExchange(words, 0, 0, -1) !== 0) { return; }
      try {
        const stack = String(error?.stack ?? '').replace(/\s+\((?:https?:|file:|wasm:)[^\n]*\)/g, '');
        const frames = stack.split('\n').slice(1).map(frame => frame.trim());
        const counts = new Map();
        for (const frame of frames) { counts.set(frame, (counts.get(frame) ?? 0) + 1); }
        const summary = [...counts].sort((left, right) => right[1] - left[1]).slice(0, 12).map(([frame, count]) => `${count}x ${frame}`).join('; ');
        const boundedStack = stack.length > 2400 ? `${stack.split('\n', 1)[0]} frames=${frames.length} frequent=${summary.slice(0, 900)} head=${stack.slice(0, 700)} [middle omitted] tail=${stack.slice(-700)}` : stack;
        const detail = `entry=${entry} arg=${argument} error=${String(error)} stack=${boundedStack}`.replace(/\r?\n/g, '\\n');
        const bytes = new TextEncoder().encode(detail.slice(0, 2600)).subarray(0, 2600);
        HEAPU8.set(bytes, address + 4);
        Atomics.store(words, 0, bytes.length);
      } catch {
        Atomics.store(words, 0, 0);
      }
    } catch {}
  };
  getWasmImports = () => {
    if (!entryWrapped && typeof ENVIRONMENT_IS_PTHREAD !== 'undefined' && ENVIRONMENT_IS_PTHREAD && typeof invokeEntryPoint === 'function') {
      const originalEntry = invokeEntryPoint;
      invokeEntryPoint = function (entry, argument) {
        let limit;
        let changed = false;
        try {
          limit = Error.stackTraceLimit;
          Error.stackTraceLimit = Math.max(limit || 0, 4096);
          changed = true;
        } catch {}
        try {
          return Reflect.apply(originalEntry, this, [entry, argument]);
        } catch (error) {
          captureThreadFailure(entry, argument, error);
          throw error;
        } finally {
          if (changed) { try { Error.stackTraceLimit = limit; } catch {} }
        }
      };
      entryWrapped = true;
    }
    const imports = originalGetWasmImports();
    for (const [namespace, values] of Object.entries(imports)) {
      for (const [name, value] of Object.entries(values)) {
        if (typeof value !== 'function') { continue; }
        let wrapper = wrappers.get(value);
        if (!wrapper) {
          wrapper = new Proxy(value, {
            apply: (target, receiver, arguments_) => {
              try {
                return Reflect.apply(target, receiver, arguments_);
              } catch (error) {
                report(`${namespace}.${name}/${target.name}`, error);
                throw error;
              }
            },
          });
          wrappers.set(value, wrapper);
          wrappers.set(wrapper, wrapper);
        }
        values[name] = wrapper;
      }
    }
    return imports;
  };
}
