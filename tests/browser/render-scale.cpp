#include "browser/render-scale.h"
#include <cstdio>
#include <stdexcept>

namespace rs = browser::render_scale;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

int main() {
  auto output = rs::Compute(rs::kConsole, 150, 4.0f / 3.0f, 1920, 1440);
  check(output.width == 640 && output.height == 480 && output.scale == 1, "console mode is the frame's shape at 480 rows, 1x");
  output = rs::Compute(rs::kConsole, 100, 16.0f / 9.0f, 0, 0);
  check(output.width == 853 && output.height == 480, "console canvas at 16:9");
  output = rs::Compute(rs::kWindow, 100, 4.0f / 3.0f, 1440, 1080);
  check(output.width == 1440 && output.height == 1080 && output.scale == 2.25f, "100% of 1080 rows");
  auto host = rs::HostSize(640, 480, 640, 480, output.scale);
  check(host.first == 1440 && host.second == 1080, "640x480 at 100% of 1080 rows is 1440x1080");
  output = rs::Compute(rs::kWindow, 50, 16.0f / 9.0f, 1280, 720);
  host = rs::HostSize(640, 480, 640, 480, output.scale);
  check(output.scale == 0.75f && host.first == 480 && host.second == 360, "50% of 720 rows is 480x360");
  output = rs::Compute(rs::kWindow, 200, 16.0f / 9.0f, 20000, 12000);
  check(output.width == 8192 && output.height == 8192 && output.scale == rs::kMaximum, "the canvas and the scale are clamped");
  check(rs::Compute(rs::kWindow, 100, 4.0f / 3.0f, 0, 0).scale == 1, "no device size yet: console");
  host = rs::HostSize(512, 512, 640, 480, 2.25f);
  check(host.first == 512 && host.second == 512, "a target not the back buffer's size (the shadow map) stays 1x");
  host = rs::HostSize(640, 480, 640, 480, 1);
  check(host.first == 640 && host.second == 480, "scale 1 is identity");

  rs::Dynamic dynamic;
  float scale = 0;
  for (int n = 0; n < 60; ++n) { scale = dynamic.Next(2.0f, 25); }
  check(scale == 1.9f, "a second of slow frames lowers the scale a step");
  for (int n = 0; n < 600; ++n) { scale = dynamic.Next(2.0f, 25); }
  check(scale == 1.0f, "never below half the ceiling");
  for (int n = 0; n < 400; ++n) { scale = dynamic.Next(2.0f, 10); }
  check(scale > 1.0f && scale <= 2.0f, "fast frames raise it again");
  for (int n = 0; n < 10000; ++n) { scale = dynamic.Next(2.0f, 10); }
  check(scale == 2.0f, "never above the ceiling");
  std::puts("render-scale-contracts-ok");
}
