#include "browser/cpu_runtime.h"
#include <emscripten.h>
#include <emscripten/threading.h>

EMSCRIPTEN_KEEPALIVE __attribute__((noinline)) void PublicPthreadCrashRoot() {
  __builtin_trap();
}

void PublicPthreadContext(X86Context& ctx, uint8_t* base) {
  if (ctx.ecx == 32) {
    void (*volatile crash)() = PublicPthreadCrashRoot;
    crash();
    return;
  }
  ++ctx.ecx;
  ctx.esp -= 16;
  browser::Runtime::From(ctx).Invoke(ctx, base, 0x1000);
}

extern "C" EMSCRIPTEN_KEEPALIVE void crash_while_main_joins() {
  if (!emscripten_is_main_runtime_thread()) { __builtin_trap(); }
  browser::Memory memory;
  const X86FuncMapping mappings[] = {{0x1000, PublicPthreadContext}};
  browser::Runtime runtime(mappings, {}, [] { return uint64_t(0); });
  X86Context context{};
  context.esp = 0x20000;
  runtime.Launch(context, memory.Base(), [&runtime](X86Context& ctx, uint8_t* base) {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    runtime.Invoke(ctx, base, 0x1000);
  });
  runtime.Join();
}
