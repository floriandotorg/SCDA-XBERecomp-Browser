#include "browser/hook-graphics.h"
#include <algorithm>
#include <bit>

namespace browser {
namespace {
constexpr uint32_t kFogPlane = 116;
constexpr uint32_t kClipW = 107;
constexpr uint32_t kFogRampWidth = 64;
constexpr uint32_t kBlendSrcAlpha = 0x302;
constexpr uint32_t kBlendInvSrcAlpha = 0x303;

uint32_t Simple(const gpu::State& state, uint32_t method) { return state.simple[method >> 2]; }

float Dot3(const std::array<float, 4>& a, const std::array<float, 4>& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool IsRamp(const std::array<uint32_t, 5>& header) {
  const uint32_t type = header[0] & 0x70000, format = header[3], size = header[4];
  if (type != 0x40000 && type != 0x50000) { return false; }
  const uint32_t width = size ? (size & 0xfff) + 1 : 1u << ((format >> 20) & 0xf);
  const uint32_t height = size ? ((size >> 12) & 0xfff) + 1 : 1u << ((format >> 24) & 0xf);
  return width == kFogRampWidth && height == 1;
}
}

void HookGraphics::SetBackend(gpu::Backend* backend, std::function<bool(GraphicsEffect)> supports) {
  std::lock_guard lock(lock_);
  backend_ = backend;
  supports_ = std::move(supports);
}

bool HookGraphics::FogBlend() const {
  return Simple(state_, gpu::kMBlendEnable) && Simple(state_, gpu::kMSrcBlend) == kBlendSrcAlpha &&
    Simple(state_, gpu::kMDestBlend) == kBlendInvSrcAlpha;
}

void HookGraphics::LatchFog(const std::array<uint32_t, 5>& rampHeader) {
  std::lock_guard lock(lock_);
  if (!FogBlend()) { return; }
  ++fogPasses_;
  if (!IsRamp(rampHeader)) { return; }
  ++fogRamps_;
  const auto& plane = state_.vsc[kFogPlane];
  const auto& depth = state_.vsc[kClipW];
  const float depthLength = Dot3(depth, depth);
  if (depthLength <= 0) { return; }
  const float slope = Dot3(plane, depth) / depthLength;
  const std::array<float, 4> rest = {plane[0] - slope * depth[0], plane[1] - slope * depth[1], plane[2] - slope * depth[2]};
  if (slope <= 0 || Dot3(rest, rest) > 1e-6f * Dot3(plane, plane)) { return; }
  fog_ = {slope, plane[3] - slope * depth[3], (Simple(state_, gpu::kMPsConstant0) >> 24) / 255.0f};
}

void HookGraphics::ClearFog() {
  std::lock_guard lock(lock_);
  fog_ = {};
  fogPasses_ = fogRamps_ = 0;
}

std::array<uint32_t, 2> HookGraphics::FogPasses() const {
  std::lock_guard lock(lock_);
  return {fogPasses_, fogRamps_};
}

FogLatch HookGraphics::Fog() const {
  std::lock_guard lock(lock_);
  return fog_;
}

void HookGraphics::ViewportConstants() {
  std::lock_guard lock(lock_);
  const auto& v = state_.viewport;
  const float halfWidth = v.w * 0.5f, halfHeight = float(v.h) * 0.5f;
  state_.vsc[58] = {halfWidth * viewScale_, -halfHeight * viewScale_, (v.max_z - v.min_z) * state_.zscale, 0};
  state_.vsc[59] = {v.x + state_.ss_offset_x + halfWidth, v.y + state_.ss_offset_y + halfHeight, v.min_z * state_.zscale, 0};
}

void HookGraphics::ViewScale(float xy) {
  std::lock_guard lock(lock_);
  viewScale_ = xy;
  ViewportConstants();
}

bool HookGraphics::Supported(GraphicsEffect effect) {
  if (supports_ && supports_(effect)) {
    drawn_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  unsupported_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

void HookGraphics::Occlusion(const float* m, bool gtao) {
  std::lock_guard lock(lock_);
  if (!backend_ || m[11] != 1.0f || m[15] != 0.0f || !m[0] || !m[5] || !Supported(GraphicsEffect::Occlusion)) { return; }
  const float p[gpu::kOcclusionParams] = {1 / m[0], 1 / m[5], m[8], m[9], m[14], m[10], fog_.slope, fog_.offset, fog_.amount, gtao ? 1.0f : 0.0f};
  backend_->Occlusion(state_, p);
}

void HookGraphics::FilterShadows(const float* m) {
  std::lock_guard lock(lock_);
  if (!backend_ || m[11] != 1.0f || m[15] != 0.0f || !Supported(GraphicsEffect::FilterShadows)) { return; }
  const float p[gpu::kShadowFilterParams] = {m[14], m[10]};
  backend_->FilterShadows(state_, p);
}

void HookGraphics::ApplyShadows() {
  std::lock_guard lock(lock_);
  if (backend_ && Supported(GraphicsEffect::ApplyShadows)) { backend_->ApplyShadows(state_); }
}

std::array<std::atomic<uint32_t>, kHookStatusWords>& HookStatus() {
  static std::array<std::atomic<uint32_t>, kHookStatusWords> words{};
  return words;
}

void PublishHookStatus(const HookGraphics& graphics, FogLatch presented) {
  auto& words = HookStatus();
  words[0].store(std::bit_cast<uint32_t>(presented.slope), std::memory_order_relaxed);
  words[1].store(std::bit_cast<uint32_t>(presented.offset), std::memory_order_relaxed);
  words[2].store(std::bit_cast<uint32_t>(presented.amount), std::memory_order_relaxed);
  words[3].store(uint32_t(std::min<uint64_t>(graphics.UnsupportedEffects(), UINT32_MAX)), std::memory_order_relaxed);
  const auto passes = graphics.FogPasses();
  words[4].store(passes[0], std::memory_order_relaxed);
  words[5].store(passes[1], std::memory_order_relaxed);
  words[6].store(uint32_t(std::min<uint64_t>(graphics.DrawnEffects(), UINT32_MAX)), std::memory_order_relaxed);
}

}

namespace browser {

void HookGraphics::SplashDraw() {
  if (!gpu::g_in_splash) { gpu::g_splash = false; }
}

bool HookGraphics::SplashPresent() {
  savedDisplay_.store(0, std::memory_order_release);
  if (gpu::g_splash_next.exchange(false)) { gpu::g_splash = true; }
  gpu::g_shown_splash = gpu::g_splash.load();
  return gpu::g_shown_splash;
}

}
