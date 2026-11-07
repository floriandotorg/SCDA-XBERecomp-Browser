// app::rows for the browser: the game's own options pages (options_menu.cpp) show the page's settings.mjs values,
// which reach the guest as host.<key> lines, and send changes back through the host message ring; the page applies
// and saves them and publishes the next settings revision. A change shows at once from a pending copy until then.
#include "app/app.h"
#include "browser/host-messages.h"
#include "browser/host-settings.h"
#include "browser/input-core.h"
#include "browser/keyboard-input.h"
#include "core/gyro.h"
#include "core/input.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace {

using app::rows::Kind;
using app::rows::Row;

struct Choice {
  const char* value;
  const char* label;
};
// a value row: a list of choices (an ON / OFF row has none and flips) or a range that steps
struct Setting {
  int id;
  const char* key;
  const char* initial;
  std::vector<Choice> choices;
  bool toggle = false;
  double low = 0, high = 0, increment = 0;
  const char* format = "%g";
};

const std::vector<Setting>& Settings() {
  static const std::vector<Setting> list = {
    {0, "scaling", "fit", {{"fit", "FIT TO WINDOW"}, {"integer", "PIXEL PERFECT"}, {"stretch", "STRETCH"}}},
    {1, "filter", "smooth", {{"smooth", "SMOOTH"}, {"sharp", "SHARP"}}},
    {2, "showFps", "false", {}, true},
    {3, "volume", "100", {}, false, 0, 100, 5, "%.0f%%"},
    {4, "skipIntros", "false", {}, true},
    {5, "pauseOnFocusLoss", "true", {}, true},
    {6, "sensitivity", "1", {}, false, 0.1, 5, 0.1, "%.1f"},
    {7, "weaponSensitivity", "1", {}, false, 0.1, 5, 0.1, "%.1f"},
    {8, "sniperSensitivity", "1", {}, false, 0.1, 5, 0.1, "%.1f"},
    {9, "invertY", "false", {}, true},
    {10, "aspect", "auto", {{"auto", "AUTO"}, {"4:3", "4:3"}, {"16:10", "16:10"}, {"16:9", "16:9"}, {"21:9", "21:9"}}},
    {11, "fov", "75", {}, false, 45, 140, 1, "%.0f"},
    {12, "fmvScale", "fill", {{"fill", "FILL"}, {"4:3", "4:3"}}},
    {13, "resolution", "console", {{"console", "CONSOLE"}, {"window", "MATCH WINDOW"}, {"dynamic", "DYNAMIC"}}},
    {14, "resolutionScale", "100", {}, false, 50, 200, 5, "%.0f%%"},
    {15, "brightness", "50", {}, false, 0, 100, 1, "%.0f"},
    {16, "contrast", "50", {}, false, 0, 100, 1, "%.0f"},
    {17, "frameCap", "0", {{"0", "DISPLAY RATE"}, {"30", "30"}, {"60", "60"}}},
    {18, "anisotropy", "0", {{"0", "OFF"}, {"2", "2X"}, {"4", "4X"}, {"8", "8X"}, {"16", "16X"}}},
    {19, "language", "auto", {{"auto", "AUTOMATIC"}, {"int", "ENGLISH"}, {"fra", "FRANCAIS"}, {"deu", "DEUTSCH"}, {"ita", "ITALIANO"}, {"esp", "ESPANOL"}}},
    {20, "audioOutput", "stereo", {{"stereo", "2.0 STEREO"}, {"mono", "MONO"}}},
    {21, "msaa", "0", {{"0", "OFF"}, {"4", "4X"}}},
    {22, "smaa", "false", {}, true},
    {23, "shadowResolution", "512", {{"512", "512"}, {"1024", "1024"}, {"2048", "2048"}, {"4096", "4096"}}},
    {24, "shadowFilter", "console", {{"console", "CONSOLE"}, {"soft", "SM3 SOFT"}}},
    {25, "shadowDistance", "1", {{"1", "1X"}, {"2", "2X"}, {"4", "4X"}, {"8", "8X"}}},
    {26, "shaderModel", "1.1", {{"1.1", "1.1"}, {"3.0", "3.0"}}},
    {27, "hdrRendering", "false", {}, true},
    {28, "toneMapping", "false", {}, true},
    {29, "hdrOutput", "true", {}, true},
    {30, "ambientOcclusion", "off", {{"off", "OFF"}, {"ssao", "SSAO"}, {"gtao", "GTAO"}}},
    {31, "colourBlind", "off", {{"off", "OFF"}, {"protanopia", "PROTANOPIA"}, {"deuteranopia", "DEUTERANOPIA"}, {"tritanopia", "TRITANOPIA"}}},
    {32, "verticalSensitivity", "1", {}, false, 0.1, 5, 0.1, "%.1f"},
    {33, "invertX", "false", {}, true},
    {34, "padSensitivity", "1", {}, false, 0.1, 4, 0.1, "%.1f"},
    {35, "padVerticalSensitivity", "1", {}, false, 0.1, 4, 0.1, "%.1f"},
    {36, "padAimSensitivity", "1", {}, false, 0.1, 4, 0.1, "%.1f"},
    {37, "padAimVerticalSensitivity", "1", {}, false, 0.1, 4, 0.1, "%.1f"},
    {38, "padInvertX", "false", {}, true},
    {39, "padInvertY", "false", {}, true},
    {40, "padDeadZone", "0", {{"0", "0.3 (DEFAULT)"}, {"0.1", "0.1"}, {"0.2", "0.2"}, {"0.3", "0.3"}, {"0.4", "0.4"}, {"0.5", "0.5"},
                              {"0.6", "0.6"}, {"0.7", "0.7"}, {"0.8", "0.8"}, {"0.9", "0.9"}, {"1", "1.0"}}},
    {41, "vibration", "100", {}, false, 0, 200, 10, "%.0f%%"},
    {42, "gyroAiming", "false", {}, true},
    {43, "gyroSensitivity", "2", {}, false, 0.1, 5, 0.1, "%.1f"},
    {44, "gyroAxis", "yaw", {{"yaw", "YAW"}, {"roll", "ROLL"}, {"world", "WORLD"}}},
  };
  return list;
}

