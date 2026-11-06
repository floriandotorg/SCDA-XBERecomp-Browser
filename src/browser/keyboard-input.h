#pragma once
#include <array>
#include <atomic>
#include <cstdint>

// The page's raw keyboard and mouse for the input core (input-core.cpp), keyboard-input.mjs KEYBOARD_INPUT mirrors
// it. The page writes every word but kFlags, which the guest writes; kMouseX and kMouseY the guest also takes back to 0.
namespace browser::keyboard_input {

inline constexpr uint32_t kWords = 64;
inline constexpr uint32_t kChange = 0;     // bumped by every page update
inline constexpr uint32_t kKeys = 1;       // 8 words: held keys, a bit per Windows virtual key code
inline constexpr uint32_t kWheelUp = 9;    // wheel notches, counted
inline constexpr uint32_t kWheelDown = 10;
inline constexpr uint32_t kPressing = 11;  // the left button is down after a press that will count as a click
inline constexpr uint32_t kClicks = 12;    // counted clicks: down and up while the guest takes the cursor
inline constexpr uint32_t kCursorX = 13;   // the cursor over the frame in CSS pixels, kCursorW 0 when it is not
inline constexpr uint32_t kCursorY = 14;
inline constexpr uint32_t kCursorW = 15;
inline constexpr uint32_t kCursorH = 16;
inline constexpr uint32_t kActive = 17;    // the page gives the game the input: focused, visible, no host panel
inline constexpr uint32_t kMouseX = 18;    // mouse movement in thousandths of a count, signed
inline constexpr uint32_t kMouseY = 19;
inline constexpr uint32_t kStartTaps = 20; // the pointer lock was lost in play (Escape): tap Start
inline constexpr uint32_t kCharHead = 21;  // UTF-16 units written to kChars, counted
inline constexpr uint32_t kChars = 22;     // 32 words: a ring of 64 typed UTF-16 units, two a word
inline constexpr uint32_t kCharUnits = 64;
inline constexpr uint32_t kFlags = 54;     // guest: what takes the cursor
inline constexpr uint32_t kMenuFlag = 1, kPointerFlag = 2, kTextFlag = 4, kMapDragFlag = 8, kMovieFlag = 16;
// a key capture of the game's options pages waits: the host leaves its own keys alone
inline constexpr uint32_t kCaptureFlag = 32;
inline constexpr uint32_t kHostPanel = 55;    // the page's settings panel is open over the game
inline constexpr uint32_t kHostConsole = 56;  // the page's console is open over the game

using Block = std::array<std::atomic<uint32_t>, kWords>;
static_assert(sizeof(Block) == kWords * 4 && std::atomic<uint32_t>::is_always_lock_free);

// one per module: it outlives every story session and restart
Block& Shared();

}
