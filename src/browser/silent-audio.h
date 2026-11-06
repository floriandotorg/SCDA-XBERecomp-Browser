#pragma once
#include "audio/output.h"
#include "browser/image.h"

namespace browser {

struct AudioOutput {
  static constexpr uint32_t kFrames = 16384;
  std::array<std::atomic<uint32_t>, 8> header{};
  std::array<float, kFrames * ::audio::kChannels> samples{};
  void Reset();
  bool Submit(std::span<const float> source);
  bool Drained() const;
};

struct SilentAudioSnapshot {
  uint64_t submittedFrames = 0;
  uint64_t consumedFrames = 0;
  uint64_t queuedFrames = 0;
  bool paused = false;
  bool operator==(const SilentAudioSnapshot&) const = default;
};

class SilentAudioSink {
  Runtime& runtime_;
  PreparedImage& image_;
  Memory* const memory_;
  mutable std::mutex lock_;
  std::vector<float> samples_;
  std::shared_ptr<AudioOutput> output_;
  SilentAudioSnapshot state_;
  struct ClockState {
    std::optional<uint64_t> last;
    uint64_t fraction = 0;
  } clock_;
  void RequireWorker(const X86Context& context) const;
  static bool AdvanceState(SilentAudioSnapshot& state, ClockState& clock, uint64_t now100ns);
public:
  static constexpr uint32_t kCapacityFrames = audio::kRate;
  // A browser ring the page emptied (a focus change or the first activation) while the guest clock still has this
  // many frames to play gets them again, so it keeps the queue's cushion instead of playing just in time.
  static constexpr uint32_t kRefillFrames = 1024;
  SilentAudioSink(Runtime& runtime, PreparedImage& image, std::shared_ptr<AudioOutput> output = {});
  bool NeedsSamples() const;
  bool HasOutput() const { return bool(output_); }
  bool BelongsTo(Runtime& runtime, PreparedImage& image) const;
  void Submit(const X86Context& context, std::span<const float> samples);
  // Submit with an externally supplied timer sample: the only safe feed path from inside the timer clock
  // observer, where sampling TimerTime again would recurse into the driving queue.
  void SubmitAtTime(const X86Context& context, uint64_t now100ns, std::span<const float> samples);
  void Advance(const X86Context& context, uint64_t now100ns);
  void Pause(const X86Context& context, bool paused);
  SilentAudioSnapshot Snapshot() const;
};

}
