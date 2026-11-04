#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace browser {
class MissionStepper {
  uint64_t previous_ = 0;
  uint64_t backlog_ = 0;
  uint32_t scene_ = 0;
  bool initialized_ = false;
  bool active_ = false;
public:
  static constexpr uint32_t MaximumSteps = 8;
  static constexpr uint64_t MaximumBacklog = 20000000;
  struct Plan { uint32_t steps; float seconds; uint64_t backlogTicks; };
  void Reset() { initialized_ = false; active_ = false; backlog_ = 0; }
  Plan Advance(uint64_t now, uint32_t scene, bool active, float originalDelta, float maximumDelta) {
    if (!std::isfinite(originalDelta) || originalDelta < 0 || !std::isfinite(maximumDelta) || maximumDelta <= 0 ||
        maximumDelta > 1 || (active && (!scene || originalDelta > maximumDelta)) || (initialized_ && now < previous_)) {
      throw std::runtime_error("invalid mission simulation clock or timestep");
    }
    const uint64_t maximumTicks = uint64_t(double(maximumDelta) * 10000000);
    if (!maximumTicks) { throw std::runtime_error("mission simulation timestep is below clock precision"); }
    if (!initialized_ || !active || !active_ || scene != scene_) {
      initialized_ = true;
      active_ = active;
      scene_ = scene;
      previous_ = now;
      backlog_ = 0;
      return {1, originalDelta, 0};
    }
    const uint64_t elapsed = now - previous_;
    if (elapsed > MaximumBacklog || backlog_ > MaximumBacklog - elapsed) {
      throw std::runtime_error("mission simulation cannot keep pace within its bounded time backlog");
    }
    backlog_ += elapsed;
    previous_ = now;
    const uint64_t consumed = std::min(backlog_, maximumTicks * MaximumSteps);
    const uint32_t steps = uint32_t(std::max<uint64_t>(1, (consumed + maximumTicks - 1) / maximumTicks));
    backlog_ -= consumed;
    return {steps, float(double(consumed) / steps / 10000000), backlog_};
  }
};
}
