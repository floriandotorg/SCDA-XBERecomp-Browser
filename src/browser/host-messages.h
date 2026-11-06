#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Two single producer, single consumer rings between the guest and the page (host-messages.mjs mirrors the layout):
// guest to host carries setting changes made in the game's own options pages, the Exit item's quit and console output;
// host to guest carries console command lines. Each ring has a head its producer advances and a tail its consumer
// advances, both counting entries; an entry is a kind, a length and up to 256 bytes of UTF-8.
namespace browser::host_messages {

inline constexpr uint32_t kEntries = 32;
inline constexpr uint32_t kPayloadBytes = 256;
inline constexpr uint32_t kEntryWords = 2 + kPayloadBytes / 4;
inline constexpr uint32_t kHeaderWords = 8;
inline constexpr uint32_t kWords = kHeaderWords + 2 * kEntries * kEntryWords;
inline constexpr uint32_t kGuestHead = 0, kGuestTail = 1, kHostHead = 2, kHostTail = 3, kDropped = 4;
inline constexpr uint32_t kGuestRing = kHeaderWords, kHostRing = kHeaderWords + kEntries * kEntryWords;

enum Kind : uint32_t { kSet = 1, kUnset = 2, kQuit = 3, kConsole = 4, kOutput = 5 };

using Block = std::array<std::atomic<uint32_t>, kWords>;
static_assert(sizeof(Block) == kWords * 4 && std::atomic<uint32_t>::is_always_lock_free);

struct Message {
  uint32_t kind;
  std::string text;
};

// one per module: it outlives every story session and restart
Block& Shared();
// guest to host, from any guest thread; false when the ring is full or the text too long (counted in kDropped)
bool Post(Block& block, uint32_t kind, std::string_view text);
inline bool Post(uint32_t kind, std::string_view text) { return Post(Shared(), kind, text); }
// host to guest, from one guest thread at a time; an entry whose length is out of range is skipped
std::optional<Message> Take(Block& block);
inline std::optional<Message> Take() { return Take(Shared()); }

}
