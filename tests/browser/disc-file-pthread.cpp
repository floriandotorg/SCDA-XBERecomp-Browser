#include "browser/file-provider.h"
#include <emscripten.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <pthread.h>

namespace {
std::array<uint32_t, 2> completed;
std::array<uint32_t, 2> unchanged;
std::array<uint32_t, 2> matched;
std::array<uint32_t, 2> statuses;
std::array<uintptr_t, 2> threads;
std::atomic<uint32_t> entered;
std::atomic<uint32_t> owned;
std::atomic<uint32_t> unbound;
std::atomic<uint32_t> rejected;
std::atomic<bool> ioStarted;
std::atomic<uint32_t> stoppedDuringIo;
browser::DiscReadSnapshot snapshot;
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
    case 8: return uint32_t(snapshot.assetBytes);
    case 9: return uint32_t(snapshot.assetOperations);
    case 10: return uint32_t(snapshot.executableBytes);
    case 11: return uint32_t(snapshot.executableOperations);
    case 12: return statuses[worker];
    default: return 0;
  }
}

EMSCRIPTEN_KEEPALIVE uint32_t pthread_reads(uint32_t mode) {
  browser::Runtime runtime({}, {});
  browser::PreparedImage image;
  image.memory = std::make_unique<browser::Memory>();
  auto& memory = *image.memory;
  memory.Ensure(0x10000, 0x10000);
  memory.Ensure(0x30000, 0x2000);
  image.image.kernel_thunk = 0x10000;
  const auto disc = browser::MountedIsoDisc();
  const auto* file = disc->Find("default.xbe");
  if (!file || file->offset <= UINT32_MAX) { return 0; }
  std::function<browser::DiscReadSnapshot()> counts;
  auto close = browser::InstallFileProvider(runtime, image, disc, {}, nullptr, &counts);
  browser::BindImageImports(image, runtime, {});
  completed.fill(0);
  unchanged.fill(0);
  matched.fill(0);
  statuses.fill(0xabcdef);
  threads.fill(0);
  snapshot = {};
  entered = owned = unbound = rejected = stoppedDuringIo = 0;
  ioStarted = false;
  std::atomic<bool> joining = false;
  const std::array<uint8_t, 7> expected = {10, 20, 30, 40, 50, 60, 70};
  std::array<uint8_t, 16> sentinel;
  sentinel.fill(0xaa);
  for (uint32_t n = 0; n < 2; ++n) {
    const uint32_t destination = 0x30000 + n * 0x1000;
    const uint32_t arena = 0x11000 + n * 0x4000;
    memory.Write({}, destination, sentinel.data(), sentinel.size());
    X86Context context{};
    context.host = &runtime; context.fs_base = 0x18000 + n * 4096; context.esp = arena + 0x3000;
    runtime.Launch(context, memory.Base(), [&, n, destination, arena](X86Context& context, uint8_t* base) {
      threads[n] = uintptr_t(pthread_self());
      if (runtime.OwnsCurrentWorker() && runtime.OwnsCurrentContext(context)) { ++owned; }
      if (EM_ASM_INT({ return typeof Module['readIsoBytes'] === 'undefined'; })) { ++unbound; }
      const auto put = [&](uint32_t at, uint32_t value) { memory.Write(context, arena + at, &value, 4); };
      const auto get = [&](uint32_t at) { uint32_t value; memory.Read(context, arena + at, &value, 4); return value; };
      const std::string path = n ? "d:\\story.dat" : "D:\\DEFAULT.xbe";
      memory.Write(context, arena + 0x100, path.data(), path.size());
      put(0, uint32_t(path.size()) * 0x10001); put(4, arena + 0x100);
      put(8, 0xfffffffd); put(12, arena); put(16, 0x40);
      if (runtime.Call(context, base, runtime.KernelTarget(190), {arena + 32, 0x80100080, arena + 8, arena + 40, 0, 0x80, n ? 7u : 1u, 1, n ? 0x64u : 0x60u})) {
        throw std::runtime_error("guest disc handle open failed");
      }
      const auto handle = get(32);
      put(48, mode == 4 ? 0xffffffff : n); put(52, mode == 4 ? 0xffffffff : 0);
      put(40, 0xabcdef); put(44, 0xabcdef);
      ++entered;
      while (entered.load() != 2 || !joining.load()) { std::this_thread::yield(); }
      if (mode == 1 && n == 1) {
        for (;;) {
          runtime.Checkpoint(context);
          std::this_thread::yield();
        }
      }
      try {
        statuses[n] = runtime.Call(context, base, runtime.KernelTarget(219), {handle, 0, 0, 0, arena + 40,
          mode == 2 ? 0x20000u : destination, mode == 3 ? (1u << 20) + 1 : mode == 5 ? 20u : 7 - n, arena + 48});
        completed[n] = get(44);
        if (get(40) != statuses[n] || (statuses[n] && completed[n])) { throw std::runtime_error("read status publication mismatch"); }
      } catch (const browser::Fault&) {
        ++rejected;
      }
      std::array<uint8_t, 16> bytes{};
      memory.Read(context, destination, bytes.data(), bytes.size());
      unchanged[n] = bytes == sentinel;
      matched[n] = completed[n] == 7 - n && bytes[7 - n] == 0xaa &&
        std::equal(bytes.begin(), bytes.begin() + 7 - n, expected.begin() + n);
      close(handle);
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
  snapshot = counts();
  return !failure && !stopped && !stopFailure && runtime.ActiveWorkers() == 0 && entered.load() == 2;
}

}
