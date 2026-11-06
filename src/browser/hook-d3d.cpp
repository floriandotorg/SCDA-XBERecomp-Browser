#include "browser/hook-compat/xdk/d3d8.h"
#include "browser/cpu_runtime.h"
#include "browser/hook-graphics.h"

namespace browser::hooks {
namespace {
std::atomic<HookGraphics*> bound = nullptr;

HookGraphics& Graphics() {
  auto* graphics = bound.load(std::memory_order_acquire);
  if (!graphics) { throw Fault({}, 0, 0, "game hook reached d3d without a bound graphics device"); }
  return *graphics;
}
}

void BindGraphics(HookGraphics& graphics) { bound.store(&graphics, std::memory_order_release); }
void UnbindGraphics() { bound.store(nullptr, std::memory_order_release); }

}

namespace d3d {

gpu::State& State() { return browser::hooks::Graphics().State(); }
std::recursive_mutex& Lock() { return browser::hooks::Graphics().Lock(); }
void Occlusion(const float* m, bool gtao) { browser::hooks::Graphics().Occlusion(m, gtao); }
void FilterShadows(const float* m) { browser::hooks::Graphics().FilterShadows(m); }
void ApplyShadows() { browser::hooks::Graphics().ApplyShadows(); }
void ViewScale(float xy) { browser::hooks::Graphics().ViewScale(xy); }

}

namespace kernel {

uint32_t SavedDataAddress();
uint32_t SavedDataAddress() { return browser::hooks::Graphics().SavedDisplay(); }

}
