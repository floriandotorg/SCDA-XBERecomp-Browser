#include <cstddef>
#include <cstdio>
#include <emscripten/emscripten.h>
#include "build_identity.h"
#include "cpu/x86_ops.h"
#include "diagnostic_service.h"

static_assert(sizeof(void*) == 4);
static_assert(sizeof(X86Xmm) == 16);
static_assert(sizeof(X86FuncMapping) == 8);
static_assert(sizeof(X86Context) == 384);
static_assert(alignof(X86Context) == 64);
static_assert(offsetof(X86Context, st) == 80);
static_assert(offsetof(X86Context, xmm) == 208);
static_assert(offsetof(X86Context, host) == 344);
static_assert(sizeof(X86Resume) == 8);

extern "C" EMSCRIPTEN_KEEPALIVE uint32_t diagnostic_mapping_count() {
  uint32_t count = 0;
#ifdef BROWSER_STORY
  while (X86FuncMappings[count].host != nullptr) {
    ++count;
  }
#endif
  return count;
}

extern "C" EMSCRIPTEN_KEEPALIVE uint32_t diagnostic_mapping_guest(uint32_t index) {
#ifdef BROWSER_STORY
  if (index < diagnostic_mapping_count()) {
    return X86FuncMappings[index].guest;
  }
#endif
  return 0;
}

extern "C" EMSCRIPTEN_KEEPALIVE int diagnostic_placeholder() {
  X86Context context{};
  try {
    x86_kernel(context, nullptr, 0x123);
  } catch (const DiagnosticService& service) {
    std::printf("Placeholder: %s address=0x%08X: %s\n", service.name, service.address, service.detail);
    return 1;
  }
  return 0;
}

int main() {
  const uint32_t count = diagnostic_mapping_count();
#ifdef BROWSER_STORY
  if (count != BROWSER_FUNCTION_COUNT) {
    std::fprintf(stderr, "Function inventory mismatch: %u != %u\n", count, BROWSER_FUNCTION_COUNT);
    return 1;
  }
  std::printf("Image: 0x%08X..0x%08X Code: 0x%08X..0x%08X Entry: 0x%08X (not invoked)\n", X86ImageBase, X86ImageEnd, X86CodeBase, X86CodeEnd, X86EntryPoint);
#endif
  std::printf("Build: %s\nToolchain: %s\nRetained functions: %u\nDiagnostic host ready; guest execution disabled\n", BROWSER_BUILD_ID, BROWSER_TOOLCHAIN, count);
  return 0;
}
