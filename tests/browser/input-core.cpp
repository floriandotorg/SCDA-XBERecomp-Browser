#include "core/input.h"
#include "app/app.h"
#include "browser/host-messages.h"
#include "browser/host-settings.h"
#include "browser/input-core.h"
#include "browser/keyboard-input.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace ki = browser::keyboard_input;
namespace hs = browser::host_settings;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

void Key(int vk, bool down) {
  auto& word = ki::Shared()[ki::kKeys + uint32_t(vk) / 32];
  if (down) { word.fetch_or(1u << (vk % 32)); } else { word.fetch_and(~(1u << (vk % 32))); }
  ki::Shared()[ki::kChange].fetch_add(1);
}
void Release() {
  for (uint32_t n = 0; n < 8; ++n) { ki::Shared()[ki::kKeys + n] = 0; }
}
void Notch(int notches) { ki::Shared()[notches > 0 ? ki::kWheelUp : ki::kWheelDown].fetch_add(uint32_t(std::abs(notches))); }

void Publish(float free, float weapon, float sniper, bool invert, const std::string& text) {
  auto& block = hs::Shared();
  const uint32_t revision = block.words[hs::kRevisionWord].load();
  block.words[hs::kRevisionWord] = revision + 1;
  block.words[hs::kSchemaWord] = hs::kSchema;
  block.words[hs::kFlagsWord] = invert ? 1u << hs::kInvertMouseBit : 0;
  block.words[hs::kFreeLookSensitivityWord] = std::bit_cast<uint32_t>(free);
  block.words[hs::kWeaponSensitivityWord] = std::bit_cast<uint32_t>(weapon);
  block.words[hs::kSniperSensitivityWord] = std::bit_cast<uint32_t>(sniper);
  for (uint32_t n = 0; n < block.text.size(); ++n) {
    uint32_t word = 0;
    for (uint32_t byte = 0; byte < 4; ++byte) {
      if (n * 4 + byte < text.size()) { word |= uint32_t(uint8_t(text[n * 4 + byte])) << (byte * 8); }
    }
    block.text[n] = word;
  }
  block.words[hs::kRevisionWord] = revision + 2;
}

// upstream's mouse and controller settings over what Publish left
void PublishInput(uint32_t flags, float vertical, float pad, float padAim, float deadZone, uint32_t vibration) {
  auto& block = hs::Shared();
  const uint32_t revision = block.words[hs::kRevisionWord].load();
  block.words[hs::kRevisionWord] = revision + 1;
  block.words[hs::kFlagsWord] = flags;
  block.words[hs::kMouseVerticalSensitivityWord] = std::bit_cast<uint32_t>(vertical);
  block.words[hs::kPadHorizontalSensitivityWord] = std::bit_cast<uint32_t>(pad);
  block.words[hs::kPadVerticalSensitivityWord] = std::bit_cast<uint32_t>(pad * 2);
  block.words[hs::kPadAimHorizontalSensitivityWord] = std::bit_cast<uint32_t>(padAim);
  block.words[hs::kPadAimVerticalSensitivityWord] = std::bit_cast<uint32_t>(padAim * 2);
  block.words[hs::kPadDeadZoneWord] = std::bit_cast<uint32_t>(deadZone);
  block.words[hs::kVibrationStrengthWord] = vibration;
  block.words[hs::kRevisionWord] = revision + 2;
}

bool Near(int16_t value, float expected) { return std::abs(float(value) - expected * 32767.0f) < 2.0f; }

