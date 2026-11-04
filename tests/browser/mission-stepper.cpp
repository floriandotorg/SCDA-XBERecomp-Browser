#include "browser/mission-stepper.h"
#include <cmath>
#include <cstdio>
#include <limits>

int main() {
  const auto check = [](bool valid) { if (!valid) { throw std::runtime_error("mission step timing changed"); } };
  const auto near = [&](double left, double right) { check(std::abs(left - right) < 0.000002); };
  const auto rejects = [&](auto operation) {
    bool failed = false;
    try { operation(); } catch (const std::runtime_error&) { failed = true; }
    check(failed);
  };
  browser::MissionStepper stepper;
  constexpr float maximum = 0.06666599959135056f;
  uint64_t clock = 1000000;
  auto plan = stepper.Advance(clock, 1, true, 0.02f, maximum);
  check(plan.steps == 1);
  near(plan.seconds, 0.02);
  clock += 1500000;
  plan = stepper.Advance(clock, 1, true, maximum, maximum);
  check(plan.steps == 3 && plan.seconds <= maximum);
  near(plan.steps * double(plan.seconds), 0.15);
  plan = stepper.Advance(clock, 1, true, maximum, maximum);
  check(plan.steps == 1 && plan.seconds == 0);
  clock += 9000000;
  const auto first = stepper.Advance(clock, 1, true, maximum, maximum);
  check(first.steps == browser::MissionStepper::MaximumSteps && first.seconds <= maximum && first.backlogTicks);
  clock += 1000000;
  const auto second = stepper.Advance(clock, 1, true, maximum, maximum);
  check(second.steps <= browser::MissionStepper::MaximumSteps && second.seconds <= maximum);
  near(first.steps * double(first.seconds) + second.steps * double(second.seconds) + double(second.backlogTicks) / 10000000, 1.0);
  clock += 50000000;
  plan = stepper.Advance(clock, 1, false, 0.016f, maximum);
  check(plan.steps == 1 && !plan.backlogTicks);
  near(plan.seconds, 0.016);
  clock += 160000;
  plan = stepper.Advance(clock, 1, true, 0.016f, maximum);
  check(plan.steps == 1 && !plan.backlogTicks);
  near(plan.seconds, 0.016);
  clock += 10000000;
  plan = stepper.Advance(clock, 2, true, 0.03f, maximum);
  near(plan.seconds, 0.03);
  check(plan.steps == 1 && !plan.backlogTicks);
  rejects([&] { stepper.Advance(clock - 1, 2, true, 0.03f, maximum); });
  rejects([&] { stepper.Advance(clock, 2, true, -1, maximum); });
  rejects([&] { stepper.Advance(clock, 2, true, std::numeric_limits<float>::quiet_NaN(), maximum); });
  rejects([&] { stepper.Advance(clock, 2, true, 0.03f, 0); });
  rejects([&] { stepper.Advance(clock, 0, true, 0.03f, maximum); });
  rejects([&] { stepper.Advance(clock + 30000000, 2, true, 0.03f, maximum); });
  browser::MissionStepper accounting;
  uint64_t elapsed = 0;
  double consumed = 0;
  accounting.Advance(1000000, 1, true, 0, maximum);
  for (uint32_t n = 0; n < 1000; ++n) {
    elapsed += uint64_t((n * 137) % 250 + 1) * 10000;
    const auto sample = accounting.Advance(1000000 + elapsed, 1, true, maximum, maximum);
    check(sample.steps <= browser::MissionStepper::MaximumSteps && sample.seconds <= maximum);
    consumed += sample.steps * double(sample.seconds);
    check(std::abs(consumed + double(sample.backlogTicks) / 10000000 - double(elapsed) / 10000000) < 0.0002);
  }
  std::puts("mission-stepper-contracts-ok");
}
