#include "browser/hook-compat/xdk/d3d8.h"
#include "browser/cpu_runtime.h"
#include "browser/hook-graphics.h"
#include <algorithm>
#include <array>

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
// the backend talks to the GPU worker and may wait, which a guest worker does outside its execution lease, as every
// guest D3D call reaches it
void Occlusion(const float* m, bool gtao) {
  std::array<float, 16> matrix;
  std::copy_n(m, 16, matrix.begin());
  browser::Memory::PauseExecution pause;
  browser::hooks::Graphics().Occlusion(matrix.data(), gtao);
}
void FilterShadows(const float* m) {
  std::array<float, 16> matrix;
  std::copy_n(m, 16, matrix.begin());
  browser::Memory::PauseExecution pause;
  browser::hooks::Graphics().FilterShadows(matrix.data());
}
void ApplyShadows() {
  browser::Memory::PauseExecution pause;
  browser::hooks::Graphics().ApplyShadows();
}
void ViewScale(float xy) { browser::hooks::Graphics().ViewScale(xy); }

}

namespace kernel {

uint32_t SavedDataAddress();
uint32_t SavedDataAddress() { return browser::hooks::Graphics().SavedDisplay(); }

}