int main() {
  constexpr int W = 'W', S = 'S', D = 'D', Escape = 0x1B, Numpad5 = 0x65, Lmb = 0x01, Delete = 0x2E;
  ki::Shared()[ki::kActive] = 1;
  Publish(1, 2, 3, false, "");

  Key(W, true);
  auto pad = input::Poll(0);
  check(pad.connected && Near(pad.ly, 1.0f) && !pad.lx, "the forward key pushes the stick fully at the top walking speed");
  check(input::StickKeys() == 1, "stick keys");
  Notch(-1);
  pad = input::Poll(0);
  check(Near(pad.ly, 0.879f), "a wheel notch down steps the walking speed");
  Key(D, true);
  pad = input::Poll(0);
  check(Near(pad.ly, 0.3f + (0.879f - 0.3f) * 0.70710678f) && pad.lx == pad.ly, "a diagonal keeps the speed past the dead zone");
  Key(D, false);
  Notch(1);
  input::Poll(0);
  Key(W, false);

  input::SetLadder(true, false);
  Key(S, true);
  pad = input::Poll(0);
  check(Near(pad.ly, -0.759f), "the back key steps down a ladder");
  input::SetLadder(false, false);
  input::SetPipe(true);
  pad = input::Poll(0);
  check(Near(pad.ly, -1.0f), "the back key slides down a pipe at the top walking speed");
  input::SetPipe(false);
  Key(S, false);

  input::SetView(input::View::Lockpick);
  Key(W, true);
  pad = input::Poll(0);
  check(Near(pad.ly, 1.0f) && Near(pad.lx, -1.0f), "lockpicking: W is the up left quadrant");
  Key(W, false);
  pad = input::Poll(0);
  check(Near(pad.ly, 0.6f) && Near(pad.lx, -0.6f), "lockpicking: let go, the stick rests in the quadrant");
  input::SetView(input::View::Normal);

  Key(Escape, true);
  pad = input::Poll(0);
  check(pad.buttons & 0x10, "pause presses Start in play");
  Key(Escape, false);
  input::Poll(0);
  input::SetMenu(true);
  Key(Escape, true);
  pad = input::Poll(0);
  check(!(pad.buttons & 0x10), "a menu settles keys held when it opened");
  Key(Escape, false);
  input::Poll(0);
  Key(Escape, true);
  pad = input::Poll(0);
  check(pad.analog[1] == 0xFF && !(pad.buttons & 0x10), "Escape backs out of a menu as B");
  Key(Escape, false);
  input::Poll(0);
  input::SetPauseMenu(true);
  Key(Escape, true);
  pad = input::Poll(0);
  check(pad.buttons & 0x10, "the pause menu keeps Escape as Start");
  Key(Escape, false);
  input::SetPauseMenu(false);
  Key(Lmb, true);
  pad = input::Poll(0);
  check(!pad.analog[7], "in a menu the left button is the cursor's, not fire");
  Key(Lmb, false);
  Notch(2);
  check(input::TakeMenuWheel() == 2, "menus take the wheel");
  input::SetMenu(false);
  input::Poll(0);

  input::SetOpsat(true);
  Key(Escape, true);
  input::Poll(0);
  pad = input::Poll(0);
  check(pad.analog[1] == 0xFF, "the OPSAT backs out on Escape");
  Key(Escape, false);
  input::SetOpsat(false);
  input::Poll(0);

  input::SetView(input::View::Eev);
  Notch(1);
  check(input::TakeWheel() == 1, "the EEV zooms one level a notch");
  input::SetView(input::View::Normal);

  Key(Numpad5, true);
  check(input::TakeKeypadButton() == 4 && input::TakeKeypadButton() == -1, "numpad 5 is keypad button 5, once a press");
  Key(Numpad5, false);

  ki::Shared()[ki::kMouseX] = 1000;
  ki::Shared()[ki::kMouseY] = uint32_t(-2000);
  float dx = 0, dy = 0;
  check(input::TakeMouse(dx, dy) && dx == 1 && dy == -2, "free look sensitivity");
  input::SetTargeting(true);
  ki::Shared()[ki::kMouseX] = 1000;
  check(input::TakeMouse(dx, dy) && dx == 2, "weapon sensitivity while aiming");
  input::SetTargeting(false);
  input::SetView(input::View::Eev);
  ki::Shared()[ki::kMouseX] = 1000;
  check(input::TakeMouse(dx, dy) && dx == 3, "sniper sensitivity in the EEV");
  input::SetView(input::View::Normal);
  Publish(1, 2, 3, true, "");
  ki::Shared()[ki::kMouseY] = 1000;
  check(input::TakeMouse(dx, dy) && dy == -1, "inverted look");
  PublishInput(1u << hs::kInvertMouseXBit, 0.5f, 1, 1, 0, 100);
  ki::Shared()[ki::kMouseX] = 1000;
  ki::Shared()[ki::kMouseY] = 1000;
  check(input::TakeMouse(dx, dy) && dx == -1 && dy == 0.5f, "inverted X and the vertical sensitivity");

  Publish(1, 2, 3, false, "keys.fire=G\npad_buttons.jump=B\n");
  check(input::Binding(9) == "G", "a rebind from the page reloads the binds");
  check(input::PadBinding(0) == "B" && input::PadBinding(1).empty(), "pad binds settle a taken button");
  input::Bind(9, 'H', 1);
  check(input::Binding(9) == "G,H", "a guest rebind applies at once");
  bool posted = false;
  while (auto message = browser::host_messages::Take()) {}
  auto& block = browser::host_messages::Shared();
  for (uint32_t n = block[browser::host_messages::kGuestTail]; n != block[browser::host_messages::kGuestHead]; ++n) {
    const uint32_t entry = browser::host_messages::kGuestRing + (n % browser::host_messages::kEntries) * browser::host_messages::kEntryWords;
    std::string text(block[entry + 1].load(), '\0');
    for (uint32_t c = 0; c < text.size(); ++c) { text[c] = char(block[entry + 2 + c / 4].load() >> ((c % 4) * 8)); }
    posted |= text == "keys.fire=G,H";
  }
  check(posted, "the guest rebind went to the page");
  input::Unbind(29);
  check(input::Binding(29) == "F10", "the settings panel keeps its key");

  Key(Delete, true);
  check(input::TakeDeletePress() && !input::TakeDeletePress(), "Delete once a press");
  Key(Delete, false);
  ki::Shared()[ki::kActive] = 0;
  Key(W, true);
  pad = input::Poll(0);
  check(!pad.ly, "an inactive page gives the game no keys");
  Release();

  browser::input_core::GamepadWords gamepad{};
  browser::input_core::BindGamepad(&gamepad);
  ki::Shared()[ki::kActive] = 1;
  gamepad[1] = 1;
  gamepad[7] = 1u << 0;
  pad = input::Poll(0);
  check(pad.analog[0] == 0xFF, "the pad's A");
  input::SetMenu(false);
  input::SetTitleScreen(false);
  Publish(1, 2, 3, false, "pad_buttons.jump=A\npad_buttons.interact=Y\n");
  gamepad[7] = 0;
  input::Poll(0);
  gamepad[7] = 1u << 0;
  pad = input::Poll(0);
  check(pad.analog[3] == 0xFF && !pad.analog[0], "in play A presses jump's Y");
  input::SetMenu(true);
  gamepad[7] = 0;
  input::Poll(0);
  gamepad[7] = 1u << 0;
  pad = input::Poll(0);
  check(pad.analog[0] == 0xFF, "menus keep the stock buttons");
  check(input::PressedPadButton() == 0, "a held button answers a capture");
  std::array<uint8_t, 18> sample{};
  check(browser::input_core::Sample(0, sample) && sample[2] == 0xFF, "XInput sample");
  check(!browser::input_core::Sample(2, sample), "port 2 has nothing");
  gamepad[7] = 0;
  Key(W, true);
  input::Poll(0);
  Key(W, false);
  input::Poll(0);
  check(input::KeyboardActive(), "a key after the pad shows keyboard prompts");
  gamepad[1] = 2;
  input::Poll(0);
  check(!input::KeyboardActive(), "the on-screen pad of a touch screen shows the pad's prompts");
  Key(W, true);
  input::Poll(0);
  Key(W, false);
  input::Poll(0);
  check(!input::KeyboardActive(), "while it is shown even after a key");
  gamepad[1] = 0;
  Key(W, true);
  input::Poll(0);
  Key(W, false);
  check(input::KeyboardActive(), "without it keys show keyboard prompts again");

  namespace ic = browser::input_core;
  gamepad[1] = 1;
  gamepad[2] = 0x7FFF;
  gamepad[3] = 20000;
  gamepad[4] = uint32_t(-20000);
  gamepad[7] = 0;
  pad = input::Poll(0);
  check(pad.lx == 20000 && pad.ly == -20000 && !pad.rx, "the sticks come from the words after the menu mask");
  gamepad[2] = gamepad[3] = gamepad[4] = 0;
  gamepad[5] = 16383;
  gamepad[6] = uint32_t(-16383);
  PublishInput(1u << hs::kInvertPadXBit, 1, 1, 1, 0, 100);
  pad = input::Poll(0);
  check(pad.rx == -16384 && pad.ry == -16383, "the right stick's X inverted, not plain negation");
  PublishInput(1u << hs::kInvertPadYBit, 1, 1, 1, 0.2f, 100);
  check(input::GameDeadZone() == 0.3f, "the story's own dead zone");
  pad = input::Poll(0);
  check(pad.rx == 18431 && pad.ry == 18430, "a dead zone of 0.2 starts the axis where the game's 0.3 does, after Y inverts");
  gamepad[5] = 6000;
  pad = input::Poll(0);
  check(pad.rx == 0, "inside the chosen dead zone the axis rests");
  PublishInput(0, 1, 1.5f, 0.5f, 0, 100);
  check(!input::Aiming() && input::PadLookScale(false).x == 1.5f && input::PadLookScale(false).y == 3.0f, "the look sensitivities");
  input::SetAiming(true);
  check(input::Aiming() && input::PadLookScale(true).x == 0.5f && input::PadLookScale(true).y == 1.0f, "the aim sensitivities while aiming");
  input::SetAiming(false);

  gamepad[7] = 1u << 0;
  input::Poll(0);
  PublishInput(0, 1, 1, 1, 0, 50);
  input::Rumble(0, 40000, 20000);
  check(gamepad[ic::kRumbleWord] == (20000u | 10000u << 16), "the motors at half the game's rumble");
  PublishInput(0, 1, 1, 1, 0, 200);
  input::Rumble(0, 40000, 20000);
  check(gamepad[ic::kRumbleWord] == (0xFFFFu | 40000u << 16), "twice the rumble tops out at the motor's full speed");
  gamepad[7] = 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  Key(W, true);
  input::Poll(0);
  Key(W, false);
  input::Rumble(0, 40000, 20000);
  check(gamepad[ic::kRumbleWord] == 0, "no rumble while the keyboard plays");
  browser::input_core::BindGamepad(nullptr);
  std::puts("input-core-contracts-ok");
}
