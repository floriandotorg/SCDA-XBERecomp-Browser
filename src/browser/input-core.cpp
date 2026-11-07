// src/core/input.cpp for the browser: the same action table (the story's), remaps, walking speeds, wheel routing,
// pad binds and key capture, over the page's raw input instead of Windows and SDL. Down(vk) reads the held key bitmap
// keyboard-input.mjs fills by virtual key code; the page's wheel notches, typed characters, clicks and Escape taps are
// taken in Pump, which every reader calls first. Story table only.
#include "core/input.h"
#include "browser/input-core.h"
#include "browser/keyboard-input.h"
#include "browser/host-messages.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <iterator>
#include <mutex>
#include <span>
#include <string_view>
#include <strings.h>
#include <thread>
#include <utility>
#include <vector>

#include "app/app.h"
#include "core/log.h"

namespace browser::keyboard_input {
Block& Shared() {
  static Block block;
  return block;
}
}

namespace browser::input_core {
namespace {
std::atomic<GamepadWords*> g_gamepad{nullptr};
}
void BindGamepad(GamepadWords* words) { g_gamepad.store(words, std::memory_order_release); }

bool Sample(uint32_t port, std::array<uint8_t, 18>& sample) {
  const input::Pad pad = input::Poll(int(port));
  if (!pad.connected) { return false; }
  sample[0] = uint8_t(pad.buttons);
  sample[1] = uint8_t(pad.buttons >> 8);
  std::copy(std::begin(pad.analog), std::end(pad.analog), sample.begin() + 2);
  const int16_t axes[] = {pad.lx, pad.ly, pad.rx, pad.ry};
  for (int n = 0; n < 4; ++n) {
    sample[10 + n * 2] = uint8_t(uint16_t(axes[n]));
    sample[11 + n * 2] = uint8_t(uint16_t(axes[n]) >> 8);
  }
  return true;
}
}

