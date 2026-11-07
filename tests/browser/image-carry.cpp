#include "browser/image-carry.h"
#include <cstdio>
#include <stdexcept>

const auto check = [](bool value, const char* reason) { if (!value) { throw std::runtime_error(reason); } };

int main() {
  browser::TemporaryFilesystem story(4u << 20);
  check(story.CreateDirectory("UDATA") == 0 && story.CreateDirectory("UDATA\\5553005E") == 0, "fixture directories");
  const std::vector<uint8_t> profile{1, 2, 3, 4, 5};
  check(story.WriteFile("UDATA\\5553005E\\PROFILE", profile) == 0, "fixture file");
  check(story.SetBasic("UDATA\\5553005E\\PROFILE", {10, 20, 30, 40}, 0x21) == 0, "fixture metadata");
  check(story.WriteFile("UDATA\\5553005E\\EMPTY", {}) == 0, "fixture empty file");
  browser::image_carry::Carry carry;
  carry.page.emplace();
  (*carry.page)[8] = 'D'; (*carry.page)[4095] = 7;
  carry.display = browser::image_carry::Display{0x40001, 0x12, 0x1df27f, {9, 8, 7, 6}};
  const auto bytes = browser::image_carry::Pack(carry, story);

  browser::TemporaryFilesystem versus(4u << 20);
  browser::image_carry::Carry received;
  check(browser::image_carry::Unpack(bytes, received, versus), "a packed carry did not unpack");
  check(received.page && *received.page == *carry.page, "the launch page changed");
  check(received.display && received.display->common == 0x40001 && received.display->format == 0x12 && received.display->size == 0x1df27f &&
    received.display->pixels == carry.display->pixels, "the kept display changed");
  std::vector<uint8_t> read(16);
  check(versus.Read("UDATA\\5553005E\\PROFILE", 0, read) == profile.size() && std::equal(profile.begin(), profile.end(), read.begin()), "the profile bytes changed");
  const auto meta = versus.Stat("UDATA\\5553005E\\PROFILE");
  check(meta && !meta->directory && meta->attributes == 0x21 && meta->creationTime == 10 && meta->lastWriteTime == 30, "the profile metadata changed");
  check(versus.Stat("UDATA\\5553005E\\EMPTY") && versus.Stat("UDATA\\5553005E")->directory, "an empty file or directory was lost");

  browser::image_carry::Carry bare;
  browser::TemporaryFilesystem empty(1u << 20), target(1u << 20);
  browser::image_carry::Carry none;
  check(browser::image_carry::Unpack(browser::image_carry::Pack(bare, empty), none, target) && !none.page && !none.display, "an empty carry gained content");
  auto truncated = bytes;
  truncated.resize(truncated.size() - 3);
  browser::TemporaryFilesystem rejected(4u << 20);
  browser::image_carry::Carry partial;
  check(!browser::image_carry::Unpack(truncated, partial, rejected), "a truncated carry was accepted");
  auto foreign = bytes;
  foreign[0] ^= 1;
  check(!browser::image_carry::Unpack(foreign, partial, rejected), "a foreign carry was accepted");
  std::puts("image-carry-ok");
}
