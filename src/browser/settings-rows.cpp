// app::rows for the browser: the game's own options pages (options_menu.cpp) show the page's settings.mjs values,
// which reach the guest as host.<key> lines, and send changes back through the host message ring; the page applies
// and saves them and publishes the next settings revision. A change shows at once from a pending copy until then.
#include "app/app.h"
#include "browser/host-messages.h"
#include "browser/host-settings.h"
#include "browser/keyboard-input.h"
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
    {17, "frameCap", "60", {{"0", "DISPLAY RATE"}, {"30", "30"}, {"60", "60"}}},
    {18, "anisotropy", "0", {{"0", "OFF"}, {"2", "2X"}, {"4", "4X"}, {"8", "8X"}, {"16", "16X"}}},
    {19, "language", "auto", {{"auto", "AUTOMATIC"}, {"int", "ENGLISH"}, {"fra", "FRANCAIS"}, {"deu", "DEUTSCH"}, {"ita", "ITALIANO"}, {"esp", "ESPANOL"}}},
    {20, "audioOutput", "stereo", {{"stereo", "2.0 STEREO"}, {"mono", "MONO"}}},
  };
  return list;
}

const Setting* Find(int id) {
  for (const Setting& setting : Settings()) {
    if (setting.id == id) { return &setting; }
  }
  return nullptr;
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
  {Kind::Heading, -1, "GRAPHICS"},
  {Kind::Value, 2, "FPS COUNTER"},
  {Kind::Value, 18, "ANISOTROPIC FILTERING"},
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
                            {Kind::Value, 9, "INVERT MOUSE"}, {Kind::Heading, -1, "KEYBOARD"}};
    size_t count = 0;
    const input::Action* actions = input::Actions(&count);
    for (size_t i = 0; i < count; i++) {
      if (!HiddenAction(actions[i].id)) { out.push_back({Kind::Bind, int(i), actions[i].label}); }
    }
    return out;
  }();
  return list;
}

const std::vector<Row>& PadRows() {
  static const std::vector<Row> list = [] {
    std::vector<Row> out;
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
  const Setting* setting = row.kind == Kind::Value ? Find(row.id) : nullptr;
  return setting ? Text(*setting, Current(*setting)) : std::string();
}

void Step(const Row& row, int step) {
  if (const Setting* setting = row.kind == Kind::Value && step ? Find(row.id) : nullptr) { StepSetting(*setting, step); }
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
  const Setting* setting = row.kind == Kind::Value ? Find(row.id) : nullptr;
  if (!setting || index < 0) { return; }
  if (setting->toggle) { Send(*setting, index ? "true" : "false"); }
  else if (index < int(setting->choices.size())) { Send(*setting, setting->choices[size_t(index)].value); }
}

bool Shown(const Row&) { return true; }
bool Enabled(const Row&) { return true; }

}  // namespace rows
}  // namespace app
