#include "browser/cpu_runtime.h"
#include "cpu/x86_ops.h"
#include <cassert>
#include <emscripten.h>

#ifndef PUBLIC_GUEST_DEPTH
#define PUBLIC_GUEST_DEPTH 32
#endif

EM_JS(void, capture_dispatch_stack, (), {
  const limit = Error.stackTraceLimit;
  try {
    Error.stackTraceLimit = 8192;
    const stack = new Error('public dispatch fixture').stack;
    out(JSON.stringify({ invoke: (stack.match(/Runtime::Invoke/g) || []).length,
      indirect: (stack.match(/x86_icall/g) || []).length,
      guest: (stack.match(/PublicGuestCall/g) || []).length }));
  } finally {
    Error.stackTraceLimit = limit;
  }
});

EMSCRIPTEN_KEEPALIVE __attribute__((noinline)) void PublicGuestCall(X86Context& ctx, uint8_t* base) {
  const uint32_t depth = ctx.eax;
  if (depth) {
    --ctx.eax;
    X86_PUSH32(0x12345678);
    x86_icall(ctx, base, 0x1000);
    assert(X86_MEM32(ctx.esp) == 0x12345678);
    ctx.esp += 4;
  } else {
    capture_dispatch_stack();
  }
  ctx.edx += depth;
}

int main() {
  browser::Memory memory;
  memory.Map(0x10000, 0x10000);
  memory.Commit(0x10000, 0x10000);
  const X86FuncMapping mappings[] = {{0x1000, PublicGuestCall}};
  browser::Runtime runtime(mappings, {}, [] { return uint64_t(0); });
  X86Context ctx{};
  ctx.host = &runtime;
  ctx.esp = 0x20000;
  ctx.eax = PUBLIC_GUEST_DEPTH;
  runtime.Invoke(ctx, memory.Base(), 0x1000);
  assert(ctx.edx == PUBLIC_GUEST_DEPTH * (PUBLIC_GUEST_DEPTH + 1) / 2);
  assert(ctx.esp == 0x20000);
}