namespace input {

namespace {

namespace ki = browser::keyboard_input;

using WORD = uint16_t;
constexpr int VK_LBUTTON = 0x01, VK_RBUTTON = 0x02, VK_MBUTTON = 0x04, VK_XBUTTON1 = 0x05, VK_XBUTTON2 = 0x06;
constexpr int VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12;
constexpr int VK_PAUSE = 0x13, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22;
constexpr int VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28;
constexpr int VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E, VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_APPS = 0x5D;
constexpr int VK_NUMPAD0 = 0x60, VK_NUMPAD1 = 0x61, VK_NUMPAD2 = 0x62, VK_NUMPAD3 = 0x63, VK_NUMPAD4 = 0x64;
constexpr int VK_NUMPAD5 = 0x65, VK_NUMPAD6 = 0x66, VK_NUMPAD7 = 0x67, VK_NUMPAD8 = 0x68, VK_NUMPAD9 = 0x69;
constexpr int VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D, VK_DECIMAL = 0x6E, VK_DIVIDE = 0x6F;
constexpr int VK_F1 = 0x70, VK_F2 = 0x71, VK_F3 = 0x72, VK_F4 = 0x73, VK_F5 = 0x74, VK_F6 = 0x75, VK_F7 = 0x76;
constexpr int VK_F8 = 0x77, VK_F9 = 0x78, VK_F10 = 0x79, VK_F11 = 0x7A, VK_F12 = 0x7B, VK_NUMLOCK = 0x90, VK_SCROLL = 0x91;
constexpr int VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3, VK_LMENU = 0xA4, VK_RMENU = 0xA5;
constexpr int VK_BROWSER_BACK = 0xA6, VK_LAUNCH_APP2 = 0xB7;
constexpr int VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB, VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE;
constexpr int VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB, VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE;
constexpr WORD XINPUT_GAMEPAD_DPAD_UP = 0x0001, XINPUT_GAMEPAD_DPAD_DOWN = 0x0002, XINPUT_GAMEPAD_DPAD_LEFT = 0x0004;
constexpr WORD XINPUT_GAMEPAD_DPAD_RIGHT = 0x0008, XINPUT_GAMEPAD_START = 0x0010, XINPUT_GAMEPAD_BACK = 0x0020;
constexpr WORD XINPUT_GAMEPAD_LEFT_THUMB = 0x0040, XINPUT_GAMEPAD_RIGHT_THUMB = 0x0080;
constexpr WORD XINPUT_GAMEPAD_LEFT_SHOULDER = 0x0100, XINPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200;
constexpr WORD XINPUT_GAMEPAD_A = 0x1000, XINPUT_GAMEPAD_B = 0x2000, XINPUT_GAMEPAD_X = 0x4000, XINPUT_GAMEPAD_Y = 0x8000;
constexpr uint8_t XINPUT_GAMEPAD_TRIGGER_THRESHOLD = 30;
constexpr int WHEEL_DELTA = 120;
struct XINPUT_GAMEPAD {
  WORD wButtons;
  uint8_t bLeftTrigger, bRightTrigger;
  int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY;
};

std::atomic<bool> g_blocked{false};
std::atomic<bool> g_menu{false};
std::atomic<bool> g_pointer{false};
bool Pointing() { return g_menu || g_pointer; }
std::atomic<bool> g_settle{false};
std::atomic<bool> g_settle_mouse{false};
std::atomic<uint64_t> g_keyboard_ms{0};
std::atomic<uint64_t> g_pad_ms{0};
std::atomic<uint32_t> g_keys_held{0};
std::atomic<uint32_t> g_pad_held{0};
std::atomic<uint64_t> g_movie_ms{0};
constexpr uint64_t kMovieMs = 250;
std::atomic<uint64_t> g_skip_ms{0};
constexpr uint64_t kSkipMs = 250;

uint64_t NowMs() {
  auto since = std::chrono::steady_clock::now().time_since_epoch();
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(since).count()) + 1;
}

bool KeyboardLast() { return g_keyboard_ms > g_pad_ms; }
// gamepad-input.mjs connects a touch screen's on-screen pad as 2: the player holds the pad its prompts name
std::atomic<bool> g_touch_pad{false};

std::atomic<int> g_view{0};
std::atomic<float> g_zoom_speed{0.0f};
std::atomic<int> g_wheel{0};
std::atomic<int> g_menu_wheel{0};
constexpr int kWheelUp = 0x0E, kWheelDown = 0x0F;
constexpr uint64_t kWheelTapMs = 80;
std::atomic<uint64_t> g_wheel_ms[2] = {};
std::atomic<bool> g_list{false};
std::atomic<uint16_t> g_tap{0};
std::atomic<uint64_t> g_tap_until{0};

constexpr float kNotchDegrees = 105.0f / 30.0f;
constexpr float kMapNotchSeconds = 1.0f / 30.0f;
float g_zoom_left = 0.0f;
std::chrono::steady_clock::time_point g_zoom_time;

struct Remap {
  int from;
  int to;
};
constexpr int kAnalogA = 0;
constexpr int kAnalogB = 1;
constexpr int kBlack = 4;
constexpr int kAltFire = 6;
constexpr int kButtonTarget = 8;
constexpr int kPauseTarget = kButtonTarget + 4;
constexpr int kMoveTarget = 16;
constexpr Remap kCameraRemap[] = {
  {7, 0}, {6, 2}, {kPauseTarget, 1}, {kMoveTarget + 2, 6}, {kMoveTarget + 3, 7}, {0, -1}, {2, -1}};
std::array<std::atomic<int>, 32> g_down_as{};
constexpr Remap kMenuRemap[] = {{kPauseTarget, 1}};
constexpr Remap kOpsatRemap[] = {{kPauseTarget, 1}};
constexpr Remap kMapRemap[] = {{kPauseTarget, 1}, {kMoveTarget + 2, 6}, {kMoveTarget + 3, 7}};
std::atomic<bool> g_title{false};
std::atomic<bool> g_pause_menu{false};
std::atomic<bool> g_opsat{false};
std::atomic<bool> g_quick{false};
std::atomic<bool> g_text{false};
std::atomic<bool> g_map{false};
std::mutex g_chars_lock;
std::u16string g_chars;
std::atomic<uint32_t> g_stick_keys{0};
constexpr uint16_t kStart = 1 << 4;

template <size_t N>
int Remapped(const Remap (&table)[N], int b) {
  for (const Remap& r : table) {
    if (r.from == b) { return r.to; }
  }
  return b;
}

constexpr float kPickStick[4][2] = {{1.0f, -1.0f}, {-1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}};
constexpr float kPickRest = 0.6f;
int g_pick_last = -1;

constexpr float kSpeeds[] = {0.44f, 0.638f, 0.759f, 0.879f, 1.0f};
constexpr float kSlowestTargeting = 0.517f;
constexpr float kDeadZone = 0.3f;
constexpr float kStickMax = 32767.0f;
std::atomic<int> g_speed{4};
std::atomic<bool> g_targeting{false};

std::atomic<uint32_t> g_events{0};

std::atomic<bool> g_net{false};
std::atomic<bool> g_video{false};
std::atomic<bool> g_interact_reloads{false};
std::atomic<bool> g_carrying{false};
std::atomic<bool> g_hanging{false};
std::atomic<bool> g_ladder{false};
std::atomic<bool> g_ladder_step{false};
std::atomic<bool> g_pipe{false};
constexpr float kClimbDown = 0.759f;
bool g_ladder_slide = false;

struct Target {
  Action action;
  int button;
  bool play_only = false;
  KeyEvent event = KeyEvent::None;
};
constexpr int kHostKey = -2;
// input.cpp's kStoryActions; the browser reserves Ctrl (Ctrl with W closes the tab), so quick inventory is Tab
constexpr Target kActions[] = {
  {{"move_forward", "MOVE FORWARD", "W"}, 16},
  {{"move_back", "MOVE BACK", "S"}, 17},
  {{"strafe_left", "MOVE LEFT", "A"}, 18},
  {{"strafe_right", "MOVE RIGHT", "D"}, 19},
  {{"jump", "JUMP / SPLIT JUMP", "Shift"}, 3},
  {{"crouch", "CROUCH", "C"}, 1},
  {{"zoom_in", "ZOOM IN / ACCELERATE", "Wheel Up"}, -1, false, KeyEvent::ZoomIn},
  {{"zoom_out", "ZOOM OUT / DECELERATE", "Wheel Down"}, -1, false, KeyEvent::ZoomOut},
  {{"back_to_wall", "BACK TO WALL / SWITCH HAND", "Q"}, 14},
  {{"fire", "FIRE", "LMB"}, 7},
  {{"alt_fire", "ALTERNATE FIRE", "RMB"}, 6},
  {{"interact", "INTERACTION", "Space,Enter"}, 0},
  {{"scope", "USE / EQUIP", "E,MMB"}, 2},
  {{"reload", "RELOAD", "R"}, -1, true, KeyEvent::Reload},
  {{"whistle", "WHISTLE", "V"}, -1, true, KeyEvent::Whistle},
  {{"quick_inventory", "QUICK INVENTORY", "Tab"}, 5},
  {{"opsat", "OPSAT", "F1"}, 13},
  {{"zoom", "EEV VISION / ZOOM / SNIPER", "1"}, 15},
  {{"previous_vision", "NIGHT VISION", "2"}, 10},
  {{"next_vision", "THERMAL VISION", "3"}, 11},
  {{"goggles", "EMF VISION", "4"}, 8},
  {{"switch_camera", "STICKY CAM VISION", "5"}, 9, true},
  {{"sidekick", "COOP ACTION", "F"}, -1, true, KeyEvent::SidekickCommand},
  {{"push_to_talk", "PUSH TO TALK", "T"}, kHostKey, false, KeyEvent::PushToTalk},
  {{"pause", "PAUSE", "Escape"}, 12},
  {{"quick_save", "QUICK SAVE", "F5"}, -1, true, KeyEvent::QuickSave},
  {{"quick_load", "QUICK LOAD", "F8"}, -1, true, KeyEvent::QuickLoad},
  {{"console", "CONSOLE", "Backtick"}, kHostKey},
  {{"hide_hud", "TOGGLE HUD", ""}, kHostKey, false, KeyEvent::HideHud},
  {{"settings_panel", "SETTINGS PANEL", "F10"}, kHostKey, false, KeyEvent::SettingsPanel},
};
constexpr size_t kCount = std::size(kActions);
static_assert(kCount <= 32);
struct PadAction {
  const char* id;
  const char* label;
  int button;
  KeyEvent event = KeyEvent::None;
};
constexpr PadAction kPadActions[] = {
  {"jump", "JUMP / SPLIT JUMP", 3},
  {"crouch", "CROUCH / STAND UP / EXIT INTERACTION", 1},
  {"back_to_wall", "BACK TO WALL / SWITCH HAND / ZOOM IN OUT", 14},
  {"fire", "FIRE", 7},
  {"alt_fire", "ALTERNATE FIRE", 6},
  {"interact", "INTERACTION / RELOAD", 0},
  {"scope", "USE / EQUIP", 2},
  {"whistle", "WHISTLE / COOP ACTION", 4},
  {"quick_inventory", "QUICK INVENTORY", 5},
  {"opsat", "OPSAT", 13},
  {"zoom", "EEV VISION / ZOOM / SNIPER", 15},
  {"night_vision", "NIGHT VISION", 10},
  {"thermal_vision", "THERMAL VISION", 11},
  {"emf_vision", "EMF VISION", 8},
  {"sticky_cam_vision", "STICKY CAM VISION", 9},
  {"pause", "PAUSE", 12},
  {"quick_save", "QUICK SAVE", -1, KeyEvent::QuickSave},
  {"quick_load", "QUICK LOAD", -1, KeyEvent::QuickLoad},
  {"push_to_talk", "PUSH TO TALK", -1, KeyEvent::PushToTalk},
  {"toggle_hud", "TOGGLE HUD", -1, KeyEvent::HideHud},
};
constexpr size_t kPadCount = std::size(kPadActions);
constexpr int kPadControls = 16;
constexpr WORD kTouchpadLeft = 0x0400;
constexpr WORD kTouchpadRight = 0x0800;
constexpr int kPadButtons = kPadControls + 2;
const char* const kPadNames[kPadButtons] = {
  "A", "B", "X", "Y", "RB", "LB", "LT", "RT",
  "D-PAD UP", "D-PAD DOWN", "D-PAD LEFT", "D-PAD RIGHT", "START", "BACK", "LS", "RS",
  "TOUCHPAD LEFT", "TOUCHPAD RIGHT",
};
constexpr WORD kPadMasks[kPadButtons] = {XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                                         XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_LEFT_SHOULDER, 0, 0,
                                         XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
                                         XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
                                         XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, kTouchpadLeft,
                                         kTouchpadRight};
std::array<int, kPadCount> g_pad_binds{};
constexpr int kEventTarget = 32;
std::array<std::atomic<int>, kPadButtons> g_pad_map{};
std::array<std::array<int, kPadButtons>, 4> g_pad_down_as{};
std::array<std::atomic<uint32_t>, 4> g_pad_events_held{};
std::array<std::atomic<bool>, 4> g_pad_settle{};

constexpr size_t kConsoleAction = [] {
  for (size_t i = 0; i < kCount; i++) {
    if (kActions[i].button == kHostKey && kActions[i].event == KeyEvent::None) { return i; }
  }
  return kCount;
}();

struct Name {
  int vk;
  const char* name;
};
constexpr Name kNames[] = {
  {VK_LBUTTON, "LMB"}, {VK_RBUTTON, "RMB"}, {VK_MBUTTON, "MMB"}, {VK_XBUTTON1, "Mouse4"}, {VK_XBUTTON2, "Mouse5"},
  {kWheelUp, "Wheel Up"}, {kWheelDown, "Wheel Down"},
  {VK_SPACE, "Space"}, {VK_RETURN, "Enter"}, {VK_ESCAPE, "Escape"}, {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},
  {VK_SHIFT, "Shift"}, {VK_CONTROL, "Ctrl"}, {VK_MENU, "Alt"}, {VK_CAPITAL, "CapsLock"},
  {VK_UP, "Up"}, {VK_DOWN, "Down"}, {VK_LEFT, "Left"}, {VK_RIGHT, "Right"},
  {VK_INSERT, "Insert"}, {VK_DELETE, "Delete"}, {VK_HOME, "Home"}, {VK_END, "End"},
  {VK_PRIOR, "PageUp"}, {VK_NEXT, "PageDown"},
  {'A', "A"}, {'B', "B"}, {'C', "C"}, {'D', "D"}, {'E', "E"}, {'F', "F"}, {'G', "G"}, {'H', "H"}, {'I', "I"},
  {'J', "J"}, {'K', "K"}, {'L', "L"}, {'M', "M"}, {'N', "N"}, {'O', "O"}, {'P', "P"}, {'Q', "Q"}, {'R', "R"},
  {'S', "S"}, {'T', "T"}, {'U', "U"}, {'V', "V"}, {'W', "W"}, {'X', "X"}, {'Y', "Y"}, {'Z', "Z"},
  {'0', "0"}, {'1', "1"}, {'2', "2"}, {'3', "3"}, {'4', "4"}, {'5', "5"}, {'6', "6"}, {'7', "7"}, {'8', "8"},
  {'9', "9"},
  {VK_F1, "F1"}, {VK_F2, "F2"}, {VK_F4, "F4"}, {VK_F5, "F5"}, {VK_F6, "F6"}, {VK_F7, "F7"},
  {VK_F8, "F8"}, {VK_F9, "F9"}, {VK_F10, "F10"}, {VK_F11, "F11"}, {VK_F12, "F12"},
  {VK_NUMPAD0, "Numpad0"}, {VK_NUMPAD1, "Numpad1"}, {VK_NUMPAD2, "Numpad2"}, {VK_NUMPAD3, "Numpad3"},
  {VK_NUMPAD4, "Numpad4"}, {VK_NUMPAD5, "Numpad5"}, {VK_NUMPAD6, "Numpad6"}, {VK_NUMPAD7, "Numpad7"},
  {VK_NUMPAD8, "Numpad8"}, {VK_NUMPAD9, "Numpad9"}, {VK_ADD, "NumpadPlus"}, {VK_SUBTRACT, "NumpadMinus"},
  {VK_MULTIPLY, "NumpadStar"}, {VK_DIVIDE, "NumpadSlash"}, {VK_DECIMAL, "NumpadPeriod"},
  {VK_OEM_3, "Backtick"}, {VK_OEM_MINUS, "Minus"}, {VK_OEM_PLUS, "Equals"}, {VK_OEM_4, "LBracket"},
  {VK_OEM_6, "RBracket"}, {VK_OEM_5, "Backslash"}, {VK_OEM_1, "Semicolon"}, {VK_OEM_7, "Quote"},
  {VK_OEM_COMMA, "Comma"}, {VK_OEM_PERIOD, "Period"}, {VK_OEM_2, "Slash"},
};

constexpr int kArrows[] = {VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT};

std::mutex g_bind_lock;
std::array<std::vector<int>, kCount> g_binds;
std::atomic<uint32_t> g_loaded{~0u};  // the settings revision the binds were loaded from

int Vk(const std::string& name) {
  for (const Name& n : kNames) {
    if (strcasecmp(n.name, name.c_str()) == 0) { return n.vk; }
  }
  return 0;
}

const char* NameOf(int vk) {
  for (const Name& n : kNames) {
    if (n.vk == vk) { return n.name; }
  }
  return nullptr;
}

std::vector<int> Parse(const std::string& list) {
  std::vector<int> out;
  size_t start = 0;
  while (start <= list.size()) {
    size_t comma = list.find(',', start);
    if (comma == std::string::npos) { comma = list.size(); }
    if (int vk = Vk(list.substr(start, comma - start))) { out.push_back(vk); }
    start = comma + 1;
  }
  return out;
}

// the page's key codes are positional, as native binds: the names are the US keys
std::string Join(const std::vector<int>& vks) {
  std::string s;
  for (int vk : vks) {
    if (!s.empty()) { s += ','; }
    s += NameOf(vk);
  }
  return s;
}

ki::Block& Raw() { return ki::Shared(); }
uint32_t Word(uint32_t index) { return Raw()[index].load(std::memory_order_acquire); }

bool Active() { return Word(ki::kActive) && !g_blocked; }
bool KeyBit(int vk) { return vk > 0 && vk < 256 && (Word(ki::kKeys + uint32_t(vk) / 32) >> (vk % 32) & 1); }
bool Down(int vk) {
  if (vk == kWheelUp || vk == kWheelDown) { return NowMs() - g_wheel_ms[vk == kWheelDown] < kWheelTapMs; }
  return KeyBit(vk);
}
bool HeldKeys(KeyEvent event) {
  std::vector<int> keys;
  {
    std::lock_guard lock(g_bind_lock);
    for (size_t i = 0; i < kCount; i++) {
      if (kActions[i].event == event) { keys = g_binds[i]; }
    }
  }
  return std::any_of(keys.begin(), keys.end(), Down);
}
bool Bound(KeyEvent event, int vk) {
  std::lock_guard lock(g_bind_lock);
  for (size_t i = 0; i < kCount; i++) {
    if (kActions[i].event != event) { continue; }
    if (std::find(g_binds[i].begin(), g_binds[i].end(), vk) != g_binds[i].end()) { return true; }
  }
  return false;
}
std::atomic<uint64_t> g_capture_ms{0};
bool Mouse(int vk) {
  return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2 ||
         vk == kWheelUp || vk == kWheelDown;
}

bool AnyKeyDown(const std::array<std::vector<int>, kCount>& binds) {
  for (auto& b : binds) {
    for (int vk : b) {
      if (Down(vk)) { return true; }
    }
  }
  for (int vk : kArrows) {
    if (Down(vk)) { return true; }
  }
  return Down(VK_BACK) || Down(VK_RETURN);
}

void SettleMouse() {
  if (!g_settle_mouse) { return; }
  if (!Down(VK_LBUTTON) && !Down(VK_RBUTTON) && !Down(VK_MBUTTON) && !Down(VK_XBUTTON1) && !Down(VK_XBUTTON2)) {
    g_settle_mouse = false;
  }
}

int ZoomOf(size_t i) {
  KeyEvent e = kActions[i].event;
  return e == KeyEvent::ZoomIn ? 1 : e == KeyEvent::ZoomOut ? -1 : 0;
}

void Notch(int notch);

bool ActionHeld(size_t i, const std::vector<int>& vks, bool no_mouse, bool menu) {
  bool held = false;
  for (int vk : vks) {
    bool wheel = vk == kWheelUp || vk == kWheelDown;
    held |= Down(vk) && !(no_mouse && Mouse(vk)) && !(menu && vk == VK_DELETE) &&
            !(wheel && (ZoomOf(i) || g_list));
  }
  if (kActions[i].play_only && menu) { held = false; }
  if (g_text && kActions[i].button != kPauseTarget) { held = false; }
  return held;
}

void ApplyEventKey(size_t i, Pad& p) {
  if (int zoom = ZoomOf(i)) {
    if (!g_down_as[i]) { Notch(zoom); }
    g_down_as[i] = 1;
    return;
  }
  KeyEvent e = kActions[i].event;
  if (g_video && (e == KeyEvent::SidekickCommand || e == KeyEvent::Whistle)) {
    p.analog[kBlack] = 0xFF;
    g_down_as[i] = 1;
    return;
  }
  if (g_net) {
    if (kActions[i].event == KeyEvent::SidekickCommand) { p.analog[kBlack] = 0xFF; }
    if (kActions[i].event == KeyEvent::Reload && g_interact_reloads) { p.analog[kAnalogA] = 0xFF; }
    g_down_as[i] = 1;
    return;
  }
  if (!g_down_as[i]) { g_events |= 1u << int(kActions[i].event); }
  g_down_as[i] = 1;
}

int RemapTarget(int button, bool camera, bool back, bool menu) {
  if (camera) { return Remapped(kCameraRemap, button); }
  if (g_map) { return Remapped(kMapRemap, button); }
  if (back) { return Remapped(kOpsatRemap, button); }
  if (menu && !g_pause_menu) { return Remapped(kMenuRemap, button); }
  return button;
}

int LatchTarget(size_t i, bool camera, bool back, bool menu) {
  if (!g_down_as[i] && kActions[i].button == 0 && g_interact_reloads && !menu) { g_down_as[i] = -1; }
  if (!g_down_as[i] && kActions[i].button == kAltFire && g_carrying && !menu && !g_net) {
    g_events |= 1u << int(KeyEvent::DropBody);
    g_down_as[i] = -1;
  }
  int b = g_down_as[i] - 1;
  if (b == -1) {
    int button = kActions[i].button;
    b = RemapTarget(button, camera, back, menu);
    g_down_as[i] = b < 0 ? -1 : b + 1;
    if (b >= kMoveTarget) { g_pick_last = b - kMoveTarget; }
  }
  return b;
}

void PressTarget(int b, Pad& p, int (&move)[4]) {
  if (b < 0) { return; }
  if (b >= kMoveTarget) { move[b - kMoveTarget] = 1; }
  else if (b < kButtonTarget) { p.analog[b] = 0xFF; }
  else { p.buttons |= uint16_t(1u << (b - kButtonTarget)); }
  if (b == kAnalogA && g_title) { p.buttons |= kStart; }
}

void PressMenuKeys(Pad& p, bool menu, bool back, bool keypad) {
  if ((Down(VK_BACK) && (menu || back) && !g_text && !keypad) || (menu && !g_map && Down(VK_RBUTTON))) {
    p.analog[kAnalogB] = 0xFF;
  }
  if (menu && g_net && !g_settle_mouse && Down(VK_LBUTTON)) { p.analog[kAnalogA] = 0xFF; }
  if (menu && Down(VK_RETURN)) {
    p.analog[kAnalogA] = 0xFF;
    if (g_title) { p.buttons |= kStart; }
  }
  if (g_title && Pressing()) { p.buttons |= kStart; }
  bool scripted = !menu && View(g_view.load()) == View::ScriptedEvent;
  if (scripted && Down(VK_RETURN) && NowMs() - g_skip_ms < kSkipMs) { p.buttons |= kStart; }
}

void NoteKeysHeld(uint32_t keys) {
  for (int i = 0; i < 4; i++) { keys |= uint32_t(Down(kArrows[i])) << (27 + i); }
  keys |= uint32_t(Down(VK_BACK)) << 31;
  if (keys & ~g_keys_held.exchange(keys)) { g_keyboard_ms = NowMs(); }
}

float StickSpeed(bool full) {
  if (full) { return 1.0f; }
  if (g_speed == 0 && g_targeting) { return kSlowestTargeting; }
  return kSpeeds[g_speed];
}

float NotchSeconds(bool camera) {
  float zoom_speed = g_zoom_speed;
  if (camera && zoom_speed > 0.0f) { return kNotchDegrees / zoom_speed; }
  if (g_map) { return kMapNotchSeconds; }
  return 0.0f;
}

int WheelZoom(bool camera, int y) {
  auto now = std::chrono::steady_clock::now();
  float seconds = std::min(0.1f, std::chrono::duration<float>(now - g_zoom_time).count());
  g_zoom_time = now;
  float notch = NotchSeconds(camera);
  if (notch <= 0.0f) {
    g_zoom_left = 0.0f;
    return y;
  }
  if (int notches = g_wheel.exchange(0)) {
    if (notches * g_zoom_left < 0.0f) { g_zoom_left = 0.0f; }
    g_zoom_left += float(notches) * notch;
  }
  if (g_zoom_left > 0.0f) { g_zoom_left = std::max(0.0f, g_zoom_left - seconds); }
  if (g_zoom_left < 0.0f) { g_zoom_left = std::min(0.0f, g_zoom_left + seconds); }
  if (!y && g_zoom_left != 0.0f) { y = g_zoom_left > 0.0f ? 1 : -1; }
  return y;
}

constexpr float kInvSqrt2 = 0.70710678f;

float DiagonalSpeed(float v) { return kDeadZone + (v - kDeadZone) * kInvSqrt2; }

void PickStick(Pad& p, const int (&move)[4]) {
  bool held = g_pick_last >= 0 && move[g_pick_last];
  for (int i = 0; i < 4 && !held; i++) {
    if (move[i]) {
      g_pick_last = i;
      held = true;
    }
  }
  if (g_pick_last < 0) { return; }
  float length = held ? 1.0f : kPickRest;
  p.ly = int16_t(kPickStick[g_pick_last][0] * length * kStickMax);
  p.lx = int16_t(kPickStick[g_pick_last][1] * length * kStickMax);
}

void UpdateStick(Pad& p, const int (&move)[4], bool camera, bool menu, bool back) {
  int x = move[3] - move[2];
  int y = move[0] - move[1];
  bool full = camera || menu || g_quick || View(g_view.load()) == View::Ledge;
  float v = StickSpeed(full);
  y = WheelZoom(camera, y);
  bool arrows = menu || back || g_map || g_quick;
  int ax = arrows ? Down(VK_RIGHT) - Down(VK_LEFT) : 0;
  int ay = arrows ? Down(VK_UP) - Down(VK_DOWN) : 0;
  if (ax || ay) {
    x = ax;
    y = ay;
    v = 1.0f;
  }
  bool step = g_ladder_step, top = g_speed == int(std::size(kSpeeds)) - 1;
  bool down = y < 0 && !menu && !ax && !ay;
  if (!g_ladder || !down) { g_ladder_slide = false; }
  else if (step && top) { g_ladder_slide = true; }
  if (x && y && !full) { v = DiagonalSpeed(v); }
  float vy = v;
  if (down && g_ladder) { vy = g_ladder_slide ? 1.0f : kClimbDown; }
  else if (down && g_pipe) { vy = top ? 1.0f : kClimbDown; }
  if (x) { p.lx = int16_t(float(x) * v * kStickMax); }
  if (y) { p.ly = int16_t(float(y) * vy * kStickMax); }
  if (View(g_view.load()) != View::Lockpick || menu) {
    if (View(g_view.load()) != View::Lockpick) { g_pick_last = -1; }
    g_stick_keys = (y > 0 ? 1u : 0u) | (y < 0 ? 2u : 0u) | (x < 0 ? 4u : 0u) | (x > 0 ? 8u : 0u);
    return;
  }
  PickStick(p, move);
}

void Refresh();

void Keyboard(Pad& p) {
  std::array<std::vector<int>, kCount> binds;
  {
    std::lock_guard lock(g_bind_lock);
    binds = g_binds;
  }
  bool menu = g_menu;
  if (g_settle) {
    if (AnyKeyDown(binds)) { return; }
    g_settle = false;
  }
  SettleMouse();
  bool no_mouse = menu || g_pointer || g_settle_mouse;
  bool camera = View(g_view.load()) == View::StickyCamera && !menu;
  View view = View(g_view.load());
  bool keypad = view == View::Keypad && !menu;
  bool back = g_opsat || (view == View::BackOut && !menu) || keypad;
  int move[4] = {};
  uint32_t keys = 0;
  for (size_t i = 0; i < kCount; i++) {
    if (kActions[i].button == kHostKey) { continue; }
    if (!ActionHeld(i, binds[i], no_mouse, menu)) {
      g_down_as[i] = 0;
      continue;
    }
    keys |= 1u << i;
    if (kActions[i].event != KeyEvent::None) {
      ApplyEventKey(i, p);
      continue;
    }
    PressTarget(LatchTarget(i, camera, back, menu), p, move);
  }
  if (g_hanging && !menu && move[1] && !move[2] && !move[3]) { p.analog[kAnalogB] = 0xFF; }
  PressMenuKeys(p, menu, back, keypad);
  NoteKeysHeld(keys);
  UpdateStick(p, move, camera, menu, back);
}

// the page's gamepad record, as XInput reports one
bool ReadPad(int slot, XINPUT_GAMEPAD& g) {
  auto* words = browser::input_core::g_gamepad.load(std::memory_order_acquire);
  if (slot != 0 || !words) { return false; }
  for (int attempt = 0; attempt < 3; ++attempt) {
    const uint32_t sequence = (*words)[0].load(std::memory_order_acquire);
    if (sequence & 1) { continue; }
    std::array<uint32_t, 7> sample;
    for (uint32_t n = 0; n < sample.size(); ++n) { sample[n] = (*words)[n + 1].load(std::memory_order_relaxed); }
    if ((*words)[0].load(std::memory_order_acquire) != sequence) { continue; }
    g_touch_pad = sample[0] == 2;
    if (!sample[0]) { return false; }
    const auto axis = [](uint32_t value) { return int16_t(std::clamp(std::bit_cast<int32_t>(value), -32767, 32767)); };
    g = {};
    for (int b = 0; b < kPadControls; b++) {
      if (kPadMasks[b] && (sample[6] >> b & 1)) { g.wButtons |= kPadMasks[b]; }
    }
    g.bLeftTrigger = uint8_t(sample[6] >> 16);
    g.bRightTrigger = uint8_t(sample[6] >> 24);
    g.sThumbLX = axis(sample[1]);
    g.sThumbLY = axis(sample[2]);
    g.sThumbRX = axis(sample[3]);
    g.sThumbRY = axis(sample[4]);
    return true;
  }
  return false;
}

uint32_t PadHeld(const XINPUT_GAMEPAD& g) {
  uint32_t held = uint32_t(g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) << 6 |
                  uint32_t(g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) << 7;
  for (int b = 0; b < kPadButtons; b++) {
    if (kPadMasks[b] && (g.wButtons & kPadMasks[b])) { held |= 1u << b; }
  }
  return held;
}

int PadTarget(int b, bool play) {
  int mapped = g_pad_map[size_t(b)];
  int own = b < kPadControls ? b : -1;
  if (!play || !mapped) { return own; }
  if (mapped < 0) { return -1; }
  return mapped - 1;
}

void Rebind(Pad& p, int port, int slot, WORD buttons) {
  uint8_t raw[kPadButtons];
  for (int b = 0; b < kButtonTarget; b++) { raw[b] = p.analog[b]; }
  for (int b = kButtonTarget; b < kPadControls; b++) { raw[b] = (p.buttons >> (b - kButtonTarget) & 1) ? 0xFF : 0; }
  for (int b = kPadControls; b < kPadButtons; b++) { raw[b] = (buttons & kPadMasks[b]) ? 0xFF : 0; }
  p.buttons = 0;
  std::fill(std::begin(p.analog), std::end(p.analog), uint8_t(0));
  bool held = std::any_of(std::begin(raw), std::end(raw), [](uint8_t v) { return v > XINPUT_GAMEPAD_TRIGGER_THRESHOLD; });
  if (g_pad_settle[size_t(slot)] && held) { return; }
  g_pad_settle[size_t(slot)] = false;
  bool play = !g_menu && !g_title;
  auto& down_as = g_pad_down_as[size_t(port)];
  for (int b = 0; b < kPadButtons; b++) {
    bool down = raw[b] > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
    int now = PadTarget(b, play);
    bool fresh = down && !down_as[size_t(b)];
    if (!down) { down_as[size_t(b)] = 0; }
    else if (fresh) { down_as[size_t(b)] = now + 2; }
    int c = down ? down_as[size_t(b)] - 2 : now;
    if (c < 0) { continue; }
    if (c >= kEventTarget) {
      KeyEvent event = KeyEvent(c - kEventTarget);
      if (fresh && event == KeyEvent::HideHud) { app::g_hide_hud = !app::g_hide_hud; }
      else if (fresh && !g_net && event != KeyEvent::PushToTalk) { g_events |= 1u << int(event); }
      continue;
    }
    if (c < kButtonTarget) { p.analog[c] = std::max(p.analog[c], raw[b]); }
    else if (down) { p.buttons |= uint16_t(1u << (c - kButtonTarget)); }
  }
}

void ApplyPadBinds() {
  for (int b = 0; b < kPadButtons; b++) {
    bool stock = std::ranges::any_of(kPadActions, [b](const PadAction& a) { return a.button == b; });
    g_pad_map[size_t(b)] = stock ? -1 : 0;
  }
  for (size_t i = 0; i < kPadCount; i++) {
    const PadAction& a = kPadActions[i];
    if (g_pad_binds[i] < 0) { continue; }
    int target = a.event != KeyEvent::None ? kEventTarget + int(a.event) : a.button;
    g_pad_map[size_t(g_pad_binds[i])] = target + 1;
  }
}

int PadButton(const std::string& name) {
  for (int b = 0; b < kPadButtons; b++) {
    if (strcasecmp(kPadNames[b], name.c_str()) == 0) { return b; }
  }
  return strcasecmp(name.c_str(), "TOUCHPAD") == 0 ? kPadControls : -1;
}

void Notch(int notch) {
  if (g_map) {
    g_wheel += notch;
  } else if (g_menu || g_list) {
    g_menu_wheel += notch * WHEEL_DELTA;
  } else {
    int s = g_speed + notch;
    View view = View(g_view.load());
    if (view == View::Eev || view == View::StickyCamera) { g_wheel += notch; }
    else if (s >= 0 && s < int(std::size(kSpeeds))) { g_speed = s; }
  }
}

// a wheel notch from the page, as OnRawInput takes one
void WheelNotch(int notch) {
  g_wheel_ms[notch < 0] = NowMs();
  if (!Active()) { return; }
  if (g_map) {
    Notch(notch);
  } else if (g_menu || g_list) {
    g_menu_wheel += notch * WHEEL_DELTA;
  }
  if (g_map || g_menu || g_list) { return; }
  int vk = notch > 0 ? kWheelUp : kWheelDown;
  std::lock_guard lock(g_bind_lock);
  for (size_t i = 0; i < kCount; i++) {
    if (ZoomOf(i) && std::ranges::find(g_binds[i], vk) != g_binds[i].end()) { Notch(ZoomOf(i)); }
  }
}

bool Capturing() { return NowMs() - g_capture_ms <= 250; }

// what the page delivered since the last call: wheel notches, Escape taps, typed characters, activity; and the
// guest's own flags back to the page
std::mutex g_pump_lock;
void Pump() {
  Refresh();
  std::lock_guard lock(g_pump_lock);
  static uint32_t change = 0, up = 0, down = 0, taps = 0, chars = 0;
  static bool hud_held = false;
  auto& raw = Raw();
  if (const uint32_t now = Word(ki::kChange); now != change) {
    change = now;
    g_keyboard_ms = NowMs();
  }
  for (const uint32_t now = Word(ki::kWheelUp); up != now; ++up) { WheelNotch(1); }
  for (const uint32_t now = Word(ki::kWheelDown); down != now; ++down) { WheelNotch(-1); }
  if (const uint32_t now = Word(ki::kStartTaps); now != taps) {
    taps = now;
    if (Active() && !g_menu) { TapButtons(kStart); }
  }
  const uint32_t head = Word(ki::kCharHead);
  if (head - chars > ki::kCharUnits) { chars = head - ki::kCharUnits; }
  for (; chars != head; ++chars) {
    const uint32_t slot = chars % ki::kCharUnits;
    OnChar(Word(ki::kChars + slot / 2) >> ((slot % 2) * 16) & 0xFFFF);
  }
  const bool hud = Active() && !Capturing() && HeldKeys(KeyEvent::HideHud);
  if (hud && !hud_held) { app::g_hide_hud = !app::g_hide_hud; }
  hud_held = hud;
  const uint32_t flags = (g_menu ? ki::kMenuFlag : 0) | (g_pointer ? ki::kPointerFlag : 0) | (g_text ? ki::kTextFlag : 0) |
                         (MapDrag() ? ki::kMapDragFlag : 0) | (Movie() ? ki::kMovieFlag : 0) | (Capturing() ? ki::kCaptureFlag : 0);
  raw[ki::kFlags].store(flags, std::memory_order_release);
}

void PostKeys(size_t action) {
  browser::host_messages::Post(browser::host_messages::kSet, std::string("keys.") + kActions[action].action.id + "=" + Join(g_binds[action]));
}

}  // namespace

