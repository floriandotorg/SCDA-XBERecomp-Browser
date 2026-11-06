#pragma once
#include <array>
#include <atomic>
#include <cstdint>

// The browser's src/core/input.h: input-core.cpp ports the platform neutral parts of src/core/input.cpp over the page's
// raw keyboard and mouse (keyboard-input.h) and its gamepad record (gamepad-input.mjs).
namespace browser::input_core {

// gamepad-input.mjs: sequence (odd while written), connected, the menu mask, lx, ly, rx, ry (y up), then the physical
// buttons in kPadNames order (bits 0-15) with the left and right trigger values in bits 16-23 and 24-31
using GamepadWords = std::array<std::atomic<uint32_t>, 8>;
// the gamepad of the session that runs, null between sessions
void BindGamepad(GamepadWords* words);
// input::Poll(port) as XInputGetState's 18 byte XINPUT_GAMEPAD; false for a port with nothing plugged in
bool Sample(uint32_t port, std::array<uint8_t, 18>& sample);

}
