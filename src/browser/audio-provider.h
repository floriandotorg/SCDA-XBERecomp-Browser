#pragma once
#include "browser/audio-core.h"
#include "browser/sdk-provider.h"
#include "browser/silent-audio.h"
#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace kernel { class DiscIndex; }

namespace browser {

class AudioProvider final : public SdkGraphics {
public:
  static constexpr uint32_t kArenaBase = 0xc0100000;
  static constexpr uint32_t kArenaCapacity = 4u << 20;
  static constexpr uint32_t kDeviceBytes = 0x40;
  static constexpr uint32_t kBufferObjectBytes = 0x40;
  static constexpr uint32_t kStreamObjectBytes = 0x80;
  static constexpr uint32_t kColdSpeakerConfig = 0;
  static constexpr uint32_t kMixBinCount = 32;
  static constexpr uint32_t kFeedChunkFrames = 4096;
  static constexpr size_t kMaxPendingCallbacks = audio::kMaxVoices * audio::kMaxPacketsPerVoice;

  struct SnapshotData {
    uint32_t device = 0;
    uint32_t voices = 0;
    uint32_t voiceLimitRejections = 0;
    uint32_t arenaFreeBytes = 0;
    uint64_t publishedCompletions = 0;
    uint64_t deliveredCallbacks = 0;
    uint64_t pendingCallbacks = 0;
  };

  AudioProvider(Runtime& runtime, PreparedImage& image, std::shared_ptr<SilentAudioSink> sink,
    std::shared_ptr<const kernel::DiscIndex> disc);
  AudioProvider(const AudioProvider&) = delete;
  AudioProvider& operator=(const AudioProvider&) = delete;

  bool BelongsTo(Runtime& runtime, PreparedImage& image) const override;
  bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) override;
  void Tick(const X86Context& context, uint64_t now);
  SnapshotData Snapshot() const;

private:
  struct ArenaBlock {
    uint32_t address;
    uint32_t size;
    bool allocated;
  };
  struct VoiceState {
    uint32_t obj = 0;
    bool stream = false;
    uint32_t refs = 1;
    uint32_t data = 0;
    uint32_t owned = 0;
    uint32_t callback = 0;
    uint32_t context = 0;
    int32_t volumeMb = 0;
    uint32_t headroomMb = 0;
    bool is3d = false;
    std::array<uint32_t, 3> position{};
    uint32_t minDistance = 0;
    uint32_t maxDistance = 0;
    std::array<uint32_t, 9> i3dl2Source{};
    std::vector<std::pair<uint32_t, int32_t>> bins;
    std::vector<std::pair<uint32_t, int32_t>> binVolumes;
    uint64_t cachedAddress = UINT64_MAX;
    uint32_t cachedBytes = 0;
    std::array<uint8_t, 4096> cachedData{};
    std::array<int16_t, 65 * audio::kMaxChannels> decoded{};
  };
  struct PacketRoute {
    uint32_t voice = 0;
    uint32_t completedPtr = 0;
    uint32_t statusPtr = 0;
    uint32_t packetContext = 0;
    uint32_t data = 0;
    uint32_t bytes = 0;
  };
  struct PendingCallback {
    uint32_t voice = 0;
    uint32_t callback = 0;
    uint32_t context = 0;
    uint32_t packetContext = 0;
    uint32_t status = 0;
  };

  void Require(const X86Context& ctx, uint8_t* base) const;
  uint32_t ArenaAllocate(const X86Context& ctx, uint32_t size);
  void ArenaFree(uint32_t address);
  uint32_t ArenaFreeBytes() const;
  void ValidateVtable(uint32_t address) const;
  std::array<uint32_t, 8> ReadArgs(const X86Context& ctx, uint32_t count) const;
  audio::Format ReadFormat(const X86Context& ctx, uint32_t address, const char* owner) const;
  std::vector<std::pair<uint32_t, int32_t>> ReadMixBins(const X86Context& ctx, uint32_t address, uint32_t limit) const;
  uint32_t Handle(const X86Context& ctx, uint32_t id, uint64_t now, std::vector<PendingCallback>& deliver);
  uint32_t CreateBuffer(const X86Context& ctx, uint32_t descriptor, uint32_t output);
  uint32_t CreateStream(const X86Context& ctx, uint32_t descriptor, uint32_t output);
  uint32_t DownloadEffectsImage(const X86Context& ctx, const std::array<uint32_t, 8>& args);
  std::string ReadName(const X86Context& ctx, uint32_t address) const;
  bool I3dl2Source(const X86Context& ctx, VoiceState& state, uint32_t address) const;
  uint32_t StreamRelease(const X86Context& ctx, VoiceState& state, uint64_t now, std::vector<PendingCallback>& deliver);
  uint32_t BufferRelease(const X86Context& ctx, VoiceState& state);
  VoiceState* FindState(uint32_t obj, bool stream);
  void AdvanceAllLocked(const X86Context& ctx, uint64_t now);
  void AdvanceVoiceLocked(const X86Context& ctx, uint32_t obj, audio::Voice& voice, uint64_t now);
  void PublishLocked(const X86Context& ctx, std::vector<audio::Completion>& completions,
      std::vector<PendingCallback>* deliver, uint32_t deliverVoice);
  void QueueFlushCallbacksLocked(const X86Context& ctx, uint32_t obj, std::vector<PendingCallback>& deliver);
  void Deliver(X86Context& ctx, uint8_t* base, const std::vector<PendingCallback>& callbacks);
  bool SoundingLocked(uint64_t now) const;
  void FeedDispatchLocked(const X86Context& ctx);
  void FeedTickLocked(const X86Context& context, uint64_t now);
  void RenderLocked(const X86Context& context, uint64_t start, size_t frames);
  float ReadSample(const X86Context& context, VoiceState& state, const audio::Format& format,
    uint32_t data, uint32_t bytes, uint32_t frame, uint32_t channel);
  std::array<std::array<float, 6>, 2> Gains(const VoiceState& state, uint32_t channels) const;
  void Write32(const X86Context& ctx, uint32_t address, uint32_t value) const;
  [[noreturn]] void Unsupported(const X86Context& ctx, uint32_t id, const char* operation) const;

  Runtime& runtime_;
  PreparedImage& image_;
  Memory* memory_;
  std::shared_ptr<SilentAudioSink> sink_;
  std::shared_ptr<const kernel::DiscIndex> disc_;
  mutable std::mutex mutex_;
  std::vector<ArenaBlock> arena_;
  uint32_t device_ = 0;
  bool headphones_ = false;
  bool crossFeed_ = false;
  std::array<uint32_t, 3> listenerPosition_{};
  std::array<uint32_t, 3> listenerFront_{};
  std::array<uint32_t, 3> listenerTop_{};
  std::array<uint32_t, 12> listenerI3dl2_{};
  std::vector<uint32_t> downloads_;
  audio::VoiceBank bank_;
  std::map<uint32_t, VoiceState> states_;
  std::map<uint64_t, PacketRoute> tokens_;
  std::deque<PendingCallback> pending_;
  std::array<uint32_t, kMixBinCount> binHeadroom_{};
  std::vector<float> silence_;
  uint64_t nextToken_ = 0;
  uint32_t voiceLimitRejections_ = 0;
  std::atomic<uint64_t> published_ = 0;
  std::atomic<uint64_t> delivered_ = 0;
};

std::shared_ptr<AudioProvider> InstallAudioProvider(Runtime& runtime, PreparedImage& image,
  std::shared_ptr<SilentAudioSink> sink, std::shared_ptr<const kernel::DiscIndex> disc = {});

}
