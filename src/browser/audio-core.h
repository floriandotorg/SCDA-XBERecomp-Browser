#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace browser::audio {

inline constexpr uint32_t kRate = 48000;
inline constexpr uint32_t kMaxVoices = 64;
inline constexpr uint32_t kMaxPacketsPerVoice = 32;
inline constexpr uint32_t kMaxChannels = 8;
inline constexpr uint32_t kMaxSampleRate = 384000;
inline constexpr uint32_t kFormatPcm = 1;
inline constexpr uint32_t kFormatXboxAdpcm = 0x69;
inline constexpr uint32_t kAdpcmFramesPerBlock = 64;
inline constexpr uint32_t kAdpcmBlockBytesPerChannel = 36;
inline constexpr uint32_t kPacketPending = 0x8000000Au;
inline constexpr uint32_t kPacketFlushed = 0x80004004u;
inline constexpr uint32_t kStatusPlaying = 0x1;
inline constexpr uint32_t kStatusLooping = 0x4;
inline constexpr uint32_t kLockEntireBuffer = 0x1;
inline constexpr uint32_t kPlayLooping = 0x1;
inline constexpr uint32_t kPlayFromStart = 0x2;
inline constexpr uint32_t kGuestErrorFail = 0x80004005u;
inline constexpr uint32_t kGuestErrorInvalidCall = 0x88780032u;

enum class AudioError {
  kNone,
  kInvalidFormat,
  kUnsupportedFormat,
  kOverflow,
  kVoiceLimit,
  kPacketLimit,
  kInvalidLoop,
  kInvalidPosition,
  kEmptyBuffer,
  kUnknownVoice,
  kDuplicateVoice,
  kInvalidState,
  kRetainedCompletions
};

struct Format {
  uint16_t tag = 0;
  uint16_t channels = 0;
  uint16_t align = 0;
  uint16_t bits = 0;
  uint32_t rate = 0;
  bool operator==(const Format&) const = default;
  static std::optional<Format> Make(uint32_t tag, uint32_t channels, uint32_t rate, uint32_t align, uint32_t bits);
};

std::optional<uint32_t> FramesOf(const Format& format, uint64_t bytes);
std::optional<uint32_t> BytesOf(const Format& format, uint32_t frames);

struct Completion {
  uint32_t voice = 0;
  uint64_t token = 0;
  uint32_t completedBytes = 0;
  uint32_t status = 0;
};

struct LockRange {
  uint32_t firstOffset = 0;
  uint32_t firstBytes = 0;
  uint32_t secondOffset = 0;
  uint32_t secondBytes = 0;
};

struct StreamInfo {
  uint32_t flags = 0x3;
  uint32_t packetSize = 0;
  uint32_t reserved1 = 0;
  uint32_t reserved2 = 0;
};

class Voice {
public:
  static Voice Buffer(const Format& format, uint32_t bytes);
  static Voice Stream(const Format& format, uint32_t maxPackets);

  bool IsStream() const;
  const Format& GetFormat() const;
  uint32_t BufferBytes() const;
  uint32_t MaxPackets() const;
  uint32_t QueuedPackets() const;
  uint32_t Frequency() const;
  uint32_t RetainedRecords() const;
  bool HasUndeliveredCompletions() const;
  bool Active() const;
  bool Paused() const;
  bool Ending() const;

  AudioError Play(uint64_t now, uint32_t flags);
  AudioError Stop(uint64_t now);
  AudioError Pause(uint64_t now, bool paused);
  AudioError SetFrequency(uint64_t now, uint32_t frequency);
  AudioError SetBufferLength(uint64_t now, uint32_t bytes);
  AudioError SetLoopRegion(uint32_t offsetBytes, uint32_t lengthBytes);
  AudioError SetCurrentPosition(uint64_t now, uint32_t bytes);

  AudioError ProcessPacket(uint64_t now, uint64_t token, uint32_t bytes);
  void Discontinuity(uint64_t now, std::vector<Completion>& out);
  void Flush(std::vector<Completion>& out);
  bool FlushEx(bool onlyIfActive, std::vector<Completion>& out);
  void Release(std::vector<Completion>& out);

  void Advance(uint64_t now, std::vector<Completion>& out);

  uint32_t Status(uint64_t now) const;
  bool StreamReady() const;
  StreamInfo GetStreamInfo() const;
  uint32_t PlayFrames(uint64_t now) const;
  std::pair<uint32_t, uint32_t> CurrentPosition(uint64_t now) const;
  AudioError Lock(uint32_t offsetBytes, uint32_t bytes, uint32_t flags, LockRange& out) const;

private:
  struct Packet {
    uint64_t token = 0;
    uint32_t bytes = 0;
    uint32_t frames = 0;
    uint32_t used = 0;
  };

  void AnchorProgress(uint64_t now);
  void DrainPending(std::vector<Completion>& out);
  uint32_t EndFrames() const;
  uint32_t LoopStartFrames() const;
  uint64_t StreamQueuedFrames() const;
  uint32_t EffectiveRate() const;
  bool Advancing() const;
  void Deactivate();

  Format format_;
  bool stream_ = false;
  uint32_t bytes_ = 0;
  uint32_t loopStart_ = 0;
  uint32_t loopLength_ = 0;
  bool playing_ = false;
  bool looping_ = false;
  bool paused_ = false;
  bool active_ = false;
  bool ending_ = false;
  uint32_t frequency_ = 0;
  uint64_t anchor_ = 0;
  uint64_t baseNumerator_ = 0;
  uint32_t maxPackets_ = 1;
  uint32_t packetHead_ = 0;
  std::vector<Packet> packets_;
  std::vector<Completion> pending_;
};

class VoiceBank {
public:
  AudioError CreateBuffer(uint32_t id, const Format& format, uint32_t bytes);
  AudioError CreateStream(uint32_t id, const Format& format, uint32_t maxPackets);
  AudioError Erase(uint32_t id);
  Voice* Find(uint32_t id);
  const Voice* Find(uint32_t id) const;
  uint32_t Count() const;
  bool Sounding(uint64_t now) const;
  std::vector<uint32_t> Ids() const;
  void AdvanceAll(uint64_t now, std::vector<Completion>& out);

private:
  std::map<uint32_t, Voice> voices_;
};

}