bool MapDrag() { return g_map && Active() && Down(VK_RBUTTON); }

void SetBlocked(bool b) {
  if (g_blocked.exchange(b) && !b) {
    for (auto& settle : g_pad_settle) { settle = true; }
  }
  if (b) { ReleaseCursor(); }
}

void StartControllers() {}

uint16_t HeldOnAnyController() {
  XINPUT_GAMEPAD g;
  return ReadPad(0, g) ? g.wButtons : 0;
}

void Init(void*) {}
void OnRawInput(void*) {}
void OnPress() {}
void OnClick() {}

void SkipPromptShown() { g_skip_ms = NowMs(); }

void ReleaseCursor() { g_settle = true; }

// a sensitivity per view, as versus has one: the sniper's in the EEV and every scope, the weapon's while aiming
bool TakeMouse(float& dx, float& dy) {
  Pump();
  auto& raw = Raw();
  const int32_t x = std::bit_cast<int32_t>(raw[ki::kMouseX].exchange(0, std::memory_order_acq_rel));
  const int32_t y = std::bit_cast<int32_t>(raw[ki::kMouseY].exchange(0, std::memory_order_acq_rel));
  if ((!x && !y) || !Active() || (Pointing() && !MapDrag())) { return false; }
  const app::Settings& s = app::Get();
  const float sensitivity = View(g_view.load()) == View::Eev ? s.sniper_sensitivity :
                            g_targeting ? s.weapon_sensitivity : s.free_look_sensitivity;
  dx = float(x) / 1000.0f * sensitivity;
  dy = float(s.invert_mouse ? -y : y) / 1000.0f * sensitivity;
  return true;
}

