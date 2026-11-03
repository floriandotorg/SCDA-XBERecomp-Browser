#include "browser/cpu_runtime.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <pthread.h>
#include <string_view>
#include <system_error>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __EMSCRIPTEN__
extern "C" int __wrap_pthread_join(pthread_t, void**) { return EINVAL; }
#else
void std::thread::join() { throw std::system_error(EINVAL, std::generic_category(), "injected thread::join failure"); }
#endif

int main(int argc, char** argv) {
  try {
    browser::Memory memory;
    browser::Runtime runtime({}, {}, [] { return uint64_t(0); });
    X86Context context{};
    runtime.Launch(context, memory.Base(), [](X86Context&, uint8_t*) {});
    if (argc > 1 && std::string_view(argv[1]) == "launch") {
      while (runtime.ActiveWorkers()) { std::this_thread::yield(); }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      try {
        runtime.Launch(context, memory.Base(), [](X86Context&, uint8_t*) {});
      } catch (const std::exception& exception) {
        std::fprintf(stderr, "[retirement-original] %s\n", exception.what());
#ifdef __EMSCRIPTEN__
        MAIN_THREAD_EM_ASM({ process.exit(73); });
#endif
        std::_Exit(73);
      }
    }
    runtime.Join();
    return 0;
  } catch (...) {
    std::fprintf(stderr, "[retirement-unexpected] primary exception escaped\n");
    return 74;
  }
}
