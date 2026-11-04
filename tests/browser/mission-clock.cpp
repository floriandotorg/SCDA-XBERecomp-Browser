#include "browser/mission-clock.h"
#include <cstdio>
int main() {
  const auto check = [](bool value) { if (!value) { throw std::runtime_error("mission clock changed simulation duration"); } };
  check(browser::MissionClock::ActiveMicroseconds(10500, 20, 28) == 2500);
  check(browser::MissionClock::ActiveMicroseconds(500, 20, 21) == 0);
  check(browser::MissionClock::ActiveMicroseconds(10500, 20, 20) == 10500);
  browser::MissionClock clock;
  check(clock.Sample(100, 0) == 1000000);
  check(clock.Sample(125, 0) == 1250000);
  check(clock.Sample(60125, 60000) == 1250000);
  check(clock.Sample(60150, 60000) == 1500000);
  check(clock.Sample(180150, 180000) == 1500000);
  browser::MissionClock raced;
  check(raced.Sample(100, 0) == 1000000);
  check(!raced.Observe(60100, 0, true, 0));
  check(!raced.Observe(60100, 0, false, 60000));
  check(raced.Observe(60125, 60000, false, 60000) == 1250000);
  check(raced.Observe(60124, 60000, false, 60000) == 1250000);
  check(raced.Observe(60126, 60000, false, 60000) == 1260000);
  bool workerReversal = false;
  try { raced.Observe(60124, 60000, false, 60000); } catch (const std::runtime_error&) { workerReversal = true; }
  check(workerReversal);
  bool rejected = false;
  try { clock.Sample(180149, 180000); } catch (const std::runtime_error&) { rejected = true; }
  check(rejected);
  std::puts("mission-clock-contracts-ok");
}
