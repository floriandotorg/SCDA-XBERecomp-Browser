#include "browser/host-messages.h"
#include <mutex>

namespace browser::host_messages {
namespace {
std::mutex posting;
}

Block& Shared() {
  static Block block;
  return block;
}

bool Post(Block& block, uint32_t kind, std::string_view text) {
  std::lock_guard lock(posting);
  const uint32_t head = block[kGuestHead].load(std::memory_order_relaxed);
  const uint32_t tail = block[kGuestTail].load(std::memory_order_acquire);
  if (text.size() > kPayloadBytes || head - tail >= kEntries) {
    block[kDropped].fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint32_t entry = kGuestRing + (head % kEntries) * kEntryWords;
  block[entry].store(kind, std::memory_order_relaxed);
  block[entry + 1].store(uint32_t(text.size()), std::memory_order_relaxed);
  for (uint32_t n = 0; n < kPayloadBytes / 4; ++n) {
    uint32_t word = 0;
    for (uint32_t byte = 0; byte < 4; ++byte) {
      if (n * 4 + byte < text.size()) { word |= uint32_t(uint8_t(text[n * 4 + byte])) << (byte * 8); }
    }
    block[entry + 2 + n].store(word, std::memory_order_relaxed);
  }
  block[kGuestHead].store(head + 1, std::memory_order_release);
  return true;
}

std::optional<Message> Take(Block& block) {
  for (;;) {
    const uint32_t tail = block[kHostTail].load(std::memory_order_relaxed);
    const uint32_t head = block[kHostHead].load(std::memory_order_acquire);
    if (head == tail) { return std::nullopt; }
    // a page that wrote past a whole ring lost the oldest entries
    const uint32_t first = head - tail > kEntries ? head - kEntries : tail;
    const uint32_t entry = kHostRing + (first % kEntries) * kEntryWords;
    Message message{block[entry].load(std::memory_order_relaxed), {}};
    const uint32_t length = block[entry + 1].load(std::memory_order_relaxed);
    if (length <= kPayloadBytes) {
      message.text.resize(length);
      for (uint32_t n = 0; n < length; ++n) { message.text[n] = char(block[entry + 2 + n / 4].load(std::memory_order_relaxed) >> ((n % 4) * 8)); }
    }
    block[kHostTail].store(first + 1, std::memory_order_release);
    if (length <= kPayloadBytes) { return message; }
  }
}

}
