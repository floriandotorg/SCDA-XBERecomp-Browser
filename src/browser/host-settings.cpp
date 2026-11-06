#include "browser/host-settings.h"
#include "app/app.h"
#include <algorithm>
#include <bit>
#include <map>
#include <cmath>
#include <string_view>
#include <thread>

namespace browser::host_settings {
namespace {
constexpr uint32_t kAttempts = 64;

struct BoolField {
  uint32_t bit;
  bool app::Settings::*field;
};
constexpr BoolField kBools[] = {
  {kSkipIntrosBit, &app::Settings::skip_intros},
  {kPauseOnFocusLossBit, &app::Settings::pause_on_focus_loss},
  {kInvertMouseBit, &app::Settings::invert_mouse},
  {kSmaaBit, &app::Settings::smaa},
  {kHdrRenderingBit, &app::Settings::hdr_rendering},
  {kToneMappingBit, &app::Settings::tone_mapping},
  {kHdrOutputBit, &app::Settings::hdr_output},
};
struct FloatField {
  uint32_t word;
  float app::Settings::*field;
};
constexpr FloatField kFloats[] = {
  {kMouseSensitivityWord, &app::Settings::mouse_sensitivity},
  {kFreeLookSensitivityWord, &app::Settings::free_look_sensitivity},
  {kWeaponSensitivityWord, &app::Settings::weapon_sensitivity},
  {kSniperSensitivityWord, &app::Settings::sniper_sensitivity},
};
thread_local std::map<std::string, std::string, std::less<>> hostValues;

void ApplyLine(app::Settings& settings, std::string_view line) {
  const size_t equals = line.find('=');
  if (equals == std::string_view::npos) { return; }
  const std::string_view name = line.substr(0, equals), value = line.substr(equals + 1);
  const auto entry = [&](std::string_view prefix) { return name.substr(prefix.size()); };
  if (name == "language") { settings.language = value; }
  else if (name.starts_with("keys.")) { settings.keys[std::string(entry("keys."))] = value; }
  else if (name.starts_with("pad_buttons.")) { settings.pad_buttons[std::string(entry("pad_buttons."))] = value; }
  else if (name.starts_with("host.")) { hostValues[std::string(entry("host."))] = value; }
}

app::Settings Decode(const Snapshot& snapshot) {
  app::Settings settings;
  if (snapshot.words[kSchemaWord] != kSchema) { return settings; }
  for (const auto& entry : kBools) { settings.*entry.field = (snapshot.words[kFlagsWord] >> entry.bit) & 1; }
  // an unsent scalar is 0, which no setting takes: the default stays
  for (const auto& entry : kFloats) {
    const float value = std::bit_cast<float>(snapshot.words[entry.word]);
    if (snapshot.words[entry.word] && std::isfinite(value)) { settings.*entry.field = value; }
  }
  if (snapshot.words[kFovWord]) { settings.fov = std::clamp<uint32_t>(snapshot.words[kFovWord], 45, 140); }
  settings.fmv_scale = snapshot.words[kFmvScaleWord] ? 1 : 0;
  settings.aniso = std::min<uint32_t>(snapshot.words[kAnisoWord], 16);
  settings.brightness = std::min<uint32_t>(snapshot.words[kBrightnessWord], 100);
  settings.contrast = std::min<uint32_t>(snapshot.words[kContrastWord], 100);
  settings.fps_cap = snapshot.words[kFpsCapWord];
  settings.audio_output = snapshot.words[kAudioOutputWord];
  settings.msaa = snapshot.words[kMsaaWord] == 4 ? 4 : 0;
  settings.shader_model = snapshot.words[kShaderModelWord] ? 3 : 1;
  if (snapshot.words[kShadowResolutionWord]) { settings.shadow_resolution = std::clamp<uint32_t>(snapshot.words[kShadowResolutionWord], 512, 4096); }
  if (snapshot.words[kShadowFilterWord]) { settings.shadow_filter = snapshot.words[kShadowFilterWord] >= 3 ? 3 : 1; }
  if (snapshot.words[kShadowDistanceWord]) { settings.shadow_distance = std::clamp<uint32_t>(snapshot.words[kShadowDistanceWord], 1, 8); }
  if (snapshot.words[kRenderPercentWord]) { settings.render_percent = std::clamp<uint32_t>(snapshot.words[kRenderPercentWord], 20, 200); }
  hostValues.clear();
  std::string_view text = snapshot.text;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    ApplyLine(settings, text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
  }
  return settings;
}
}

std::string HostValue(std::string_view key) {
  app::Get();
  const auto found = hostValues.find(key);
  return found == hostValues.end() ? std::string() : found->second;
}

namespace {
std::atomic<float> latchedAspect = 4.0f / 3.0f;
}

float LatchAspect() {
  const float aspect = std::bit_cast<float>(Shared().words[kAspectWord].load(std::memory_order_acquire));
  latchedAspect.store(std::isfinite(aspect) && aspect >= 4.0f / 3.0f ? std::min(aspect, 4.0f) : 4.0f / 3.0f, std::memory_order_release);
  return latchedAspect.load(std::memory_order_acquire);
}

Block& Shared() {
  static Block block;
  return block;
}

bool Read(const Block& block, Snapshot& snapshot) {
  for (uint32_t attempt = 0; attempt < kAttempts; ++attempt) {
    const uint32_t before = block.words[kRevisionWord].load(std::memory_order_acquire);
    if (before & 1) {
      std::this_thread::yield();
      continue;
    }
    for (uint32_t n = 1; n < kWords; ++n) { snapshot.words[n] = block.words[n].load(std::memory_order_relaxed); }
    std::array<char, kBytes> bytes;
    for (uint32_t n = 0; n < block.text.size(); ++n) {
      const uint32_t word = block.text[n].load(std::memory_order_relaxed);
      for (uint32_t byte = 0; byte < 4; ++byte) { bytes[n * 4 + byte] = char(word >> (byte * 8)); }
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (block.words[kRevisionWord].load(std::memory_order_relaxed) != before) { continue; }
    snapshot.revision = snapshot.words[kRevisionWord] = before;
    snapshot.text.assign(bytes.begin(), std::find(bytes.begin(), bytes.end(), '\0'));
    return true;
  }
  return false;
}

}

namespace app {

// Each guest thread keeps its own copy, refreshed when the page publishes a new revision; a hook's writes to it stay
// on that thread until then.
Settings& Get() {
  thread_local Settings settings;
  thread_local uint32_t seen = 0;
  namespace hs = browser::host_settings;
  const uint32_t revision = hs::Shared().words[hs::kRevisionWord].load(std::memory_order_acquire);
  if (revision != seen) {
    hs::Snapshot snapshot;
    if (hs::Read(hs::Shared(), snapshot)) {
      settings = hs::Decode(snapshot);
      seen = snapshot.revision;
    }
  }
  return settings;
}

uint32_t SettingsRevision() {
  namespace hs = browser::host_settings;
  return hs::Shared().words[hs::kRevisionWord].load(std::memory_order_acquire) / 2;
}

// the guest frame stays 640 x 480 and is shown this wide, as on a native window of that shape
float Aspect() {
  namespace hs = browser::host_settings;
  return hs::latchedAspect.load(std::memory_order_acquire);
}

float FovScale() {
  constexpr double kRadians = 3.14159265358979323846 / 360.0;
  return float(std::tan(Get().fov * kRadians) / std::tan(75 * kRadians));
}

}

namespace app {

// "auto" is the browser's language when the game has it (the page sends it resolved), English otherwise
std::string ResolveLanguage(const std::string& setting) {
  for (const char* language : kLanguages) {
    if (setting == language) { return setting; }
  }
  return "int";
}

const std::string& BootLanguage() {
  static const std::string language = ResolveLanguage(Get().language);
  return language;
}

}