void SetTitleScreen(bool title) { g_title = title; }
void SetPauseMenu(bool pause) { g_pause_menu = pause; }
void SetOpsat(bool opsat) { g_opsat = opsat; }
void SetMap(bool map) { g_map = map; }
void SetQuickInventory(bool open) { g_quick = open; }

void SetTextEntry(bool on) {
  if (g_text.exchange(on) == on) { return; }
  std::lock_guard lock(g_chars_lock);
  g_chars.clear();
}

void OnChar(unsigned c) {
  if (!g_text || !Active() || c > 0xFFFF) { return; }
  g_keyboard_ms = NowMs();
  std::lock_guard lock(g_chars_lock);
  if (g_chars.size() < 64) { g_chars += char16_t(c); }
}

std::u16string TakeChars() {
  Pump();
  std::lock_guard lock(g_chars_lock);
  return std::exchange(g_chars, {});
}

void SetMenu(bool menu) {
  if (g_menu.exchange(menu) != menu) {
    g_menu_wheel = 0;
    g_settle_mouse = true;
  }
}

bool InMenu() { return g_menu; }

void SetPointer(bool on) {
  if (g_pointer.exchange(on) != on) { g_settle_mouse = true; }
}

bool Pointer() { return g_pointer; }

void MovieFrame() { g_movie_ms = NowMs(); }

