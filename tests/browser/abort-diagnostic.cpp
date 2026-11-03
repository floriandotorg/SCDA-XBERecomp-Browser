#include <cstdlib>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <emscripten.h>
#ifdef BROWSER_ABORT_THREAD_TEST
#include <thread>
#endif

static uint32_t sentinel = 0x51AB07CD;

__attribute__((noinline)) void AbortRoot() {
  std::abort();
}

__attribute__((noinline)) void ThrowingAbortRoot(int kind) {
  if (kind == 2) {
    throw 42;
  }
  if (kind == 1) {
    static char message[2048];
    for (int n = 0; n < 2047; ++n) {
      message[n] = 'x';
    }
    throw std::runtime_error(message);
  }
  throw std::runtime_error("public active exception payload");
}

__attribute__((noinline)) void NoexceptAbortRoot(int kind) noexcept {
  ThrowingAbortRoot(kind);
}

__attribute__((noinline)) void CaughtTerminateRoot(int kind) {
  try {
    ThrowingAbortRoot(kind);
  } catch (...) {
    std::terminate();
  }
}

static void (*volatile failure)() = AbortRoot;
static void (*volatile caughtFailure)(int) = CaughtTerminateRoot;
static void (*volatile noexceptFailure)(int) = NoexceptAbortRoot;

__attribute__((noinline)) void LongAbortStack(int depth) {
  if (depth == 0) {
    failure();
    return;
  }
  LongAbortStack(depth - 1);
  ++sentinel;
}

#ifdef BROWSER_ABORT_THREAD_TEST
__attribute__((noinline)) void AbortThreadRoot() {
  AbortRoot();
}
#endif

extern "C" {
#ifdef BROWSER_ABORT_THREAD_TEST
EMSCRIPTEN_KEEPALIVE void abort_thread_fixture() {
  std::thread(AbortThreadRoot).detach();
}
#endif

EMSCRIPTEN_KEEPALIVE void abort_fixture(int depth) {
  if (depth == 0) {
    AbortRoot();
  }
  LongAbortStack(depth);
}

EMSCRIPTEN_KEEPALIVE void abort_exception_fixture(int kind, int caught) {
  if (caught) {
    caughtFailure(kind);
  }
  noexceptFailure(kind);
}

EMSCRIPTEN_KEEPALIVE uint32_t* abort_sentinel() {
  return &sentinel;
}
}
