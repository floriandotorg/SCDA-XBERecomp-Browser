#include "browser/host-messages.h"
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace messages = browser::host_messages;

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};

// the page's post, as host-messages.mjs writes it
void HostPost(messages::Block& block, uint32_t kind, const std::string& text, uint32_t length) {
  const uint32_t head = block[messages::kHostHead].load();
  const uint32_t entry = messages::kHostRing + (head % messages::kEntries) * messages::kEntryWords;
  block[entry].store(kind);
  block[entry + 1].store(length);
  for (uint32_t n = 0; n < messages::kPayloadBytes / 4; ++n) {
    uint32_t word = 0;
    for (uint32_t byte = 0; byte < 4; ++byte) {
      if (n * 4 + byte < text.size()) { word |= uint32_t(uint8_t(text[n * 4 + byte])) << (byte * 8); }
    }
    block[entry + 2 + n].store(word);
  }
  block[messages::kHostHead].store(head + 1);
}

std::string GuestEntry(messages::Block& block, uint32_t index) {
  const uint32_t entry = messages::kGuestRing + (index % messages::kEntries) * messages::kEntryWords;
  std::string text(block[entry + 1].load(), '\0');
  for (uint32_t n = 0; n < text.size(); ++n) { text[n] = char(block[entry + 2 + n / 4].load() >> ((n % 4) * 8)); }
  return text;
}

int main() {
  auto block = std::make_unique<messages::Block>();
  for (uint32_t n = 0; n < messages::kEntries; ++n) { check(messages::Post(*block, messages::kSet, "volume=" + std::to_string(n)), "post"); }
  check(!messages::Post(*block, messages::kSet, "full"), "a full ring took an entry");
  check(!messages::Post(*block, messages::kSet, std::string(257, 'x')), "an entry over 256 bytes was taken");
  check((*block)[messages::kDropped].load() == 2, "drops are counted");
  check(GuestEntry(*block, 5) == "volume=5", "entry text");
  (*block)[messages::kGuestTail].store(messages::kEntries);
  check(messages::Post(*block, messages::kQuit, ""), "room after the page drained");
  check(GuestEntry(*block, messages::kEntries).empty() && (*block)[messages::kGuestHead].load() == messages::kEntries + 1, "wrap");

  check(!messages::Take(*block), "an empty host ring gave an entry");
  for (uint32_t round = 0; round < 3; ++round) {
    for (uint32_t n = 0; n < 20; ++n) { HostPost(*block, messages::kConsole, "say " + std::to_string(n), 5 + (n >= 10)); }
    for (uint32_t n = 0; n < 20; ++n) {
      const auto message = messages::Take(*block);
      check(message && message->kind == messages::kConsole && message->text == "say " + std::to_string(n), "host entries in order");
    }
  }
  HostPost(*block, messages::kConsole, "torn", 4000);
  HostPost(*block, messages::kConsole, "next", 4);
  const auto next = messages::Take(*block);
  check(next && next->text == "next" && !messages::Take(*block), "a torn length is skipped");

  std::vector<std::thread> writers;
  auto shared = std::make_unique<messages::Block>();
  for (int n = 0; n < 4; ++n) {
    writers.emplace_back([&] {
      for (int m = 0; m < 8; ++m) { messages::Post(*shared, messages::kSet, "keys.fire=G"); }
    });
  }
  for (auto& writer : writers) { writer.join(); }
  check((*shared)[messages::kGuestHead].load() == 32, "posts from several threads");
  std::puts("host-messages-contracts-ok");
}
