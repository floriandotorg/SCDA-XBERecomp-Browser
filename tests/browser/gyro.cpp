#include "browser/input-core.h"
#include "core/gyro.h"
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace ic = browser::input_core;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

ic::GamepadWords gamepad{};
uint32_t g_us = 1000;

// a reading every 10 ms, the gyro in degrees a second about the controller's axes, gravity up its y
void Read(float x, float y, float z, uint32_t count) {
  for (uint32_t n = 0; n < count; ++n) {
    const uint32_t head = gamepad[ic::kMotionHead].load();
    const uint32_t entry = ic::kMotionRing + head % ic::kMotionEntries * ic::kMotionEntryWords;
    const float values[] = {x, y, z, 0, 9.81f, 0};
    gamepad[entry] = g_us;
    for (uint32_t v = 0; v < 6; ++v) { gamepad[entry + 1 + v] = std::bit_cast<uint32_t>(values[v]); }
    gamepad[entry + 7] = 1;
    gamepad[ic::kMotionHead] = head + 1;
    g_us += 10000;
  }
}

bool Near(float value, float expected) { return std::abs(value - expected) < 0.05f; }

int main() {
  ic::BindGamepad(&gamepad);
  gyro::Settings s;
  check(!gyro::Take(s).yaw, "an off gyro turns nothing");
  s.enabled = true;
  s.sensitivity = 2;
  Read(0, 90, 0, 10);
  check(!gyro::Take(s).yaw, "the first take after a stall drops what was gathered");
  Read(0, 90, 0, 11);
  gyro::Turn turn = gyro::Take(s);
  check(Near(turn.yaw, 2 * 90 * 0.11f) && Near(turn.pitch, 0), "a fast turn about the controller's up passes as it is, times the sensitivity");
  Read(30, 0, 0, 10);
  turn = gyro::Take(s);
  check(Near(turn.pitch, -2 * 30 * 0.1f) && Near(turn.yaw, 0), "raising the front pitches up");
  s.axis = gyro::kRoll;
  Read(0, 90, 60, 10);
  check(Near(gyro::Take(s).yaw, 2 * 60 * 0.1f), "roll turns by the axis toward the player");
  s.axis = gyro::kWorld;
  Read(0, 0, 60, 10);
  check(Near(gyro::Take(s).yaw, 0), "world ignores a turn across gravity's up");
  Read(0, 90, 0, 10);
  check(Near(gyro::Take(s).yaw, 2 * 90 * 0.1f), "world turns about gravity's up");
  s.axis = gyro::kYaw;
  Read(0, 90, 0, 40);
  check(Near(gyro::Take(s).yaw, 2 * 90 * 0.15f), "readings past the ring's room are dropped, not misread");
  Read(0, 90, 0, 5);
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  check(!gyro::Take(s).yaw, "a turn gathered while the look did not run is dropped");

  gyro::StartCalibration();
  check(gyro::Calibrating(), "calibration waits for a still window");
  for (uint32_t n = 0; n < 13 && gyro::Calibrating(); ++n) {
    Read(0, 0.5f, 0, 16);
    check(!gyro::Take(s).yaw, "nothing turns while calibrating");
  }
  check(!gyro::Calibrating(), "two still seconds set the drift");
  Read(0, 90.5f, 0, 11);
  check(Near(gyro::Take(s).yaw, 2 * 90 * 0.11f), "the drift is taken off every reading");
  ic::BindGamepad(nullptr);
  check(!gyro::Take(s).yaw, "no session, no gyro");
  std::puts("gyro-contracts-ok");
}
