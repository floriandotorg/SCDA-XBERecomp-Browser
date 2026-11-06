#include "browser/silent-audio.h"
#include <algorithm>

namespace browser {

void AudioOutput::Reset() {
  header[3].store(0, std::memory_order_release);
  header[0].store(0, std::memory_order_release);
  header[1].store(0, std::memory_order_release);
  header[2].fetch_add(1, std::memory_order_acq_rel);
}

bool AudioOutput::Submit(std::span<const float> source) {
  if (!header[3].load(std::memory_order_acquire)) { return false; }
  const uint32_t write = header[0].load(std::memory_order_relaxed);
  const uint32_t read = header[1].load(std::memory_order_acquire);
  const size_t frames = source.size() / audio::kChannels;
  const uint32_t queued = write - read;
  if (source.size() % audio::kChannels || queued > kFrames || frames > kFrames - queued) {
    header[4].fetch_add(uint32_t(frames), std::memory_order_relaxed);
    return false;
  }
  const size_t offset = size_t(write % kFrames) * audio::kChannels;
  const size_t first = std::min(source.size(), samples.size() - offset);
  std::copy_n(source.begin(), first, samples.begin() + offset);
  std::copy(source.begin() + first, source.end(), samples.begin());
  header[0].store(write + uint32_t(frames), std::memory_order_release);
  return true;
}

bool AudioOutput::Drained() const {
  return header[3].load(std::memory_order_acquire) && header[0].load(std::memory_order_relaxed) == header[1].load(std::memory_order_acquire);
}

SilentAudioSink::SilentAudioSink(Runtime& runtime, PreparedImage& image, std::shared_ptr<AudioOutput> output)
  : runtime_(runtime), image_(image), memory_(image.memory.get()), output_(std::move(output)) {
  runtime_.Checkpoint({});
  if (!memory_ || image.importsBound || image.boundRuntime || runtime.HasPublishedImage() || runtime.ActiveWorkers() ||
      (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) || Memory::HasBorrow()) {
    throw Fault({}, 0, 0, "silent audio requires its unpublished stopped owned image without a backing lease");
  }
  samples_.resize(size_t(kCapacityFrames) * audio::kChannels);
  if (output_) { output_->Reset(); }
}

bool SilentAudioSink::NeedsSamples() const { return output_ && output_->header[3].load(std::memory_order_acquire); }

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
    const uint64_t pending = state.queuedFrames, pendingStart = state.submittedFrames - pending;
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
    if (output_ && frames && pending >= kRefillFrames && output_->Drained()) {
      const size_t start = size_t(pendingStart % kCapacityFrames) * audio::kChannels, count = size_t(pending) * audio::kChannels;
      const size_t first = std::min(count, samples_.size() - start);
      output_->Submit(std::span(samples_).subspan(start, first));
      output_->Submit(std::span(samples_).first(count - first));
    }
    if (output_) { output_->Submit(samples); }
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
