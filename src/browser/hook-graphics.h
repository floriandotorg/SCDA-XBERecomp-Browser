#pragma once
#include "gpu/backend.h"
#include <array>
#include <atomic>
#include <functional>
#include <mutex>

namespace browser {

enum class GraphicsEffect : uint32_t { Occlusion, FilterShadows, ApplyShadows };

// fog amount at view depth z: amount * saturate(slope * z + offset)
struct FogLatch {
  float slope = 0;
  float offset = 0;
  float amount = 0;
};

// What native game hooks reach through d3d:: (xdk/d3d8.h): the live device state and its lock, the frame's distance
// fog, the view scale and the optional effects. Calls under the device lock; the owner holds it around its own use.
class HookGraphics {
public:
  HookGraphics(gpu::State& state, std::recursive_mutex& lock) : state_(state), lock_(lock) {}
  gpu::State& State() { return state_; }
  std::recursive_mutex& Lock() { return lock_; }
  // supports: whether the backend draws an effect; none, or one that says no, leaves the effect counted and undrawn
  void SetBackend(gpu::Backend* backend, std::function<bool(GraphicsEffect)> supports);
  // before a draw: the game fogs a surface in a pass of its own, the fog colour blended by a 64 x 1 ramp on stage 3
  bool FogBlend() const;
  void LatchFog(const std::array<uint32_t, 5>& rampHeader);
  // at Present
  void ClearFog();
  FogLatch Fog() const;
  // fog blended draws since the last ClearFog, and those with the 64 x 1 ramp
  std::array<uint32_t, 2> FogPasses() const;
  // c58 and c59, the viewport scale and offset, with draws magnified about the viewport's centre
  void ViewportConstants();
  void ViewScale(float xy);
  void Occlusion(const float* m, bool gtao);
  void FilterShadows(const float* m);
  void ApplyShadows();
  uint64_t UnsupportedEffects() const { return unsupported_.load(std::memory_order_relaxed); }
  // AvGetSavedDataAddress: the display a story restart kept, shown until the title presents its own (native Swap
  // clears it)
  void SetSavedDisplay(uint32_t surface) { savedDisplay_.store(surface, std::memory_order_release); }
  uint32_t SavedDisplay() const { return savedDisplay_.load(std::memory_order_acquire); }
  // gpu::g_splash at a draw and a Present, as the native HLE keeps it: true when the frame to show is a 4:3 picture
  void SplashDraw();
  bool SplashPresent();
private:
  std::atomic<uint32_t> savedDisplay_ = 0;
  bool Supported(GraphicsEffect effect);
  gpu::State& state_;
  std::recursive_mutex& lock_;
  gpu::Backend* backend_ = nullptr;
  std::function<bool(GraphicsEffect)> supports_;
  FogLatch fog_;
  uint32_t fogPasses_ = 0;
  uint32_t fogRamps_ = 0;
  float viewScale_ = 1;
  std::atomic<uint64_t> unsupported_ = 0;
};

// Words the page reads for Diagnostics: the fog latched by the last presented frame (f32 slope, offset, amount), the
// optional effects a hook asked for that the backend does not draw, then that frame's fog passes and ramp passes
inline constexpr uint32_t kHookStatusWords = 8;
std::array<std::atomic<uint32_t>, kHookStatusWords>& HookStatus();
void PublishHookStatus(const HookGraphics& graphics, FogLatch presented);

namespace hooks {
void BindGraphics(HookGraphics& graphics);
void UnbindGraphics();
}

}
