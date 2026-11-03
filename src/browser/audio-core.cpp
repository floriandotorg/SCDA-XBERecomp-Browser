#include "browser/audio-core.h"

#include <algorithm>

namespace browser::audio {

namespace {

std::optional<uint64_t> AdvanceFrames(uint64_t delta, uint32_t rate) {
  const uint64_t whole = delta / kRate;
  const uint64_t rem = delta % kRate;
  if (whole > UINT64_MAX / rate) { return std::nullopt; }
  const uint64_t frames = whole * rate;
  const uint64_t extra = (rem * rate) / kRate;
  if (frames > UINT64_MAX - extra) { return std::nullopt; }
  return frames + extra;
}

bool ReachesNumerator(uint64_t base, uint64_t delta, uint32_t rate, uint64_t limit) {
  if (base >= limit) { return true; }
  const uint64_t need = limit - base;
  return delta >= (need + rate - 1) / rate;
}

uint64_t LoopWrapFrames(uint64_t base, uint64_t delta, uint32_t rate, uint32_t start, uint32_t end) {
  const uint64_t period = end - start;
  const uint64_t q = delta / kRate;
  const uint64_t r = delta % kRate;
  const uint64_t s = base % kRate + r * rate;
  const uint64_t totalMod =
      ((base / kRate) % period + (q % period) * (rate % period) + (s / kRate) % period) % period;
  return start + (period + totalMod - start % period) % period;
}

}  // namespace

std::optional<Format> Format::Make(uint32_t tag, uint32_t channels, uint32_t rate, uint32_t align, uint32_t bits) {
  if (!channels || channels > kMaxChannels || !rate || rate > kMaxSampleRate) { return std::nullopt; }
  uint32_t expected;
  if (tag == kFormatPcm) {
    if (bits != 8 && bits != 16) { return std::nullopt; }
    expected = channels * (bits / 8);
  } else if (tag == kFormatXboxAdpcm) {
    if (bits != 4) { return std::nullopt; }
    expected = channels * kAdpcmBlockBytesPerChannel;
  } else {
    return std::nullopt;
  }
  if (align != expected) { return std::nullopt; }
  Format format;
  format.tag = uint16_t(tag);
  format.channels = uint16_t(channels);
  format.align = uint16_t(align);
  format.bits = uint16_t(bits);
  format.rate = rate;
  return format;
}

std::optional<uint32_t> FramesOf(const Format& format, uint64_t bytes) {
  uint64_t frames;
  if (format.tag == kFormatXboxAdpcm) {
    frames = bytes / (uint64_t(kAdpcmBlockBytesPerChannel) * format.channels) * kAdpcmFramesPerBlock;
  } else {
    if (!format.align) { return std::nullopt; }
    frames = bytes / format.align;
  }
  if (frames > UINT32_MAX) { return std::nullopt; }
  return uint32_t(frames);
}

std::optional<uint32_t> BytesOf(const Format& format, uint32_t frames) {
  uint64_t bytes;
  if (format.tag == kFormatXboxAdpcm) {
    bytes = uint64_t(frames / kAdpcmFramesPerBlock) * kAdpcmBlockBytesPerChannel * format.channels;
  } else {
    bytes = uint64_t(frames) * format.align;
  }
  if (bytes > UINT32_MAX) { return std::nullopt; }
  return uint32_t(bytes);
}

Voice Voice::Buffer(const Format& format, uint32_t bytes) {
  Voice v;
  v.format_ = format;
  v.stream_ = false;
  v.bytes_ = bytes;
  v.loopStart_ = 0;
  v.loopLength_ = bytes;
  return v;
}

Voice Voice::Stream(const Format& format, uint32_t maxPackets) {
  Voice v;
  v.format_ = format;
  v.stream_ = true;
  v.maxPackets_ = std::max(maxPackets, 1u);
  return v;
}

bool Voice::IsStream() const { return stream_; }
const Format& Voice::GetFormat() const { return format_; }
uint32_t Voice::BufferBytes() const { return bytes_; }
uint32_t Voice::MaxPackets() const { return maxPackets_; }
uint32_t Voice::QueuedPackets() const { return uint32_t(packets_.size()) - packetHead_; }
uint32_t Voice::Frequency() const { return frequency_; }
uint32_t Voice::RetainedRecords() const { return uint32_t(packets_.size()) + uint32_t(pending_.size()); }
bool Voice::HasUndeliveredCompletions() const { return !pending_.empty() || QueuedPackets() != 0; }
bool Voice::Active() const { return active_; }
bool Voice::Paused() const { return paused_; }
bool Voice::Ending() const { return ending_; }

uint32_t Voice::EffectiveRate() const { return frequency_ ? frequency_ : format_.rate; }

bool Voice::Advancing() const { return stream_ ? (active_ && !paused_) : playing_; }

uint32_t Voice::EndFrames() const {
  if (looping_ && loopLength_) {
    return FramesOf(format_, uint64_t(loopStart_) + loopLength_).value_or(0);
  }
  return FramesOf(format_, bytes_).value_or(0);
}

uint32_t Voice::LoopStartFrames() const { return loopLength_ ? FramesOf(format_, loopStart_).value_or(0) : 0; }

uint64_t Voice::StreamQueuedFrames() const {
  uint64_t frames = 0;
  for (uint32_t n = packetHead_; n < packets_.size(); ++n) { frames += packets_[n].frames; }
  return frames;
}

void Voice::AnchorProgress(uint64_t now) {
  if (now <= anchor_) { return; }
  const uint64_t delta = now - anchor_;
  anchor_ = now;
  if (!Advancing()) { return; }
  const uint32_t rate = EffectiveRate();
  const uint64_t q = delta / kRate;
  const uint64_t r = delta % kRate;
  const uint64_t s = baseNumerator_ % kRate + r * rate;
  if (stream_) {
    const uint64_t limit = StreamQueuedFrames() * kRate;
    if (ReachesNumerator(baseNumerator_, delta, rate, limit)) {
      while (packetHead_ < packets_.size()) {
        Packet& packet = packets_[packetHead_];
        packet.used = packet.bytes;
        pending_.push_back({0, packet.token, packet.bytes, 0});
        ++packetHead_;
      }
      packets_.clear();
      packetHead_ = 0;
      baseNumerator_ = 0;
      if (ending_) { Deactivate(); }
      return;
    }
    uint64_t pos = baseNumerator_ / kRate + q * rate + s / kRate;
    while (packetHead_ < packets_.size() && pos >= packets_[packetHead_].frames) {
      Packet& packet = packets_[packetHead_];
      packet.used = packet.bytes;
      pending_.push_back({0, packet.token, packet.bytes, 0});
      pos -= packet.frames;
      ++packetHead_;
    }
    baseNumerator_ = pos * kRate + s % kRate;
    return;
  }
  const uint32_t end = EndFrames();
  const uint64_t limit = uint64_t(end) * kRate;
  if (looping_) {
    const uint32_t start = LoopStartFrames();
    if (end <= start) {
      playing_ = false;
      baseNumerator_ = 0;
      return;
    }
    if (!ReachesNumerator(baseNumerator_, delta, rate, limit)) {
      baseNumerator_ += delta * rate;
      return;
    }
    baseNumerator_ = LoopWrapFrames(baseNumerator_, delta, rate, start, end) * kRate + s % kRate;
    return;
  }
  if (ReachesNumerator(baseNumerator_, delta, rate, limit)) {
    baseNumerator_ = limit;
    return;
  }
  baseNumerator_ += delta * rate;
}

void Voice::DrainPending(std::vector<Completion>& out) {
  out.insert(out.end(), pending_.begin(), pending_.end());
  pending_.clear();
}

void Voice::Deactivate() {
  active_ = false;
  paused_ = false;
  ending_ = false;
}

AudioError Voice::Play(uint64_t now, uint32_t flags) {
  if (stream_) { return AudioError::kInvalidState; }
  const bool looping = (flags & kPlayLooping) != 0;
  uint32_t end;
  if (looping && loopLength_) {
    end = FramesOf(format_, uint64_t(loopStart_) + loopLength_).value_or(0);
  } else {
    end = FramesOf(format_, bytes_).value_or(0);
  }
  if (!end) { return AudioError::kEmptyBuffer; }
  AnchorProgress(now);
  playing_ = true;
  looping_ = looping;
  anchor_ = now;
  if (flags & kPlayFromStart) { baseNumerator_ = 0; }
  return AudioError::kNone;
}

AudioError Voice::Stop(uint64_t now) {
  if (stream_) { return AudioError::kInvalidState; }
  AnchorProgress(now);
  playing_ = false;
  return AudioError::kNone;
}

AudioError Voice::Pause(uint64_t now, bool paused) {
  if (!stream_) { return AudioError::kInvalidState; }
  if (paused == paused_) { return AudioError::kNone; }
  AnchorProgress(now);
  paused_ = paused;
  return AudioError::kNone;
}

AudioError Voice::SetFrequency(uint64_t now, uint32_t frequency) {
  if (frequency > kMaxSampleRate) { return AudioError::kInvalidFormat; }
  AnchorProgress(now);
  frequency_ = frequency;
  return AudioError::kNone;
}

AudioError Voice::SetBufferLength(uint64_t now, uint32_t bytes) {
  bytes_ = bytes;
  loopStart_ = 0;
  loopLength_ = bytes;
  baseNumerator_ = 0;
  anchor_ = now;
  return AudioError::kNone;
}

AudioError Voice::SetLoopRegion(uint32_t offsetBytes, uint32_t lengthBytes) {
  if (offsetBytes > bytes_ || lengthBytes > bytes_ - offsetBytes) { return AudioError::kInvalidLoop; }
  const uint32_t resolved = lengthBytes ? lengthBytes : bytes_ - offsetBytes;
  const uint32_t start = FramesOf(format_, offsetBytes).value_or(0);
  const uint32_t end = FramesOf(format_, uint64_t(offsetBytes) + resolved).value_or(0);
  if (end <= start) { return AudioError::kInvalidLoop; }
  loopStart_ = offsetBytes;
  loopLength_ = resolved;
  return AudioError::kNone;
}

AudioError Voice::SetCurrentPosition(uint64_t now, uint32_t bytes) {
  if (stream_) { return AudioError::kInvalidState; }
  if (bytes > bytes_) { return AudioError::kInvalidPosition; }
  AnchorProgress(now);
  baseNumerator_ = uint64_t(FramesOf(format_, bytes).value_or(0)) * kRate;
  anchor_ = now;
  return AudioError::kNone;
}

AudioError Voice::ProcessPacket(uint64_t now, uint64_t token, uint32_t bytes) {
  if (!stream_) { return AudioError::kInvalidState; }
  if (bytes % format_.align) { return AudioError::kInvalidFormat; }
  const std::optional<uint32_t> frames = FramesOf(format_, bytes);
  if (!frames) { return AudioError::kOverflow; }
  AnchorProgress(now);
  if (packetHead_ == packets_.size()) {
    packets_.clear();
    packetHead_ = 0;
  } else if (packetHead_) {
    packets_.erase(packets_.begin(), packets_.begin() + packetHead_);
    packetHead_ = 0;
  }
  if (packets_.size() + pending_.size() >= kMaxPacketsPerVoice) { return AudioError::kPacketLimit; }
  packets_.push_back({token, bytes, *frames, 0});
  active_ = true;
  ending_ = false;
  return AudioError::kNone;
}

void Voice::Discontinuity(uint64_t now, std::vector<Completion>& out) {
  if (!stream_) { return; }
  AnchorProgress(now);
  if (QueuedPackets()) {
    ending_ = true;
    DrainPending(out);
  } else if (active_) {
    Flush(out);
  } else {
    DrainPending(out);
  }
}

void Voice::Flush(std::vector<Completion>& out) {
  DrainPending(out);
  for (uint32_t n = packetHead_; n < packets_.size(); ++n) {
    const Packet& packet = packets_[n];
    out.push_back({0, packet.token, packet.used, kPacketFlushed});
  }
  packets_.clear();
  packetHead_ = 0;
  baseNumerator_ = 0;
  Deactivate();
}

bool Voice::FlushEx(bool onlyIfActive, std::vector<Completion>& out) {
  if (onlyIfActive && !active_) { return false; }
  Flush(out);
  return true;
}

void Voice::Release(std::vector<Completion>& out) { Flush(out); }

void Voice::Advance(uint64_t now, std::vector<Completion>& out) {
  AnchorProgress(now);
  if (!stream_ && playing_ && !looping_) {
    const uint32_t end = EndFrames();
    if (!end || baseNumerator_ / kRate >= end) {
      playing_ = false;
      baseNumerator_ = 0;
      anchor_ = now;
    }
  }
  DrainPending(out);
}

uint32_t Voice::Status(uint64_t now) const {
  if (stream_) { return 0; }
  bool playing = playing_;
  if (playing) {
    const uint32_t end = EndFrames();
    const uint64_t delta = Advancing() && now > anchor_ ? now - anchor_ : 0;
    if (!end || (!looping_ && ReachesNumerator(baseNumerator_, delta, EffectiveRate(), uint64_t(end) * kRate))) {
      playing = false;
    }
  }
  return (playing ? kStatusPlaying : 0) | (playing && looping_ ? kStatusLooping : 0);
}

bool Voice::StreamReady() const {
  const uint32_t queued = QueuedPackets();
  return queued < maxPackets_ && queued + pending_.size() < kMaxPacketsPerVoice;
}

StreamInfo Voice::GetStreamInfo() const {
  StreamInfo info;
  info.packetSize = format_.tag == kFormatXboxAdpcm ? kAdpcmBlockBytesPerChannel * format_.channels : format_.align;
  return info;
}

uint32_t Voice::PlayFrames(uint64_t now) const {
  const uint64_t delta = Advancing() && now > anchor_ ? now - anchor_ : 0;
  const uint32_t rate = EffectiveRate();
  if (stream_) {
    const uint64_t limit = StreamQueuedFrames() * kRate;
    if (ReachesNumerator(baseNumerator_, delta, rate, limit)) { return 0; }
    const std::optional<uint64_t> advance = AdvanceFrames(delta, rate);
    if (!advance) { return 0; }
    uint64_t pos = baseNumerator_ / kRate + *advance;
    for (uint32_t n = packetHead_; n < packets_.size() && pos >= packets_[n].frames; ++n) { pos -= packets_[n].frames; }
    return uint32_t(pos);
  }
  const uint32_t end = EndFrames();
  if (!end) { return 0; }
  if (looping_) {
    const uint32_t start = LoopStartFrames();
    if (end <= start) { return 0; }
    if (!ReachesNumerator(baseNumerator_, delta, rate, uint64_t(end) * kRate)) {
      return uint32_t(baseNumerator_ / kRate + AdvanceFrames(delta, rate).value_or(0));
    }
    return uint32_t(LoopWrapFrames(baseNumerator_, delta, rate, start, end));
  }
  if (ReachesNumerator(baseNumerator_, delta, rate, uint64_t(end) * kRate)) { return 0; }
  return uint32_t(baseNumerator_ / kRate + AdvanceFrames(delta, rate).value_or(0));
}

std::pair<uint32_t, uint32_t> Voice::CurrentPosition(uint64_t now) const {
  const uint32_t play = BytesOf(format_, PlayFrames(now)).value_or(0);
  const uint32_t ahead = BytesOf(format_, kAdpcmFramesPerBlock * 4).value_or(0);
  return {play, bytes_ ? (play + ahead) % bytes_ : 0};
}

AudioError Voice::Lock(uint32_t offsetBytes, uint32_t bytes, uint32_t flags, LockRange& out) const {
  if (!bytes_) { return AudioError::kEmptyBuffer; }
  uint32_t count = bytes;
  if (flags & kLockEntireBuffer) { count = bytes_; }
  const uint32_t offset = offsetBytes % bytes_;
  const uint32_t first = std::min(count, bytes_ - offset);
  out.firstOffset = offset;
  out.firstBytes = first;
  out.secondOffset = 0;
  out.secondBytes = count > first ? count - first : 0;
  return AudioError::kNone;
}

AudioError VoiceBank::CreateBuffer(uint32_t id, const Format& format, uint32_t bytes) {
  if (voices_.count(id)) { return AudioError::kDuplicateVoice; }
  if (voices_.size() >= kMaxVoices) { return AudioError::kVoiceLimit; }
  voices_.emplace(id, Voice::Buffer(format, bytes));
  return AudioError::kNone;
}

AudioError VoiceBank::CreateStream(uint32_t id, const Format& format, uint32_t maxPackets) {
  if (voices_.count(id)) { return AudioError::kDuplicateVoice; }
  if (voices_.size() >= kMaxVoices) { return AudioError::kVoiceLimit; }
  voices_.emplace(id, Voice::Stream(format, maxPackets));
  return AudioError::kNone;
}

AudioError VoiceBank::Erase(uint32_t id) {
  auto it = voices_.find(id);
  if (it == voices_.end()) { return AudioError::kUnknownVoice; }
  if (it->second.HasUndeliveredCompletions()) { return AudioError::kRetainedCompletions; }
  voices_.erase(it);
  return AudioError::kNone;
}

Voice* VoiceBank::Find(uint32_t id) {
  auto it = voices_.find(id);
  return it == voices_.end() ? nullptr : &it->second;
}

const Voice* VoiceBank::Find(uint32_t id) const {
  auto it = voices_.find(id);
  return it == voices_.end() ? nullptr : &it->second;
}

uint32_t VoiceBank::Count() const { return uint32_t(voices_.size()); }

std::vector<uint32_t> VoiceBank::Ids() const {
  std::vector<uint32_t> ids;
  ids.reserve(voices_.size());
  for (const auto& [id, voice] : voices_) { ids.push_back(id); }
  return ids;
}

void VoiceBank::AdvanceAll(uint64_t now, std::vector<Completion>& out) {
  for (auto& [id, voice] : voices_) {
    const size_t first = out.size();
    voice.Advance(now, out);
    for (size_t n = first; n < out.size(); ++n) { out[n].voice = id; }
  }
}

}
