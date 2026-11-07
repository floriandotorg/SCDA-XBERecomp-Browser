#pragma once
#include <array>
#include <atomic>
#include <cstdint>

// The browser's src/core/input.h: input-core.cpp ports the platform neutral parts of src/core/input.cpp over the page's
// raw keyboard and mouse (keyboard-input.h) and its gamepad record (gamepad-input.mjs).
namespace browser::input_core {

// gamepad-input.mjs: sequence (odd while written), connected, the menu mask, lx, ly, rx, ry (y up), then the physical
// buttons in kPadNames order (bits 0-15) with the left and right trigger values in bits 16-23 and 24-31. Then the
// guest's rumble, and a ring of the device's motion sensor readings (gyro.cpp) the page fills.
inline constexpr uint32_t kGamepadWords = 144;
inline constexpr uint32_t kRumbleWord = 8;   // guest: the motors after the vibration strength, left | right << 16
inline constexpr uint32_t kMotionHead = 9;   // page: readings written to the ring, counted
inline constexpr uint32_t kMotionFlags = 10; // page: kMotionSensor while it can read one
inline constexpr uint32_t kMotionSensor = 1;
inline constexpr uint32_t kMotionRing = 16;
inline constexpr uint32_t kMotionEntries = 16;
// microseconds (wrapping), the gyro's x, y, z in degrees a second and the accelerometer's x, y, z in metres a second
// squared (f32, gravity up), all about the controller's axes (x right, y up, z toward the player), then 1 when the
// accelerometer read
inline constexpr uint32_t kMotionEntryWords = 8;
static_assert(kMotionRing + kMotionEntries * kMotionEntryWords == kGamepadWords);
using GamepadWords = std::array<std::atomic<uint32_t>, kGamepadWords>;
// the bound session's, null between sessions
GamepadWords* Gamepad();
// the gamepad of the session that runs, null between sessions
void BindGamepad(GamepadWords* words);
// input::Poll(port) as XInputGetState's 18 byte XINPUT_GAMEPAD; false for a port with nothing plugged in
bool Sample(uint32_t port, std::array<uint8_t, 18>& sample);
// XInputSetState's motors for input::Rumble
void Rumble(uint32_t port, uint16_t left, uint16_t right);

}
