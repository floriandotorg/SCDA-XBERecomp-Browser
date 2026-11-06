#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

// Host settings from settings.mjs (host-settings.mjs holds the schema these indices mirror). The page writes the
// block under a seqlock: word 0 is odd while a writer holds it. Byte area: name=value lines for string and map fields.
namespace browser::host_settings {

inline constexpr uint32_t kSchema = 1;
inline constexpr uint32_t kWords = 64;
inline constexpr uint32_t kBytes = 4096;
inline constexpr uint32_t kRevisionWord = 0;
inline constexpr uint32_t kSchemaWord = 1;
inline constexpr uint32_t kFlagsWord = 2;
inline constexpr uint32_t kSkipIntrosBit = 0;
inline constexpr uint32_t kPauseOnFocusLossBit = 1;
inline constexpr uint32_t kInvertMouseBit = 2;
inline constexpr uint32_t kMouseSensitivityWord = 3;
inline constexpr uint32_t kFreeLookSensitivityWord = 4;
inline constexpr uint32_t kWeaponSensitivityWord = 5;
inline constexpr uint32_t kSniperSensitivityWord = 6;
inline constexpr uint32_t kFovWord = 7;
inline constexpr uint32_t kFmvScaleWord = 8;
inline constexpr uint32_t kAspectWord = 9;
inline constexpr uint32_t kResolutionWord = 10;
inline constexpr uint32_t kRenderPercentWord = 11;
inline constexpr uint32_t kDeviceWidthWord = 12;
inline constexpr uint32_t kDeviceHeightWord = 13;
inline constexpr uint32_t kAnisoWord = 14;
inline constexpr uint32_t kBrightnessWord = 15;
inline constexpr uint32_t kContrastWord = 16;
inline constexpr uint32_t kFpsCapWord = 17;
inline constexpr uint32_t kDisplayRateWord = 18;
inline constexpr uint32_t kAudioOutputWord = 19;
inline constexpr uint32_t kMsaaWord = 20;
inline constexpr uint32_t kSmaaBit = 3;

struct Block {
  std::array<std::atomic<uint32_t>, kWords> words{};
  std::array<std::atomic<uint32_t>, kBytes / 4> text{};
};
static_assert(sizeof(Block) == kWords * 4 + kBytes && std::atomic<uint32_t>::is_always_lock_free);

struct Snapshot {
  uint32_t revision = 0;
  std::array<uint32_t, kWords> words{};
  std::string text;
};

// one per module: it outlives every story session and restart
Block& Shared();
// a browser setting the in-game options pages show (host.<key>=<value> lines) as the calling thread's app::Get() last
// read it, empty when the page sent none
std::string HostValue(std::string_view key);
// app::Aspect() for the frame being drawn: the page's shape, taken once a Present so one frame's hooks and its
// presentation agree (MenuBackend's output source calls it)
float LatchAspect();
// false while a writer holds the block or it changed under every attempt
bool Read(const Block& block, Snapshot& snapshot);

}