bool Movie() { return NowMs() - g_movie_ms < kMovieMs; }

bool KeyboardActive() { return KeyboardPort() >= 0 && !g_touch_pad && g_keyboard_ms >= g_pad_ms; }

void SetView(View view, float zoom_speed) {
  if (g_view.exchange(int(view)) != int(view)) { g_wheel = 0; }
  g_zoom_speed = zoom_speed;
}

int TakeWheel() {
  Pump();
  return g_wheel.exchange(0);
}

void SetInteractionList(bool focused) {
  if (g_list.exchange(focused) != focused) { g_menu_wheel = 0; }
}

int TakeKeypadButton() {
  static constexpr std::pair<int, int> kKeys[] = {
    {VK_NUMPAD1, 0}, {VK_NUMPAD2, 1}, {VK_NUMPAD3, 2}, {VK_NUMPAD4, 3}, {VK_NUMPAD5, 4}, {VK_NUMPAD6, 5},
    {VK_NUMPAD7, 6}, {VK_NUMPAD8, 7}, {VK_NUMPAD9, 8}, {VK_MULTIPLY, 9}, {VK_BACK, 9}, {VK_DELETE, 9},
    {VK_NUMPAD0, 10}, {VK_DECIMAL, 11},
  };
  static uint32_t held = 0;
  uint32_t now = 0;
  for (size_t i = 0; i < std::size(kKeys); i++) { now |= uint32_t(Active() && Down(kKeys[i].first)) << i; }
  uint32_t fresh = now & ~std::exchange(held, now);
  return fresh ? kKeys[std::countr_zero(fresh)].second : -1;
}

