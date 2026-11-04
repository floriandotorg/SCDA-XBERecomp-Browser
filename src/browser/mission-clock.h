#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace browser {
class MissionClock {
  uint64_t previous_ = 0;
public:
  static uint64_t ActiveMicroseconds(uint64_t elapsed, uint32_t inactiveBefore, uint32_t inactiveAfter) {
    const uint64_t inactive = uint64_t(uint32_t(inactiveAfter - inactiveBefore)) * 1000;
    return elapsed > inactive ? elapsed - inactive : 0;
  }
#ifdef __EMSCRIPTEN__
  static double HostOrigin() { return EM_ASM_DOUBLE({ return performance.timeOrigin; }); }
  static double HostMilliseconds(double origin) { return EM_ASM_DOUBLE({ return performance.timeOrigin + performance.now() - $0; }, origin); }
#endif
  std::optional<uint64_t> Observe(uint64_t milliseconds, uint64_t inactiveBefore, bool pausedAfter, uint64_t inactiveAfter) {
    if (pausedAfter || inactiveBefore != inactiveAfter) { return std::nullopt; }
    if (milliseconds >= inactiveAfter && milliseconds <= UINT64_MAX / 10000) {
      const uint64_t sample = (milliseconds - inactiveAfter) * 10000;
      if (sample < previous_ && previous_ - sample <= 10000) { return previous_; }
    }
    return Sample(milliseconds, inactiveAfter);
  }
  uint64_t Sample(uint64_t milliseconds, uint64_t inactiveMilliseconds) {
    if (inactiveMilliseconds > milliseconds || milliseconds > UINT64_MAX / 10000) { throw std::runtime_error("mission clock duration is out of range"); }
    const uint64_t sample = (milliseconds - inactiveMilliseconds) * 10000;
    if (sample < previous_) { throw std::runtime_error("mission clock moved backwards"); }
    previous_ = sample;
    return sample;
  }
};
}