const Setting* Find(int id) {
  for (const Setting& setting : Settings()) {
    if (setting.id == id) { return &setting; }
  }
  return nullptr;
}

// CALIBRATE GYRO: a step starts it, no setting
constexpr int kGyroCalibrate = 45;
// the gyro rows, shown where the page reads a motion sensor (gamepad-input.mjs)
bool GyroRow(int id) { return id >= 42 && id <= kGyroCalibrate; }
bool MotionSensor() {
  const auto* words = browser::input_core::Gamepad();
  return words && (*words)[browser::input_core::kMotionFlags].load() & browser::input_core::kMotionSensor;
}

// PRESET: no setting of its own, it reads and sets the graphics rows as settings.mjs GRAPHICS_PRESETS
constexpr int kGraphicsPreset = 46;
struct Preset {
  const char* label;
  std::vector<std::pair<int, const char*>> values;
};
const std::vector<Preset>& Presets() {
  static const std::vector<Preset> list = {
    {"ORIGINAL", {{13, "console"}, {18, "0"}, {21, "0"}, {22, "false"}, {23, "512"}, {24, "console"}, {25, "1"}, {26, "1.1"},
                  {27, "false"}, {28, "false"}, {30, "off"}}},
    {"HIGH QUALITY", {{13, "window"}, {18, "16"}, {21, "4"}, {22, "false"}, {23, "2048"}, {24, "soft"}, {25, "4"}, {26, "3.0"},
                      {27, "true"}, {28, "true"}, {30, "gtao"}}},
  };
  return list;
}

