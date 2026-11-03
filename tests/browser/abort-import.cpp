#include "browser/cpu_runtime.h"
#include <emscripten.h>

EM_JS(void, browser_throw_primary, (), {
  if (!globalThis.browserPrimaryError) { globalThis.browserPrimaryError = new Error('retirement primary JS error'); }
  throw globalThis.browserPrimaryError;
});

EM_JS(void, browser_unwind_primary, (), { throw 'unwind'; });

extern "C" int __wrap_pthread_join(pthread_t, void**) { browser_throw_primary(); return 0; }

extern "C" {
EMSCRIPTEN_KEEPALIVE void throw_direct() { browser_throw_primary(); }
EMSCRIPTEN_KEEPALIVE void unwind_direct() { browser_unwind_primary(); }
EMSCRIPTEN_KEEPALIVE void throw_during_join() {
  browser::Memory memory;
  browser::Runtime runtime({}, {}, [] { return uint64_t(0); });
  runtime.Launch({}, memory.Base(), [&runtime](X86Context&, uint8_t*) {
    while (!runtime.ActiveWorkers()) { std::this_thread::yield(); }
    for (;;) { std::this_thread::yield(); }
  });
  runtime.Join();
}
}