int TakeMenuWheel() {
  Pump();
  int notches = g_menu_wheel / WHEEL_DELTA;
  g_menu_wheel -= notches * WHEEL_DELTA;
  return notches;
}

uint32_t StickKeys() { return g_stick_keys; }

int KeyboardPort() {
  const app::Settings& s = app::Get();
  for (int port = 0; port < 2; port++) {
    if (s.players[port].keyboard) { return port; }
  }
  return -1;
}

bool PadConnected(int slot) {
  XINPUT_GAMEPAD g;
  return ReadPad(slot, g);
}

bool NetSession() { return g_net; }
void SetNetSession(bool net) { g_net = net; }
void SetTrainingVideo(bool on) { g_video = on; }
void SetInteractReloads(bool on) { g_interact_reloads = on; }
void SetTargeting(bool on) { g_targeting = on; }
void SetCarrying(bool on) { g_carrying = on; }
void SetHanging(bool on) { g_hanging = on; }
void SetLadder(bool on, bool stepping) {
  g_ladder = on;
  g_ladder_step = on && stepping;
}
void SetPipe(bool on) { g_pipe = on; }

bool TakeEvent(KeyEvent event) { return g_events.fetch_and(~(1u << int(event))) >> int(event) & 1; }

bool EventKeyHeld(KeyEvent event) {
  bool pad = std::any_of(g_pad_events_held.begin(), g_pad_events_held.end(),
                         [&](const std::atomic<uint32_t>& h) { return h >> int(event) & 1; });
  return Active() && (HeldKeys(event) || pad);
}