constexpr Row kSettingsRows[] = {
  {Kind::Heading, -1, "DISPLAY"},
  {Kind::Value, 0, "PICTURE"},
  {Kind::Value, 10, "ASPECT RATIO"},
  {Kind::Value, 13, "RENDER RESOLUTION"},
  {Kind::Value, 14, "RESOLUTION SCALE"},
  {Kind::Value, 1, "UPSCALING"},
  {Kind::Value, 11, "FIELD OF VIEW"},
  {Kind::Value, 12, "FMV SCALE"},
  {Kind::Value, 15, "BRIGHTNESS"},
  {Kind::Value, 16, "CONTRAST"},
  {Kind::Value, 17, "MAX FRAME RATE"},
  {Kind::Value, 31, "COLOUR BLIND MODE"},
  {Kind::Heading, -1, "GRAPHICS"},
  {Kind::Value, kGraphicsPreset, "PRESET"},
  {Kind::Value, 2, "FPS COUNTER"},
  {Kind::Value, 18, "ANISOTROPIC FILTERING"},
  {Kind::Value, 21, "MSAA"},
  {Kind::Value, 22, "SMAA"},
  {Kind::Value, 23, "SHADOW RESOLUTION"},
  {Kind::Value, 24, "SHADOW FILTER"},
  {Kind::Value, 25, "SHADOW DISTANCE"},
  {Kind::Value, 26, "SHADER MODEL"},
  {Kind::Value, 27, "HDR RENDERING"},
  {Kind::Value, 28, "TONE MAPPING"},
  {Kind::Value, 29, "HDR OUTPUT"},
  {Kind::Value, 30, "AMBIENT OCCLUSION"},
  {Kind::Heading, -1, "GENERAL"},
  {Kind::Value, 3, "MASTER VOLUME"},
  {Kind::Value, 4, "SKIP INTROS"},
  {Kind::Value, 5, "PAUSE ON FOCUS LOSS"},
  {Kind::Value, 19, "LANGUAGE"},
  {Kind::Value, 20, "AUDIO OUTPUT"},
};

// the actions the browser has no use for: push to talk waits for voice
bool HiddenAction(std::string_view id) { return id == "push_to_talk"; }

const std::vector<Row>& InputRows() {
  static const std::vector<Row> list = [] {
    std::vector<Row> out = {{Kind::Heading, -1, "MOUSE"}, {Kind::Value, 6, "FREE LOOK SENSITIVITY"},
                            {Kind::Value, 7, "WEAPON SENSITIVITY"}, {Kind::Value, 8, "SNIPER SENSITIVITY"},
                            {Kind::Value, 32, "VERTICAL SENSITIVITY"}, {Kind::Value, 33, "INVERT X AXIS"},
                            {Kind::Value, 9, "INVERT Y AXIS"}, {Kind::Heading, -1, "KEYBOARD"}};
    size_t count = 0;
    const input::Action* actions = input::Actions(&count);
    for (size_t i = 0; i < count; i++) {
      if (!HiddenAction(actions[i].id)) { out.push_back({Kind::Bind, int(i), actions[i].label}); }
    }
    return out;
  }();
  return list;
}

// native's controller page: its sections are GENERAL, GYRO and BUTTONS (options_menu.cpp)
const std::vector<Row>& PadRows() {
  static const std::vector<Row> list = [] {
    std::vector<Row> out = {{Kind::Heading, -1, "GENERAL"},
                            {Kind::Value, 34, "HORIZONTAL SENSITIVITY"},
                            {Kind::Value, 35, "VERTICAL SENSITIVITY"},
                            {Kind::Value, 36, "AIM HORIZONTAL SENSITIVITY"},
                            {Kind::Value, 37, "AIM VERTICAL SENSITIVITY"},
                            {Kind::Value, 38, "INVERT X AXIS"},
                            {Kind::Value, 39, "INVERT Y AXIS"},
                            {Kind::Value, 40, "DEADZONE"},
                            {Kind::Value, 41, "VIBRATION STRENGTH"},
                            {Kind::Heading, -1, "GYRO"},
                            {Kind::Value, 42, "GYRO AIMING"},
                            {Kind::Value, 43, "GYRO SENSITIVITY"},
                            {Kind::Value, 44, "GYRO AXIS"},
                            {Kind::Value, kGyroCalibrate, "CALIBRATE GYRO"},
                            {Kind::Heading, -1, "BUTTONS"}};
    size_t count = 0;
    const input::Action* actions = input::PadActions(&count);
    for (size_t i = 0; i < count; i++) {
      if (!HiddenAction(actions[i].id)) { out.push_back({Kind::PadBind, int(i), actions[i].label}); }
    }
    return out;
  }();
  return list;
}

