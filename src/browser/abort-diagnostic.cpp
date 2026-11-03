#include <atomic>
#include <cstdint>
#include <exception>
#include <emscripten.h>

alignas(4) static uint32_t browser_thread_failure[1024]{};
static std::atomic<uint32_t> browser_context[1 + 16 * 7]{};

extern "C" void browser_abort_record_context(uint32_t worker, uint32_t target, uint32_t esp, uint32_t ecx, uint32_t ebx, uint32_t esi, uint32_t edi) {
  const uint32_t sequence = browser_context[0].fetch_add(1, std::memory_order_relaxed);
  const uint32_t start = 1 + sequence % 16 * 7;
  const uint32_t fields[] = {worker, target, esp, ecx, ebx, esi, edi};
  for (uint32_t n = 0; n < 7; ++n) { browser_context[start + n].store(fields[n], std::memory_order_relaxed); }
}

extern "C" EMSCRIPTEN_KEEPALIVE std::atomic<uint32_t>* browser_abort_context() {
  return browser_context;
}

extern "C" EMSCRIPTEN_KEEPALIVE uint32_t* browser_abort_thread_failure() {
  return browser_thread_failure;
}

EM_JS_DEPS(browser_abort_diagnostic, "emscripten_get_callstack,$UTF8ToString");

EM_JS(void, browser_abort_pthread, (uint32_t* buffer), {
  try {
    const length = Atomics.load(HEAP32, buffer / 4);
    if (length <= 0 || length > 2600) { return; }
    const detail = UTF8ToString(buffer + 4, length);
    for (let n = 0; n * 480 < detail.length; ++n) {
      err("[browser-pthread-crash] " + n + ":" + detail.slice(n * 480, (n + 1) * 480));
    }
  } catch (failure) {
  }
});

EM_JS(void, browser_abort_context_report, (void* buffer), {
  try {
    const words = new Uint32Array(HEAPU8.buffer, buffer, 1 + 16 * 7);
    const sequence = Atomics.load(words, 0);
    const records = [];
    for (let n = Math.max(0, sequence - 16); n < sequence; ++n) {
      const start = 1 + n % 16 * 7;
      const names = ['worker', 'target', 'esp', 'ecx', 'ebx', 'esi', 'edi'];
      records.push(names.map((name, field) => name + '=' + Atomics.load(words, start + field).toString(16)).join(' '));
    }
    const text = records.join('; ');
    for (let n = 0; n < Math.min(1920, text.length); n += 480) {
      try { err('[browser-pthread-context] ' + n / 480 + ':' + text.slice(n, n + 480)); } catch {}
    }
  } catch {}
});

EM_JS(void, browser_abort_stack, (int flags, char* buffer, int capacity), {
  try {
    const limit = Error.stackTraceLimit;
    try {
      Error.stackTraceLimit = Math.max(limit || 0, 64);
      const written = _emscripten_get_callstack(flags, buffer, capacity);
      const stack = UTF8ToString(buffer, capacity - 1) + (written >= capacity ? "\n[truncated]" : "");
      for (let n = 0; n * 480 < stack.length; ++n) {
        err("[browser-abort-stack] " + n + ":" + stack.slice(n * 480, (n + 1) * 480));
      }
    } finally {
      Error.stackTraceLimit = limit;
    }
  } catch (failure) {
  }
});

EM_JS(void, browser_abort_exception, (const char* message, int active), {
  try {
    const text = active === 0 ? "[std::current_exception empty]" : active === 2 ? "[non-std C++ exception]" :
      message ? UTF8ToString(message, 1024) : "[std::exception has null what()]";
    for (let n = 0; n * 480 < text.length; ++n) {
      err("[browser-abort-exception] " + n + ":" + text.slice(n * 480, (n + 1) * 480));
    }
  } catch (failure) {
  }
});

extern "C" {
[[noreturn]] void __real_abort() noexcept;

[[noreturn]] void __wrap_abort() noexcept {
  browser_abort_pthread(browser_thread_failure);
  browser_abort_context_report(browser_context);
  char stack[4096];
  browser_abort_stack(EM_LOG_JS_STACK | EM_LOG_NO_PATHS, stack, sizeof(stack));
  const auto active = std::current_exception();
  if (active) {
    try {
      std::rethrow_exception(active);
    } catch (const std::exception& failure) {
      browser_abort_exception(failure.what(), 1);
    } catch (...) {
      browser_abort_exception(nullptr, 2);
    }
  } else {
    browser_abort_exception(nullptr, 0);
  }
  __real_abort();
}
}
