#include "browser/file-provider.h"
#include "browser/iso-disc.h"
#include <emscripten.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <pthread.h>

namespace {
std::array<uint32_t, 2> completed;
std::array<uint32_t, 2> unchanged;
std::array<uint32_t, 2> matched;
std::array<uintptr_t, 2> threads;
std::atomic<uint32_t> entered;
std::atomic<uint32_t> owned;
std::atomic<uint32_t> unbound;
std::atomic<uint32_t> rejected;
std::atomic<bool> ioStarted;
std::atomic<uint32_t> stoppedDuringIo;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE void pthread_io_started() { ioStarted.store(true); }

EMSCRIPTEN_KEEPALIVE uint32_t pthread_result(uint32_t field, uint32_t worker) {
  if (worker >= 2) { return 0; }
  switch (field) {
    case 0: return completed[worker];
    case 1: return unchanged[worker];
    case 2: return matched[worker];
    case 3: return owned.load();
    case 4: return unbound.load();
    case 5: return rejected.load();
    case 6: return threads[0] != threads[1];
    case 7: return stoppedDuringIo.load();
    default: return 0;
  }
}

EMSCRIPTEN_KEEPALIVE uint32_t pthread_reads(uint32_t mode) {
  browser::Memory memory;
  memory.Ensure(0x10000, 0x2000);
  browser::Runtime runtime({}, {});
  const auto disc = browser::MountedIsoDisc();
  const auto* file = disc->Find("default.xbe");
  if (!file || file->offset <= UINT32_MAX) { return 0; }
  completed.fill(0);
  unchanged.fill(0);
  matched.fill(0);
  threads.fill(0);
  entered = owned = unbound = rejected = stoppedDuringIo = 0;
  ioStarted = false;
  std::atomic<bool> joining = false;
  const std::array<uint8_t, 7> expected = {10, 20, 30, 40, 50, 60, 70};
  const std::array<uint8_t, 16> sentinel = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
  for (uint32_t n = 0; n < 2; ++n) {
    const uint32_t destination = 0x10000 + n * 0x1000;
    memory.Write({}, destination, sentinel.data(), sentinel.size());
    runtime.Launch({}, memory.Base(), [&, n, destination](X86Context& context, uint8_t*) {
      threads[n] = uintptr_t(pthread_self());
      if (runtime.OwnsCurrentWorker() && runtime.OwnsCurrentContext(context)) { ++owned; }
      if (EM_ASM_INT({ return typeof Module['readIsoBytes'] === 'undefined'; })) { ++unbound; }
      ++entered;
      while (entered.load() != 2 || !joining.load()) { std::this_thread::yield(); }
      if (mode == 1 && n == 1) {
        for (;;) {
          runtime.Checkpoint(context);
          std::this_thread::yield();
        }
      }
      try {
        auto entry = *file;
        if (mode == 4) { entry.offset = uint64_t(6) * 1024 * 1024 * 1024; }
        completed[n] = browser::ReadDiscToGuest(*disc, entry, n, memory, runtime, context,
          mode == 2 ? 0x20000 : destination, mode == 3 ? (1u << 20) + 1 : 7 - n);
      } catch (const browser::Fault&) {
        ++rejected;
      }
      std::array<uint8_t, 16> bytes{};
      memory.Read(context, destination, bytes.data(), bytes.size());
      unchanged[n] = bytes == sentinel;
      matched[n] = completed[n] == 7 - n && bytes[7 - n] == 0xaa &&
        std::equal(bytes.begin(), bytes.begin() + 7 - n, expected.begin() + n);
    });
  }
  std::thread stopper;
  std::exception_ptr stopFailure;
  if (mode == 1) {
    stopper = std::thread([&] {
      while (!ioStarted.load()) { std::this_thread::yield(); }
      stoppedDuringIo = 1;
      stopFailure = runtime.Stop();
    });
  }
  EM_ASM({ Module['isoJoining'] = true; });
  joining.store(true);
  const auto failure = runtime.Join();
  EM_ASM({ Module['isoJoining'] = false; });
  if (stopper.joinable()) { stopper.join(); }
  const auto stopped = runtime.Stop();
  return !failure && !stopped && !stopFailure && runtime.ActiveWorkers() == 0 && entered.load() == 2;
}

}