void TapButtons(uint16_t buttons, uint64_t ms) {
  g_tap = buttons;
  g_tap_until = NowMs() + ms;
}

namespace {

struct PressWatch {
  std::array<bool, 256> held{};
  uint64_t pad_held = 0;
  bool started = false;
};

uint64_t Pads() {
  XINPUT_GAMEPAD g;
  if (!ReadPad(0, g)) { return 0; }
  uint32_t left = uint32_t(g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
  uint32_t right = uint32_t(g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
  return g.wButtons | left << 10 | right << 11;
}

bool AnswerKey(int vk) {
  switch (vk) {
    case VK_F3: case VK_SNAPSHOT: case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL: case VK_PAUSE:
    case 0x07: case 0x0A: case 0x0B: case 0x0E: case 0x0F: case 0x5E: case 0xE0: case 0xE8: case 0xFF:
      return false;
  }
  bool media_or_reserved = (vk >= VK_BROWSER_BACK && vk <= VK_LAUNCH_APP2) || (vk >= 0x3A && vk <= 0x40) ||
                           (vk >= 0x88 && vk <= 0x9F) || (vk >= 0xC1 && vk <= 0xDA);
  if (media_or_reserved) { return false; }
  if (ConsoleKey(vk) || Bound(KeyEvent::HideHud, vk) || Bound(KeyEvent::SettingsPanel, vk)) { return false; }
  return KeyBit(vk);
}

int NewPress(PressWatch& w) {
  bool live = w.started && Active();
  int pressed = 0;
  uint64_t pad = Pads();
  uint64_t fresh = pad & ~w.pad_held;
  if (live && fresh) { pressed = 0x100 + std::countr_zero(fresh); }
  w.pad_held = pad;
  for (int vk = 1; vk < 256; vk++) {
    bool d = AnswerKey(vk);
    if (live && d && !w.held[vk] && !pressed) { pressed = vk; }
    w.held[vk] = d;
  }
  w.started = Active();
  return pressed;
}

// the settings revision changed: the binds the page sent replace ours
void Refresh() {
  if (g_loaded.load(std::memory_order_acquire) != app::SettingsRevision()) { LoadBindings(); }
}

}  // namespace

void WaitForPress() {
  SetMenu(false);
  PressWatch w;
  NewPress(w);
  int pressed;
  while (!(pressed = NewPress(w))) {
    Pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  bool pad = pressed >= 0x100;
  LOGI("input: the wait ended by %s %X", pad ? "pad button" : "key", pad ? pressed - 0x100 : pressed);
}

bool PressReleased() {
  static PressWatch w;
  static int pressed = 0;
  if (int p = NewPress(w); p && !pressed) { pressed = p; }
  if (!pressed) { return false; }
  bool down = pressed >= 0x100 ? (w.pad_held >> (pressed - 0x100) & 1) != 0 : w.held[size_t(pressed)];
  if (down) { return false; }
  pressed = 0;
  return true;
}

bool SkipKeyReleased() {
  constexpr int kKeys[] = {VK_SHIFT, VK_RETURN, VK_SPACE, VK_ESCAPE};
  constexpr uint64_t kGapMs = 500;
  static uint32_t up = 0;
  static uint32_t down = 0;
  static uint64_t last = 0;
  uint64_t now = NowMs();
  if (now - last > kGapMs || !Active()) {
    up = 0;
    down = 0;
  }
  last = now;
  if (!Active()) { return false; }
  bool released = false;
  for (size_t i = 0; i < std::size(kKeys); i++) {
    uint32_t bit = 1u << i;
    if (Down(kKeys[i])) {
      if (up & bit) { down |= bit; }
    } else {
      up |= bit;
      released |= (down & bit) != 0;
      down &= ~bit;
    }
  }
  return released;
}

// the cursor over the frame the page shows: the frame is the whole client area
bool Cursor(int& x, int& y, int& w, int& h) {
  Pump();
  if (!Active() || !Word(ki::kCursorW) || !Word(ki::kCursorH)) { return false; }
  x = std::bit_cast<int32_t>(Word(ki::kCursorX));
  y = std::bit_cast<int32_t>(Word(ki::kCursorY));
  w = int(Word(ki::kCursorW));
  h = int(Word(ki::kCursorH));
  return true;
}

uint32_t Clicks() {
  Pump();
  return Word(ki::kClicks);
}

bool Pressing() { return Word(ki::kPressing) != 0; }

const Action* Actions(size_t* count) {
  static const auto list = [] {
    std::array<Action, kCount> a{};
    for (size_t i = 0; i < kCount; i++) { a[i] = kActions[i].action; }
    return a;
  }();
  *count = kCount;
  return list.data();
}

std::string Binding(size_t action) {
  Refresh();
  std::lock_guard lock(g_bind_lock);
  return action < kCount ? Join(g_binds[action]) : std::string();
}

namespace {
bool LockedOut(size_t action) { return kActions[action].event == KeyEvent::SettingsPanel; }
}

// the page keeps the bindings: a change goes to it, and comes back with the next settings revision
void Bind(size_t action, int vk, int slot) {
  Refresh();
  if (action >= kCount || slot < 0 || slot > 1 || (vk && !NameOf(vk))) { return; }
  if (kActions[action].button == kHostKey && kActions[action].event != KeyEvent::PushToTalk && Mouse(vk)) { return; }
  std::lock_guard lock(g_bind_lock);
  std::vector<int>& keys = g_binds[action];
  int slots[2] = {keys.size() > 0 ? keys[0] : 0, keys.size() > 1 ? keys[1] : 0};
  if (vk && slots[1 - slot] == vk) { slots[1 - slot] = 0; }
  slots[slot] = vk;
  if (!slots[0] && !slots[1] && LockedOut(action)) { return; }
  keys.clear();
  for (int k : slots) {
    if (k) { keys.push_back(k); }
  }
  PostKeys(action);
}

void Unbind(size_t action) {
  Refresh();
  if (action >= kCount || LockedOut(action)) { return; }
  std::lock_guard lock(g_bind_lock);
  g_binds[action].clear();
  PostKeys(action);
}

bool TakeDeletePress() {
  static bool held = false;
  bool down = Down(VK_DELETE);
  bool fresh = down && !held;
  held = down;
  return fresh && Active();
}

bool ConsoleKey(int vk) {
  std::lock_guard lock(g_bind_lock);
  if (kConsoleAction >= kCount) { return false; }
  const auto& b = g_binds[kConsoleAction];
  return std::find(b.begin(), b.end(), vk) != b.end();
}

bool HostKey(KeyEvent event, int vk) { return !Capturing() && Bound(event, vk); }

// the US layout's characters, as the page's key codes are
bool ConsoleChar(unsigned c) {
  constexpr std::pair<int, char> kChars[] = {{VK_OEM_3, '`'}, {VK_OEM_MINUS, '-'}, {VK_OEM_PLUS, '='}, {VK_OEM_4, '['},
    {VK_OEM_6, ']'}, {VK_OEM_5, '\\'}, {VK_OEM_1, ';'}, {VK_OEM_7, '\''}, {VK_OEM_COMMA, ','}, {VK_OEM_PERIOD, '.'},
    {VK_OEM_2, '/'}};
  std::lock_guard lock(g_bind_lock);
  if (kConsoleAction >= kCount) { return false; }
  for (int vk : g_binds[kConsoleAction]) {
    unsigned k = (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') ? unsigned(vk) : 0;
    for (auto [key, character] : kChars) {
      if (key == vk) { k = unsigned(uint8_t(character)); }
    }
    if (k && towupper(wint_t(c)) == towupper(wint_t(k))) { return true; }
  }
  return false;
}

static int SavedPadButton(const std::map<std::string, std::string>& pads, size_t i) {
  auto it = pads.find(kPadActions[i].id);
  if (it == pads.end()) { return kPadActions[i].button; }
  if (it->second.empty()) { return -1; }
  return PadButton(it->second);
}

void ResetBindings() {
  for (size_t i = 0; i < kCount; i++) {
    browser::host_messages::Post(browser::host_messages::kUnset, std::string("keys.") + kActions[i].action.id);
  }
}

void LoadBindings() {
  const uint32_t revision = app::SettingsRevision();
  std::lock_guard lock(g_bind_lock);
  const auto& keys = app::Get().keys;
  for (size_t i = 0; i < kCount; i++) {
    auto it = keys.find(kActions[i].action.id);
    g_binds[i] = Parse(it != keys.end() ? it->second : kActions[i].action.defaults);
  }
  const auto& pads = app::Get().pad_buttons;
  std::array<bool, kPadButtons> taken{};
  std::vector<size_t> later;
  for (size_t i = 0; i < kPadCount; i++) {
    int b = SavedPadButton(pads, i);
    if (b >= 0 && taken[size_t(b)]) {
      later.push_back(i);
      continue;
    }
    g_pad_binds[i] = b;
    if (b >= 0) { taken[size_t(b)] = true; }
  }
  for (size_t i : later) {
    int b = kPadActions[i].button;
    g_pad_binds[i] = b >= 0 && !taken[size_t(b)] ? b : -1;
    if (g_pad_binds[i] >= 0) { taken[size_t(b)] = true; }
  }
  ApplyPadBinds();
  g_loaded.store(revision, std::memory_order_release);
}

const Action* PadActions(size_t* count) {
  static const auto list = [] {
    std::array<Action, kPadCount> a{};
    for (size_t i = 0; i < kPadCount; i++) {
      int b = kPadActions[i].button;
      a[i] = {kPadActions[i].id, kPadActions[i].label, b >= 0 ? kPadNames[b] : ""};
    }
    return a;
  }();
  *count = kPadCount;
  return list.data();
}

std::string PadBinding(size_t action) {
  Refresh();
  std::lock_guard lock(g_bind_lock);
  return action < kPadCount && g_pad_binds[action] >= 0 ? kPadNames[g_pad_binds[action]] : "";
}

void BindPad(size_t action, int button) {
  Refresh();
  bool event = action < kPadCount && kPadActions[action].event != KeyEvent::None;
  if (action >= kPadCount || button < -1 || button >= kPadButtons || (button < 0 && !event)) { return; }
  std::lock_guard lock(g_bind_lock);
  int old = g_pad_binds[action];
  bool off = button < 0 || (old == button && event);
  for (int& b : g_pad_binds) {
    if (button >= 0 && b == button) { b = old; }
  }
  g_pad_binds[action] = off ? -1 : button;
  for (size_t i = 0; i < kPadCount; i++) {
    int b = g_pad_binds[i];
    const std::string path = std::string("padButtons.") + kPadActions[i].id;
    if (b == kPadActions[i].button) { browser::host_messages::Post(browser::host_messages::kUnset, path); }
    else { browser::host_messages::Post(browser::host_messages::kSet, path + "=" + (b >= 0 ? kPadNames[b] : "")); }
  }
  ApplyPadBinds();
}

void ResetPadBindings() {
  for (const PadAction& a : kPadActions) {
    browser::host_messages::Post(browser::host_messages::kUnset, std::string("padButtons.") + a.id);
  }
}

int PressedPadButton() {
  XINPUT_GAMEPAD g;
  if (uint32_t held = ReadPad(0, g) ? PadHeld(g) : 0) { return std::countr_zero(held); }
  return -1;
}

int PressedKey() {
  g_capture_ms = NowMs();
  Pump();
  for (const Name& n : kNames) {
    if (Down(n.vk)) { return n.vk; }
  }
  return 0;
}

static uint32_t PadEventsHeld(uint32_t down) {
  uint32_t events = 0;
  for (int b = 0; b < kPadButtons; b++) {
    int m = g_pad_map[size_t(b)];
    if (down >> b & 1 && m > kEventTarget) { events |= 1u << (m - 1 - kEventTarget); }
  }
  return events;
}

static void FillFromXInput(const XINPUT_GAMEPAD& g, Pad& p) {
  auto b = [&](WORD m) { return (g.wButtons & m) != 0; };
  p.buttons = uint16_t((b(XINPUT_GAMEPAD_DPAD_UP) << 0) | (b(XINPUT_GAMEPAD_DPAD_DOWN) << 1) |
                       (b(XINPUT_GAMEPAD_DPAD_LEFT) << 2) | (b(XINPUT_GAMEPAD_DPAD_RIGHT) << 3) |
                       (b(XINPUT_GAMEPAD_START) << 4) | (b(XINPUT_GAMEPAD_BACK) << 5) |
                       (b(XINPUT_GAMEPAD_LEFT_THUMB) << 6) | (b(XINPUT_GAMEPAD_RIGHT_THUMB) << 7));
  auto a = [&](WORD m) { return uint8_t(b(m) ? 0xFF : 0); };
  p.analog[0] = a(XINPUT_GAMEPAD_A);
  p.analog[1] = a(XINPUT_GAMEPAD_B);
  p.analog[2] = a(XINPUT_GAMEPAD_X);
  p.analog[3] = a(XINPUT_GAMEPAD_Y);
  p.analog[4] = a(XINPUT_GAMEPAD_RIGHT_SHOULDER);
  p.analog[5] = a(XINPUT_GAMEPAD_LEFT_SHOULDER);
  p.analog[6] = g.bLeftTrigger;
  p.analog[7] = g.bRightTrigger;
  p.lx = g.sThumbLX;
  p.ly = g.sThumbLY;
  p.rx = g.sThumbRX;
  p.ry = g.sThumbRY;
}

constexpr int kStickActive = 8000;

static void NotePadHeld(const Pad& p) {
  uint32_t held = p.buttons;
  for (int i = 0; i < 8; i++) { held |= uint32_t(p.analog[i] > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) << (8 + i); }
  int bit = 16;
  for (int16_t axis : {p.lx, p.ly, p.rx, p.ry}) { held |= uint32_t(std::abs(int(axis)) > kStickActive) << bit++; }
  if (held & ~g_pad_held.exchange(held)) { g_pad_ms = NowMs(); }
}

Pad Poll(int port) {
  Pump();
  Pad p;
  int slot = port >= 0 && port < 2 ? app::Get().players[port].pad : -1;
  bool keyboard = port == KeyboardPort();
  XINPUT_GAMEPAD g;
  bool pad = ReadPad(slot, g);
  uint32_t down = pad ? PadHeld(g) : 0;
  uint32_t events = PadEventsHeld(down);
  if (port >= 0 && port < 4) { g_pad_events_held[size_t(port)] = g_blocked ? 0 : events; }
  p.connected = pad || keyboard;
  if (!p.connected || g_blocked || port < 0 || port >= 4) { return p; }
  if (pad) {
    FillFromXInput(g, p);
    Rebind(p, port, slot, g.wButtons);
    if (keyboard) { NotePadHeld(p); }
  }
  if (!keyboard) { return p; }
  g_stick_keys = 0;
  if (Active()) { Keyboard(p); }
  if (NowMs() < g_tap_until) { p.buttons |= g_tap; }
  return p;
}

void Rumble(int, uint16_t, uint16_t) {}

}  // namespace input
