#pragma once
#include "browser/temporary-filesystem.h"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// What a switch between the story and versus modules hands over: the 4 KiB launch page, the persisted display the
// next image shows while it loads, and the profile filesystem both images share (title 5553005E). The old module packs
// it into JS before teardown; the new one unpacks it after image_prepare, before its runtime binds.
namespace browser::image_carry {

struct Display {
  uint32_t common = 0, format = 0, size = 0;
  std::vector<uint8_t> pixels;
};
struct Carry {
  std::optional<std::array<uint8_t, 4096>> page;
  std::optional<Display> display;
};
std::vector<uint8_t> Pack(const Carry& carry, const TemporaryFilesystem& profile);
// false when bytes are not a complete carry; profile then holds whatever was restored before the fault
bool Unpack(std::span<const uint8_t> bytes, Carry& carry, TemporaryFilesystem& profile);

}
