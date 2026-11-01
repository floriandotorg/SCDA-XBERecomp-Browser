#include "browser/cpu_runtime.h"
#include <atomic>
#include <cstdio>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

int main() {
  browser::Memory memory;
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime runtime({}, {});
    std::atomic<bool> childEntered = false;
    std::atomic<bool> childCompleted = false;
    X86Context context{};
    runtime.Launch(context, memory.Base(), [&](X86Context& parent, uint8_t* base) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      runtime.Launch(parent, base, [&](X86Context& child, uint8_t*) {
        childEntered.store(true);
        runtime.Wait(child, runtime.Sequence(), std::chrono::milliseconds(50));
        childCompleted.store(true);
        if (n == 1) { throw browser::Fault(child, 255, 0, "descendant guest fault"); }
      });
      while (!childEntered.load()) { std::this_thread::yield(); }
    });
    const auto failure = runtime.Join();
    const bool joined = runtime.ActiveWorkers() == 0 && childCompleted.load();
    runtime.Stop();
    check(joined, "Join returned before a guest descendant completed");
    check(bool(failure) == (n == 1), "Join lost a descendant guest fault");
    if (failure) {
      bool retained = false;
      try { std::rethrow_exception(failure); }
      catch (const browser::Fault& fault) { retained = fault.address == 255; }
      check(retained, "Join changed the descendant fault identity");
    }
  }
  for (uint32_t n = 0; n < 3; ++n) {
    browser::Runtime runtime({}, {});
    std::atomic<bool> publishing = false;
    std::atomic<bool> attempted = false;
    std::atomic<bool> published = false;
    std::atomic<bool> entered = false;
    std::atomic<bool> partial = false;
    std::thread launcher([&] {
      while (!publishing.load()) { std::this_thread::yield(); }
      attempted.store(true);
      X86Context context{};
      context.host = &runtime;
      runtime.Launch(context, memory.Base(), [&](X86Context&, uint8_t*) {
        partial.store(!published.load());
        entered.store(true);
      });
    });
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [&] {
      publishing.store(true);
      while (!attempted.load()) { std::this_thread::yield(); }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      published.store(true);
    });
    launcher.join();
    check(!runtime.Join() && entered.load() && !partial.load(), "guest worker entered a partially published image");
  }
  {
    browser::Runtime runtime({}, {});
    bool rejected = false;
    try {
      runtime.PublishImageMemory(memory, runtime.KernelRevision(), [] { throw std::runtime_error("publication refused"); });
    } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "publication callback failure was hidden");
    runtime.RegisterKernel(255, [](X86Context&, uint8_t*) {});
    bool published = false;
    runtime.PublishImageMemory(memory, runtime.KernelRevision(), [&] { published = true; });
    check(published, "failed publication sealed the runtime");
    rejected = false;
    try { runtime.PublishImageMemory(memory, runtime.KernelRevision(), [&] { published = false; }); }
    catch (const browser::Fault&) { rejected = true; }
    check(rejected && published, "second publication mutated the image");
  }
  std::puts("runtime-ownership-contracts-ok");
}
