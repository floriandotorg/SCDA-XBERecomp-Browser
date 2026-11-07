#include "app/app.h"
#include "browser/host-settings.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

namespace settings = browser::host_settings;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

void Publish(settings::Block& block, bool skip, const std::string& language) {
  auto& revision = block.words[settings::kRevisionWord];
  uint32_t current = revision.load();
  while ((current & 1) || !revision.compare_exchange_weak(current, current + 1)) { current &= ~1u; }
  block.words[settings::kSchemaWord].store(settings::kSchema);
  block.words[settings::kFlagsWord].store((skip ? 1u << settings::kSkipIntrosBit : 0) | (skip ? 0 : 1u << settings::kPauseOnFocusLossBit));
  const std::string text = "language=" + language + "\nkeys.fire=Mouse1\n";
  for (uint32_t n = 0; n < block.text.size(); ++n) {
    uint32_t word = 0;
    for (uint32_t byte = 0; byte < 4; ++byte) {
      const size_t at = n * 4 + byte;
      if (at < text.size()) { word |= uint32_t(uint8_t(text[at])) << (byte * 8); }
    }
    block.text[n].store(word);
  }
  revision.store(current + 2);
}

int main() {
  auto& block = settings::Shared();
  check(!app::Get().skip_intros && app::Get().pause_on_focus_loss && app::Get().language == "auto", "an unwritten block did not give defaults");
  check(app::SettingsRevision() == 0 && app::Aspect() == 4.0f / 3.0f && app::FovScale() == 1.0f, "fixed frame shape or fov");
  Publish(block, true, "fra");
  check(app::Get().skip_intros && !app::Get().pause_on_focus_loss && app::Get().language == "fra", "a published block was not read");
  check(app::Get().keys.at("fire") == "Mouse1" && app::SettingsRevision() == 1, "map fields or revision");
  constexpr uint32_t writes = 20000;
  std::atomic<bool> done = false;
  std::atomic<uint32_t> torn = 0, observed = 0;
  std::vector<std::thread> readers;
  for (uint32_t n = 0; n < 2; ++n) {
    readers.emplace_back([&] {
      uint32_t previous = 0;
      while (!done.load()) {
        const auto& current = app::Get();
        if (current.skip_intros != (current.language == "fra") || current.skip_intros == current.pause_on_focus_loss) { ++torn; }
        const uint32_t revision = app::SettingsRevision();
        if (revision < previous) { ++torn; }
        previous = revision;
        ++observed;
      }
    });
  }
  for (uint32_t n = 0; n < writes; ++n) { Publish(block, n % 2 == 0, n % 2 == 0 ? "fra" : "deu"); }
  done = true;
  for (auto& reader : readers) { reader.join(); }
  check(torn == 0, "a reader saw a torn settings write");
  check(observed > 0, "readers never ran");
  Publish(block, true, "fra");
  check(app::Get().skip_intros && app::Get().language == "fra", "the final write was not read");
  block.words[settings::kAspectWord].store(std::bit_cast<uint32_t>(16.0f / 9.0f));
  check(app::Aspect() == 4.0f / 3.0f, "the shape changed before a Present latched it");
  check(settings::LatchAspect() == 16.0f / 9.0f && app::Narrow() == 0.75f && app::Widescreen(), "Narrow(16/9) is 3/4");
  block.words[settings::kAspectWord].store(std::bit_cast<uint32_t>(1.0f));
  check(settings::LatchAspect() == 4.0f / 3.0f && app::Narrow() == 1.0f && !app::Widescreen(), "a portrait shape stays 4:3");
  auto& revision = block.words[settings::kRevisionWord];
  revision.store(revision.load() + 1);
  block.words[settings::kFovWord].store(90);
  block.words[settings::kFmvScaleWord].store(1);
  revision.store(revision.load() + 1);
  const float expected = float(std::tan(90 * 3.14159265358979323846 / 360) / std::tan(75 * 3.14159265358979323846 / 360));
  check(std::abs(app::FovScale() - expected) < 1e-6f && app::Get().fmv_scale == 1, "FovScale(90) and the movie scale");
  check(app::Get().vibration_strength == 0 && app::Get().colour_blind == 0, "a written block sends the vibration strength");
  revision.store(revision.load() + 1);
  block.words[settings::kFlagsWord].store(1u << settings::kInvertMouseXBit | 1u << settings::kInvertPadYBit | 1u << settings::kGyroEnabledBit);
  block.words[settings::kMouseVerticalSensitivityWord].store(std::bit_cast<uint32_t>(0.5f));
  block.words[settings::kPadAimVerticalSensitivityWord].store(std::bit_cast<uint32_t>(2.5f));
  block.words[settings::kPadDeadZoneWord].store(std::bit_cast<uint32_t>(1.5f));
  block.words[settings::kVibrationStrengthWord].store(500);
  block.words[settings::kColourBlindWord].store(2);
  block.words[settings::kGyroSensitivityWord].store(std::bit_cast<uint32_t>(3.0f));
  block.words[settings::kGyroAxisWord].store(7);
  revision.store(revision.load() + 1);
  const auto& input = app::Get();
  check(input.invert_mouse_x && !input.invert_pad_x && input.invert_pad_y && input.gyro.enabled, "the input flags");
  check(input.mouse_vertical_sensitivity == 0.5f && input.pad_aim_vertical_sensitivity == 2.5f && input.pad_horizontal_sensitivity == 1.0f,
        "the sensitivities, an unsent one at its default");
  check(input.pad_dead_zone == 1.0f && input.vibration_strength == 200, "the dead zone and vibration strength are clamped");
  check(input.colour_blind == 2 && input.gyro.sensitivity == 3.0f && input.gyro.axis == gyro::kYaw, "colour blind mode and the gyro");
  std::puts("host-settings-contracts-ok");
}
