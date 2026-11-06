#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>

// Small native settings for the browser renderer and pacing (C12): the brightness and contrast ramp, the frame cap and
// forced anisotropic filtering, as src/gpu/d3d12.cpp and src/app/main.cpp have them.
namespace browser::settings_pack {

// native UploadGammaRamp's curve (contrast an S curve about mid grey, brightness a gamma after it, black and white
// fixed), applied after the game's own ramp, so Shadow Calibration still works under it; 50 / 50 leaves it alone
inline std::array<uint8_t, 768> ComposeRamp(const uint8_t* guest, uint32_t brightness, uint32_t contrast) {
  std::array<uint8_t, 256> curve;
  const float k = std::exp2((float(contrast) - 50.0f) / 50.0f), g = std::exp2((50.0f - float(brightness)) / 50.0f);
  for (int n = 0; n < 256; ++n) {
    const float x = float(n) / 255.0f;
    const float y = x < 0.5f ? 0.5f * std::pow(2.0f * x, k) : 1.0f - 0.5f * std::pow(2.0f - 2.0f * x, k);
    curve[size_t(n)] = uint8_t(std::lround(std::clamp(std::pow(y, g), 0.0f, 1.0f) * 255.0f));
  }
  std::array<uint8_t, 768> out;
  for (size_t n = 0; n < out.size(); ++n) { out[n] = curve[guest[n]]; }
  return out;
}

// native StageSampler's forced value: only where the title filters linearly across mip levels of a texture that has
// them and the stage is no shadow compare (nor border addressed, which the browser samples in its shader); the title's
// own anisotropic filter (3) uses its MaxAnisotropy. 1 for none.
inline uint32_t Anisotropy(uint32_t setting, uint32_t min, uint32_t mag, uint32_t mip, uint32_t titleMaximum, uint32_t levels,
                           bool shadow, bool border) {
  const auto linear = [](uint32_t filter) { return filter >= 2; };
  uint32_t value = min == 3 || mag == 3 ? std::max(titleMaximum, 1u) : 1u;
  if (setting && linear(min) && mip && levels > 1 && !shadow) { value = setting; }
  return border || shadow ? 1 : std::min(value, 16u);
}

// native app::Pace after each Swap: sleeps to the cap's period, resynchronises when more than a period late; no cap,
// or one at or above the display rate (presentation already waits for it), does not sleep
class Pacer {
public:
  using Clock = std::chrono::steady_clock;
  explicit Pacer(std::function<Clock::time_point()> now = Clock::now,
                 std::function<void(Clock::time_point)> sleepUntil = [](Clock::time_point until) { std::this_thread::sleep_until(until); })
    : now_(std::move(now)), sleepUntil_(std::move(sleepUntil)), next_(now_()) {}
  void Pace(uint32_t cap, uint32_t displayRate) {
    const auto now = now_();
    if (!cap || (displayRate && cap + 3 >= displayRate)) {
      next_ = now;
      return;
    }
    const auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next_ += period;
    if (next_ < now - period) { next_ = now; }
    if (now < next_) { sleepUntil_(next_); }
  }
private:
  std::function<Clock::time_point()> now_;
  std::function<void(Clock::time_point)> sleepUntil_;
  Clock::time_point next_;
};

// the disc's language folders to XC_LANGUAGE ids, as native's kernel answers ExQueryNonVolatileSetting index 7
inline uint32_t LanguageId(std::string_view folder) {
  constexpr std::pair<std::string_view, uint32_t> kIds[] = {{"int", 1}, {"deu", 3}, {"fra", 4}, {"esp", 5}, {"ita", 6}};
  for (const auto& [name, id] : kIds) {
    if (name == folder) { return id; }
  }
  return 1;
}

}
