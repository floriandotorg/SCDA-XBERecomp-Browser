#include "browser/silent-audio.h"
#include <algorithm>

namespace browser {

SilentAudioSink::SilentAudioSink(Runtime& runtime, PreparedImage& image)
  : runtime_(runtime), image_(image), memory_(image.memory.get()) {
  runtime_.Checkpoint({});
  if (!memory_ || image.importsBound || image.boundRuntime || runtime.HasPublishedImage() || runtime.ActiveWorkers() ||
      (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) || Memory::HasBorrow()) {
    throw Fault({}, 0, 0, "silent audio requires its unpublished stopped owned image without a backing lease");
  }
  samples_.resize(size_t(kCapacityFrames) * audio::kChannels);
}

void SilentAudioSink::RequireWorker(const X86Context& context) const {
  if (context.host != &runtime_ || !runtime_.OwnsCurrentContext(context) || !image_.importsBound ||
      image_.boundRuntime != &runtime_ || image_.memory.get() != memory_ || !runtime_.HasPublishedImage() || Memory::HasBorrow()) {
    throw Fault(context, 0, 0, "silent audio requires its bound current image worker without a backing lease");
  }
  runtime_.Checkpoint(context);
}

bool SilentAudioSink::BelongsTo(Runtime& runtime, PreparedImage& image) const {
  return &runtime == &runtime_ && &image == &image_ && image.memory.get() == memory_;
}

void SilentAudioSink::Submit(const X86Context& context, std::span<const float> samples) {
  RequireWorker(context);
  SubmitAtTime(context, runtime_.TimerTime(context), samples);
}

void SilentAudioSink::SubmitAtTime(const X86Context& context, uint64_t now, std::span<const float> samples) {
  RequireWorker(context);
  bool consumed = false;
  {
    std::lock_guard lock(lock_);
    const uint64_t frames = samples.size() / audio::kChannels;
    if (samples.size() % audio::kChannels || frames > kCapacityFrames || frames > UINT64_MAX - state_.submittedFrames) {
      throw Fault(context, 0, 0, "silent audio requires whole interleaved frames within its bounded queue and counters");
    }
    auto state = state_;
    auto clock = clock_;
    consumed = AdvanceState(state, clock, now);
    if (frames > kCapacityFrames - state.queuedFrames) {
      throw Fault(context, 0, 0, "silent audio queue capacity exhausted");
    }
    runtime_.Checkpoint(context);
    if (frames) {
      const size_t start = size_t(state.submittedFrames % kCapacityFrames) * audio::kChannels;
      const size_t first = std::min(samples.size(), samples_.size() - start);
      std::copy_n(samples.begin(), first, samples_.begin() + start);
      std::copy(samples.begin() + first, samples.end(), samples_.begin());
      if (!state.queuedFrames) {
        clock.last = now;
        clock.fraction = 0;
      }
      state.submittedFrames += frames;
      state.queuedFrames += frames;
    }
    state_ = state;
    clock_ = clock;
  }
  if (consumed) { runtime_.Wake(); }
}

bool SilentAudioSink::AdvanceState(SilentAudioSnapshot& state, ClockState& clock, uint64_t now100ns) {
  if (clock.last && now100ns < *clock.last) { return false; }
  const uint64_t elapsed = clock.last ? now100ns - *clock.last : 0;
  clock.last = now100ns;
  if (!state.queuedFrames) {
    clock.fraction = 0;
    return false;
  }
  if (state.paused) { return false; }
  constexpr uint64_t unitsPerSecond = 10000000;
  uint64_t frames = state.queuedFrames;
  if (elapsed < unitsPerSecond) {
    const uint64_t scaled = elapsed * audio::kRate + clock.fraction;
    frames = std::min(frames, scaled / unitsPerSecond);
    clock.fraction = scaled % unitsPerSecond;
  }
  state.queuedFrames -= frames;
  state.consumedFrames += frames;
  if (!state.queuedFrames) { clock.fraction = 0; }
  return frames != 0;
}

void SilentAudioSink::Advance(const X86Context& context, uint64_t now100ns) {
  RequireWorker(context);
  bool consumed = false;
  {
    std::lock_guard lock(lock_);
    runtime_.Checkpoint(context);
    consumed = AdvanceState(state_, clock_, now100ns);
  }
  if (consumed) { runtime_.Wake(); }
}

void SilentAudioSink::Pause(const X86Context& context, bool paused) {
  RequireWorker(context);
  bool consumed = false;
  {
    std::lock_guard lock(lock_);
    const uint64_t now = runtime_.TimerTime(context);
    runtime_.Checkpoint(context);
    consumed = AdvanceState(state_, clock_, now);
    state_.paused = paused;
  }
  if (consumed) { runtime_.Wake(); }
}

SilentAudioSnapshot SilentAudioSink::Snapshot() const {
  std::lock_guard lock(lock_);
  return state_;
}

}
