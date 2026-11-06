#include "browser/settings-pack.h"
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace sp = browser::settings_pack;
using Clock = std::chrono::steady_clock;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

int main() {
  std::array<uint8_t, 768> identity;
  for (size_t n = 0; n < identity.size(); ++n) { identity[n] = uint8_t(n % 256); }
  check(sp::ComposeRamp(identity.data(), 50, 50) == identity, "50 / 50 is identity");
  for (auto [brightness, contrast] : {std::pair{0u, 50u}, {100u, 50u}, {50u, 0u}, {50u, 100u}}) {
    const auto ramp = sp::ComposeRamp(identity.data(), brightness, contrast);
    check(ramp[0] == 0 && ramp[255] == 255 && ramp[256] == 0 && ramp[767] == 255, "black and white stay fixed");
  }
  check(sp::ComposeRamp(identity.data(), 100, 50)[64] > 64 && sp::ComposeRamp(identity.data(), 0, 50)[64] < 64, "brightness lifts and lowers the darks");
  check(sp::ComposeRamp(identity.data(), 50, 100)[64] < 64 && sp::ComposeRamp(identity.data(), 50, 100)[192] > 192, "contrast steepens about mid grey");
  std::array<uint8_t, 768> calibrated;
  for (size_t n = 0; n < calibrated.size(); ++n) { calibrated[n] = uint8_t(std::min<size_t>(255, n % 256 + 10)); }
  check(sp::ComposeRamp(calibrated.data(), 50, 50) == calibrated, "the game's own ramp is kept");

  check(sp::Anisotropy(16, 2, 2, 2, 0, 5, false, false) == 16, "forced for a linear mipmapped stage");
  check(sp::Anisotropy(16, 1, 2, 2, 0, 5, false, false) == 1, "not for point min");
  check(sp::Anisotropy(16, 2, 2, 0, 0, 5, false, false) == 1, "not without mips");
  check(sp::Anisotropy(16, 2, 2, 2, 0, 1, false, false) == 1, "not for one level");
  check(sp::Anisotropy(16, 2, 2, 2, 0, 5, true, false) == 1 && sp::Anisotropy(16, 2, 2, 2, 0, 5, false, true) == 1, "not for shadow or border stages");
  check(sp::Anisotropy(0, 3, 2, 0, 8, 1, false, false) == 8 && sp::Anisotropy(0, 3, 2, 0, 32, 1, false, false) == 16, "the title's own anisotropic filter, capped");
  check(sp::Anisotropy(0, 2, 2, 2, 0, 5, false, false) == 1, "off forces nothing");

  auto time = Clock::time_point{};
  std::vector<double> sleeps;
  sp::Pacer pacer([&] { return time; }, [&](Clock::time_point until) { sleeps.push_back(std::chrono::duration<double, std::milli>(until - time).count()); time = until; });
  for (int n = 0; n < 10; ++n) {
    time += std::chrono::milliseconds(5);
    pacer.Pace(30, 60);
  }
  check(sleeps.size() == 10 && sleeps.back() > 28 && sleeps.back() < 29, "30 Hz periods of 33.3 ms");
  time += std::chrono::milliseconds(500);
  sleeps.clear();
  pacer.Pace(30, 60);
  check(sleeps.empty(), "a late frame resynchronises without sleeping");
  pacer.Pace(0, 60);
  pacer.Pace(60, 60);
  pacer.Pace(60, 61);
  check(sleeps.empty(), "no cap, or a cap at the display rate, never sleeps");
  check(sp::LanguageId("fra") == 4 && sp::LanguageId("deu") == 3 && sp::LanguageId("ita") == 6 && sp::LanguageId("esp") == 5 &&
        sp::LanguageId("int") == 1 && sp::LanguageId("xx") == 1, "language ids");
  std::puts("settings-pack-contracts-ok");
}