std::mutex g_lock;
// sent, not yet back with a settings revision
std::map<std::string, std::pair<std::string, uint32_t>> g_pending;

std::string Current(const Setting& setting) {
  std::lock_guard lock(g_lock);
  const auto pending = g_pending.find(setting.key);
  if (pending != g_pending.end() && pending->second.second == app::SettingsRevision()) { return pending->second.first; }
  g_pending.erase(setting.key);
  std::string value = browser::host_settings::HostValue(setting.key);
  return value.empty() ? setting.initial : value;
}

void Send(const Setting& setting, const std::string& value) {
  browser::host_messages::Post(browser::host_messages::kSet, std::string(setting.key) + "=" + value);
  std::lock_guard lock(g_lock);
  g_pending[setting.key] = {value, app::SettingsRevision()};
}

std::string Number(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.4g", value);
  return text;
}

int ChoiceIndex(const Setting& setting, const std::string& value) {
  for (size_t i = 0; i < setting.choices.size(); i++) {
    if (value == setting.choices[i].value) { return int(i); }
  }
  return 0;
}

std::string Text(const Setting& setting, const std::string& value) {
  if (setting.toggle) { return value == "true" ? "ON" : "OFF"; }
  if (!setting.choices.empty()) { return setting.choices[size_t(ChoiceIndex(setting, value))].label; }
  char text[32];
  std::snprintf(text, sizeof(text), setting.format, std::atof(value.c_str()));
  return text;
}

void StepSetting(const Setting& setting, int step) {
  const std::string value = Current(setting);
  if (setting.toggle) {
    Send(setting, value == "true" ? "false" : "true");
  } else if (!setting.choices.empty()) {
    const int count = int(setting.choices.size());
    Send(setting, setting.choices[size_t(((ChoiceIndex(setting, value) + step) % count + count) % count)].value);
  } else {
    const double next = std::clamp(std::round(std::atof(value.c_str()) / setting.increment + step) * setting.increment,
                                   setting.low, setting.high);
    Send(setting, Number(next));
  }
}

// the preset the graphics rows match, -1 when they match none
int PresetIndex() {
  const std::vector<Preset>& presets = Presets();
  for (size_t n = 0; n < presets.size(); ++n) {
    const bool matches = std::all_of(presets[n].values.begin(), presets[n].values.end(),
                                     [](const auto& value) { return Current(*Find(value.first)) == value.second; });
    if (matches) { return int(n); }
  }
  return -1;
}

void ApplyPreset(size_t index) {
  for (const auto& [id, value] : Presets()[index].values) {
    const Setting& setting = *Find(id);
    if (Current(setting) != value) { Send(setting, value); }
  }
}

// native's menu.cpp: the HDR rows only act at shader model 3.0, HDR output only on an HDR display
bool ShaderModel3() {
  const Setting* model = Find(26);
  return Current(*model) == "3.0";
}
bool HdrDisplay() {
  namespace hs = browser::host_settings;
  return (hs::Shared().words[hs::kFlagsWord].load() >> hs::kHdrDisplayBit) & 1;
}
bool Usable(int id) {
  if (id == 27 || id == 28) { return ShaderModel3(); }
  if (id == 29) { return HdrDisplay(); }
  return true;
}

}  // namespace

