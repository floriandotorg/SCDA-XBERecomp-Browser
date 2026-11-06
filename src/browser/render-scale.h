#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

// Render scale (internal resolution) as native's RenderScale: screen sized render targets are drawn this many times the
// console's 640 x 480, one factor for both axes; the canvas is the frame's shape at 480 rows (Console) or the page's
// device pixels (Match window, Dynamic).
namespace browser::render_scale {

enum Mode : uint32_t { kConsole = 0, kWindow = 1, kDynamic = 2 };
inline constexpr float kMaximum = 3.0f;  // 1920 x 1440 a target: about ten of them fit the GPU texture budget
inline constexpr float kMinimum = 0.5f;
inline constexpr uint32_t kGuestRows = 480;

struct Output {
  uint32_t width, height;
  float scale;
};

// the canvas and the scale for a mode, the scale percent (of the canvas's rows), the frame's shape and the page's
// device pixels (0 before the page sent them)
inline Output Compute(uint32_t mode, uint32_t percent, float aspect, uint32_t deviceWidth, uint32_t deviceHeight) {
  if (mode == kConsole || !deviceWidth || !deviceHeight) {
    return {uint32_t(std::lround(kGuestRows * aspect)), kGuestRows, 1.0f};
  }
  const uint32_t width = std::clamp(deviceWidth, 1u, 8192u), height = std::clamp(deviceHeight, 1u, 8192u);
  const float scale = float(height) * float(std::clamp(percent, 20u, 200u)) / 100.0f / float(kGuestRows);
  return {width, height, std::clamp(scale, kMinimum, kMaximum)};
}

// a target's host size: the guest's, or scaled when it is the back buffer's size (a screen sized target)
inline std::pair<uint32_t, uint32_t> HostSize(uint32_t width, uint32_t height, uint32_t screenWidth, uint32_t screenHeight, float scale) {
  if (width != screenWidth || height != screenHeight || scale == 1.0f) { return {width, height}; }
  return {std::clamp(uint32_t(float(width) * scale), 1u, 8192u), std::clamp(uint32_t(float(height) * scale), 1u, 8192u)};
}

// Dynamic mode: lowers the scale a step after a second of slow frames, raises it after three seconds of fast ones,
// never above the ceiling (the percent's scale) nor below half of it
class Dynamic {
public:
  float Next(float ceiling, double frameMilliseconds) {
    if (current_ <= 0 || current_ > ceiling) { current_ = ceiling; }
    const double weight = 0.1;
    average_ = average_ ? average_ * (1 - weight) + frameMilliseconds * weight : frameMilliseconds;
    elapsed_ += frameMilliseconds;
    const float step = ceiling * 0.05f;
    if (average_ > 17.5) {
      fast_ = 0;
      if ((slow_ += frameMilliseconds) >= 1000) { current_ = std::max(ceiling * 0.5f, current_ - step); slow_ = 0; }
    } else if (average_ < 14.0) {
      slow_ = 0;
      if ((fast_ += frameMilliseconds) >= 3000) { current_ = std::min(ceiling, current_ + step); fast_ = 0; }
    } else {
      slow_ = fast_ = 0;
    }
    return std::max(kMinimum, current_);
  }
private:
  float current_ = 0;
  double average_ = 0, elapsed_ = 0, slow_ = 0, fast_ = 0;
};

}
