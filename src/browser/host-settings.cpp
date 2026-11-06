#include "browser/host-settings.h"
#include "app/app.h"
#include <algorithm>
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
};

void ApplyLine(app::Settings& settings, std::string_view line) {
  const size_t equals = line.find('=');
  if (equals == std::string_view::npos) { return; }
  const std::string_view name = line.substr(0, equals), value = line.substr(equals + 1);
  const auto entry = [&](std::string_view prefix) { return name.substr(prefix.size()); };
  if (name == "language") { settings.language = value; }
  else if (name.starts_with("keys.")) { settings.keys[std::string(entry("keys."))] = value; }
  else if (name.starts_with("pad_buttons.")) { settings.pad_buttons[std::string(entry("pad_buttons."))] = value; }
}

app::Settings Decode(const Snapshot& snapshot) {
  app::Settings settings;
  if (snapshot.words[kSchemaWord] != kSchema) { return settings; }
  for (const auto& entry : kBools) { settings.*entry.field = (snapshot.words[kFlagsWord] >> entry.bit) & 1; }
  std::string_view text = snapshot.text;
  while (!text.empty()) {
    const size_t end = text.find('\n');
    ApplyLine(settings, text.substr(0, end));
    text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
  }
  return settings;
}
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

// the guest frame is a fixed 640 x 480
float Aspect() { return 4.0f / 3.0f; }

float FovScale() {
  constexpr double kRadians = 3.14159265358979323846 / 360.0;
  return float(std::tan(Get().fov * kRadians) / std::tan(75 * kRadians));
}

}