namespace app {

// the host panel or the console is open over the game (keyboard-input.mjs)
bool MenuOpen() { return browser::keyboard_input::Shared()[browser::keyboard_input::kHostPanel].load() != 0; }
bool ConsoleOpen() { return browser::keyboard_input::Shared()[browser::keyboard_input::kHostConsole].load() != 0; }

namespace rows {

const Row* Page(int page, size_t* count) {
  if (page == 1 || page == 2) {
    const std::vector<Row>& list = page == 1 ? InputRows() : PadRows();
    *count = list.size();
    return list.data();
  }
  *count = std::size(kSettingsRows);
  return kSettingsRows;
}

std::string Value(const Row& row) {
  if (row.kind == Kind::Bind || row.kind == Kind::PadBind) {
    std::string keys = row.kind == Kind::Bind ? input::Binding(size_t(row.id)) : input::PadBinding(size_t(row.id));
    if (keys.empty()) { return "UNBOUND"; }
    for (char& c : keys) { c = c == ',' ? '/' : char(std::toupper(uint8_t(c))); }
    return keys;
  }
  if (row.kind == Kind::Value && row.id == kGyroCalibrate) { return gyro::Calibrating() ? "KEEP STILL" : "START"; }
  if (row.kind == Kind::Value && row.id == kGraphicsPreset) {
    const int index = PresetIndex();
    return index < 0 ? "CUSTOM" : Presets()[size_t(index)].label;
  }
  const Setting* setting = row.kind == Kind::Value ? Find(row.id) : nullptr;
  if (setting && !Usable(row.id)) { return row.id == 29 ? "NOT SUPPORTED" : "OFF"; }
  return setting ? Text(*setting, Current(*setting)) : std::string();
}

void Step(const Row& row, int step) {
  if (row.kind == Kind::Value && row.id == kGyroCalibrate) {
    if (step) { gyro::StartCalibration(); }
    return;
  }
  if (row.kind == Kind::Value && row.id == kGraphicsPreset) {
    if (!step) { return; }
    const int count = int(Presets().size()), index = PresetIndex();
    ApplyPreset(size_t(index < 0 ? 0 : ((index + step) % count + count) % count));
    return;
  }
  if (const Setting* setting = row.kind == Kind::Value && step && Usable(row.id) ? Find(row.id) : nullptr) { StepSetting(*setting, step); }
}

void Defaults(int page) {
  size_t count = 0;
  const Row* list = Page(page, &count);
  for (size_t i = 0; i < count; i++) {
    const Setting* setting = list[i].kind == Kind::Value ? Find(list[i].id) : nullptr;
    if (setting && Current(*setting) != setting->initial) { Send(*setting, setting->initial); }
  }
  if (page == 1) { input::ResetBindings(); }
  if (page == 2) { input::ResetPadBindings(); }
}

std::vector<std::string> Choices(const Row& row, int* current) {
  *current = 0;
  if (row.kind == Kind::Value && row.id == kGraphicsPreset) {
    *current = std::max(PresetIndex(), 0);
    std::vector<std::string> out;
    for (const Preset& preset : Presets()) { out.push_back(preset.label); }
    return out;
  }
  const Setting* setting = row.kind == Kind::Value ? Find(row.id) : nullptr;
  if (!setting) { return {}; }
  if (setting->toggle) {
    *current = Current(*setting) == "true";
    return {"OFF", "ON"};
  }
  std::vector<std::string> out;
  for (const Choice& choice : setting->choices) { out.push_back(choice.label); }
  *current = ChoiceIndex(*setting, Current(*setting));
  return out;
}

void Choose(const Row& row, int index) {
  if (row.kind == Kind::Value && row.id == kGraphicsPreset) {
    if (index >= 0 && index < int(Presets().size())) { ApplyPreset(size_t(index)); }
    return;
  }
  const Setting* setting = row.kind == Kind::Value ? Find(row.id) : nullptr;
  if (!setting || index < 0) { return; }
  if (setting->toggle) { Send(*setting, index ? "true" : "false"); }
  else if (index < int(setting->choices.size())) { Send(*setting, setting->choices[size_t(index)].value); }
}

bool Shown(const Row& row) { return row.kind != Kind::Value || !GyroRow(row.id) || MotionSensor(); }
bool Enabled(const Row& row) { return row.kind != Kind::Value || Usable(row.id); }

}  // namespace rows
}  // namespace app
