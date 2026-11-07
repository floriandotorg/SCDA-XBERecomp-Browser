#include "browser/audio-provider.h"
#include <bit>
#include <cmath>
#include "browser/audio-effects.h"
#include "browser/audio-entries.h"
#include "kernel/disc_index.h"
#include "cpu/x86_ops.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

BROWSER_AUDIO_ENTRIES(BROWSER_AUDIO_PRESENT)
BROWSER_AUDIO_ABSENT_PLACEHOLDERS

#define BROWSER_AUDIO_WRAPPER(name, id, args) \
  X86_FUNC(BROWSER_AUDIO_ORIGINAL(name)); \
  X86_FUNC(BROWSER_AUDIO_GUEST(name)) { \
    if (browser::Runtime::ObserveSdkEntry(ctx, base, id)) { \
      throw browser::Fault(ctx, BROWSER_AUDIO_ADDRESS(name), 0, "audio entry delegated to the retained hardware body"); \
    } \
  }
BROWSER_AUDIO_ENTRIES(BROWSER_AUDIO_WRAPPER)

namespace browser {
namespace {

constexpr size_t kArenaAllocationLimit = 4096;
constexpr uint32_t kFlag3d = 0x10;
constexpr uint32_t kDefaultHeadroomFlags = 0x382010;
constexpr uint32_t kDefaultHeadroomMb = 600;

std::optional<std::string> EffectsDiscPath(std::string path) {
  std::replace(path.begin(), path.end(), '/', '\\');
  std::string upper = path;
  for (char& character : upper) {
    if (character >= 'a' && character <= 'z') { character = char(character - 'a' + 'A'); }
  }
  if (upper.starts_with("\\??\\")) { path.erase(0, 4); upper.erase(0, 4); }
  constexpr std::string_view device = "\\DEVICE\\CDROM0\\";
  if (upper.starts_with(device)) {
    path.erase(0, device.size());
  } else if (upper.starts_with("D:\\")) {
    path.erase(0, 3);
  } else if (path.starts_with('\\') || path.find(':') != std::string::npos) {
    return std::nullopt;
  }
  if (path.find(':') != std::string::npos) { return std::nullopt; }
  return path;
}

struct EntryInfo {
  uint32_t address;
  uint32_t arguments;
  const char* name;
};

#define BROWSER_AUDIO_INFO(name, id, args) { BROWSER_AUDIO_ADDRESS(name), args, #name },
constexpr EntryInfo kEntries[] = { BROWSER_AUDIO_ENTRIES(BROWSER_AUDIO_INFO) };
static_assert(std::size(kEntries) == BROWSER_AUDIO_ENTRY_COUNT);

const EntryInfo& InfoFor(uint32_t id) {
  return kEntries[id - BROWSER_AUDIO_FIRST_ID];
}

}

AudioProvider::AudioProvider(Runtime& runtime, PreparedImage& image, std::shared_ptr<SilentAudioSink> sink,
    std::shared_ptr<const kernel::DiscIndex> disc)
  : runtime_(runtime), image_(image), memory_(image.memory.get()), sink_(std::move(sink)), disc_(std::move(disc)) {
  runtime_.Checkpoint({});
  if (!memory_ || !sink_ || image.importsBound || image.boundRuntime || runtime_.HasPublishedImage() || runtime_.ActiveWorkers() ||
      Memory::HasBorrow() || (image.diagnosticRuntime && image.diagnosticRuntime != &runtime_) ||
      !sink_->BelongsTo(runtime_, image)) {
    throw Fault({}, 0, 0, "audio provider requires its cold owned generated image and sink");
  }
  for (const EntryInfo& entry : kEntries) {
    if (runtime_.Find(entry.address) == nullptr) {
      throw Fault({}, entry.address, 0, "audio provider requires its mapped generated entry");
    }
  }
#define BROWSER_AUDIO_CHECK_STRONG(name, id, args) \
  if (runtime_.Find(BROWSER_AUDIO_ADDRESS(name)) != BROWSER_AUDIO_GUEST(name) || \
      BROWSER_AUDIO_GUEST(name) == BROWSER_AUDIO_ORIGINAL(name)) { \
    throw Fault({}, BROWSER_AUDIO_ADDRESS(name), 0, "audio provider requires its actual strong entry binding"); \
  }
  BROWSER_AUDIO_ENTRIES(BROWSER_AUDIO_CHECK_STRONG)
  ValidateVtable(BROWSER_AUDIO_ADDRESS(DSound_StreamVtable));
  ValidateVtable(BROWSER_AUDIO_ADDRESS(DSound_StreamVtable2));
  if (memory_->CommittedPages(kArenaBase, kArenaCapacity) || memory_->QueryPages(kArenaBase).mapped ||
      memory_->QueryPages(kArenaBase + kArenaCapacity - 4096).mapped) {
    throw Fault({}, kArenaBase, kArenaCapacity, "audio provider arena is already owned");
  }
  memory_->Map(kArenaBase, kArenaCapacity);
  memory_->Commit(kArenaBase, kArenaCapacity);
  arena_.reserve(kArenaAllocationLimit * 2 + 1);
  arena_.push_back({kArenaBase, kArenaCapacity, false});
  silence_.assign(size_t(kFeedChunkFrames) * ::audio::kChannels, 0.0f);
  image.diagnosticRuntime = &runtime_;
}

bool AudioProvider::BelongsTo(Runtime& runtime, PreparedImage& image) const {
  return &runtime == &runtime_ && &image == &image_ && image.memory.get() == memory_ &&
    sink_ && sink_->BelongsTo(runtime_, image_);
}

void AudioProvider::Require(const X86Context& ctx, uint8_t* base) const {
  if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ ||
      base != memory_->Base() || !runtime_.OwnsCurrentContext(ctx) || Memory::HasBorrow()) {
    throw Fault(ctx, ctx.esp, 0, "audio provider requires its bound current image worker without a lease");
  }
  runtime_.Checkpoint(ctx);
}

void AudioProvider::ValidateVtable(uint32_t address) const {
  std::array<uint32_t, BROWSER_AUDIO_VTABLE_SLOTS> slots{};
  memory_->Read({}, address, slots.data(), sizeof(slots));
  for (const uint32_t target : slots) {
    if (!runtime_.Find(target)) {
      throw Fault({}, address, sizeof(slots), "retained stream vtable slot resolves to unmapped code");
    }
  }
}

uint32_t AudioProvider::ArenaAllocate(const X86Context& ctx, uint32_t size) {
  if (!size || size > kArenaCapacity) { throw Fault(ctx, kArenaBase, size, "audio arena allocation size invalid"); }
  const uint32_t rounded = (size + 15) & ~15u;
  for (size_t n = 0; n < arena_.size(); ++n) {
    ArenaBlock& block = arena_[n];
    if (block.allocated || block.size < rounded) { continue; }
    if (arena_.size() >= kArenaAllocationLimit * 2 + 1 && block.size != rounded) {
      throw Fault(ctx, kArenaBase, kArenaCapacity, "audio arena block budget exhausted");
    }
    const uint32_t address = block.address;
    if (block.size == rounded) {
      block.allocated = true;
    } else {
      arena_.insert(arena_.begin() + ptrdiff_t(n) + 1, ArenaBlock{block.address + rounded, block.size - rounded, false});
      block.size = rounded;
      block.allocated = true;
    }
    const std::array<uint8_t, 16> zeros{};
    for (uint32_t left = rounded, at = address; left; at += 16, left -= 16) {
      memory_->Write(ctx, at, zeros.data(), std::min<uint32_t>(left, 16));
    }
    return address;
  }
  throw Fault(ctx, kArenaBase, kArenaCapacity, "audio arena capacity exhausted");
}

void AudioProvider::ArenaFree(uint32_t address) {
  if (!address) { return; }
  for (size_t n = 0; n < arena_.size(); ++n) {
    if (arena_[n].address != address || !arena_[n].allocated) { continue; }
    arena_[n].allocated = false;
    if (n + 1 < arena_.size() && !arena_[n + 1].allocated) {
      arena_[n].size += arena_[n + 1].size;
      arena_.erase(arena_.begin() + ptrdiff_t(n) + 1);
    }
    if (n && !arena_[n - 1].allocated) {
      arena_[n - 1].size += arena_[n].size;
      arena_.erase(arena_.begin() + ptrdiff_t(n));
    }
    return;
  }
}

uint32_t AudioProvider::ArenaFreeBytes() const {
  uint32_t total = 0;
  for (const ArenaBlock& block : arena_) {
    if (!block.allocated) { total += block.size; }
  }
  return total;
}

void AudioProvider::Write32(const X86Context& ctx, uint32_t address, uint32_t value) const {
  memory_->Write(ctx, address, &value, sizeof(value));
}

std::array<uint32_t, 8> AudioProvider::ReadArgs(const X86Context& ctx, uint32_t count) const {
  std::array<uint32_t, 8> args{};
  if (count) { memory_->Read(ctx, ctx.esp + 4, args.data(), count * 4); }
  return args;
}

audio::Format AudioProvider::ReadFormat(const X86Context& ctx, uint32_t address, const char* owner) const {
  char reason[80];
  if (!address) {
    std::snprintf(reason, sizeof(reason), "%s requires an explicit wave format", owner);
    throw Fault(ctx, ctx.esp, 0, reason);
  }
  std::array<uint8_t, 16> wave{};
  memory_->Read(ctx, address, wave.data(), wave.size());
  const auto field = [&](uint32_t offset) {
    return uint32_t(wave[offset]) | uint32_t(wave[offset + 1]) << 8;
  };
  const uint32_t rate = uint32_t(wave[4]) | uint32_t(wave[5]) << 8 | uint32_t(wave[6]) << 16 | uint32_t(wave[7]) << 24;
  auto format = audio::Format::Make(field(0), field(2), rate, field(12), field(14));
  if (!format) {
    std::snprintf(reason, sizeof(reason), "%s wave format is invalid or unsupported", owner);
    throw Fault(ctx, address, wave.size(), reason);
  }
  return *format;
}

std::vector<std::pair<uint32_t, int32_t>> AudioProvider::ReadMixBins(const X86Context& ctx, uint32_t address, uint32_t limit) const {
  std::vector<std::pair<uint32_t, int32_t>> bins;
  if (!address) { return bins; }
  std::array<uint32_t, 2> header{};
  memory_->Read(ctx, address, header.data(), sizeof(header));
  const uint32_t count = std::min(header[0], limit);
  if (count && !header[1]) {
    throw Fault(ctx, address, 8, "audio mix bin list has entries but no pairs");
  }
  if (count && uint64_t(header[1]) + uint64_t(count) * 8 > uint64_t(1) << 32) {
    throw Fault(ctx, address, 8, "audio mix bin pair range wraps the guest address space");
  }
  for (uint32_t n = 0; n < count; ++n) {
    std::array<uint32_t, 2> pair{};
    memory_->Read(ctx, header[1] + n * 8, pair.data(), sizeof(pair));
    bins.emplace_back(pair[0] & 31, int32_t(pair[1]));
  }
  return bins;
}

AudioProvider::VoiceState* AudioProvider::FindState(uint32_t obj, bool stream) {
  auto it = states_.find(obj);
  if (it == states_.end() || it->second.stream != stream || !bank_.Find(obj)) { return nullptr; }
  return &it->second;
}

void AudioProvider::AdvanceVoiceLocked(const X86Context& ctx, uint32_t obj, audio::Voice& voice, uint64_t now) {
  std::vector<audio::Completion> completions;
  voice.Advance(now, completions);
  for (audio::Completion& completion : completions) { completion.voice = obj; }
  PublishLocked(ctx, completions, nullptr, 0);
}

void AudioProvider::AdvanceAllLocked(const X86Context& ctx, uint64_t now) {
  std::vector<audio::Completion> completions;
  bank_.AdvanceAll(now, completions);
  PublishLocked(ctx, completions, nullptr, 0);
}

void AudioProvider::PublishLocked(const X86Context& ctx, std::vector<audio::Completion>& completions,
    std::vector<PendingCallback>* deliver, uint32_t deliverVoice) {
  for (const audio::Completion& completion : completions) {
    auto route = tokens_.find(completion.token);
    if (route == tokens_.end()) {
      throw Fault(ctx, ctx.esp, 0, "audio completion without its packet route");
    }
    const PacketRoute packet = route->second;
    tokens_.erase(route);
    const uint32_t obj = completion.voice ? completion.voice : packet.voice;
    if (packet.completedPtr) { Write32(ctx, packet.completedPtr, completion.completedBytes); }
    if (packet.statusPtr) { Write32(ctx, packet.statusPtr, completion.status); }
    ++published_;
    const auto state = states_.find(obj);
    if (state == states_.end() || !state->second.callback) { continue; }
    const PendingCallback callback{obj, state->second.callback, state->second.context, packet.packetContext, completion.status};
    if (deliver && deliverVoice == obj) {
      deliver->push_back(callback);
    } else {
      if (pending_.size() >= kMaxPendingCallbacks) {
        throw Fault(ctx, ctx.esp, 0, "audio pending callback budget exhausted");
      }
      pending_.push_back(callback);
    }
  }
}

void AudioProvider::QueueFlushCallbacksLocked(const X86Context& ctx, uint32_t obj, std::vector<PendingCallback>& deliver) {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->voice != obj) { ++it; continue; }
    deliver.push_back(*it);
    it = pending_.erase(it);
  }
}

void AudioProvider::Deliver(X86Context& ctx, uint8_t* base, const std::vector<PendingCallback>& callbacks) {
  for (const PendingCallback& callback : callbacks) {
    runtime_.Call(ctx, base, callback.callback, {callback.context, callback.packetContext, callback.status});
    ++delivered_;
  }
}

bool AudioProvider::SoundingLocked(uint64_t now) const { return bank_.Sounding(now); }

void AudioProvider::FeedDispatchLocked(const X86Context& ctx) {
  FeedTickLocked(ctx, runtime_.TimerTime(ctx));
}

void AudioProvider::FeedTickLocked(const X86Context& context, uint64_t now) {
  while (true) {
    const SilentAudioSnapshot snapshot = sink_->Snapshot();
    if (snapshot.paused) { return; }
    const uint32_t capacity = sink_->HasOutput() ? 8192 : SilentAudioSink::kCapacityFrames;
    const uint64_t space = capacity - std::min<uint64_t>(snapshot.queuedFrames, capacity);
    if (!space || !SoundingLocked(snapshot.consumedFrames)) { return; }
    const size_t frames = size_t(std::min<uint64_t>(space, kFeedChunkFrames));
    RenderLocked(context, snapshot.submittedFrames, frames);
    sink_->SubmitAtTime(context, now, std::span(silence_.data(), frames * ::audio::kChannels));
  }
}

float AudioProvider::ReadSample(const X86Context& context, VoiceState& state, const audio::Format& format,
    uint32_t data, uint32_t bytes, uint32_t frame, uint32_t channel) {
  if (!data || channel >= format.channels || frame >= audio::FramesOf(format, bytes).value_or(0)) { return 0; }
  const uint32_t blockFrames = format.tag == audio::kFormatXboxAdpcm ? 64 : 128;
  const uint32_t offset = format.tag == audio::kFormatXboxAdpcm ? frame / 64 * format.align : frame / 128 * 128 * format.align;
  const uint64_t address = uint64_t(data) + offset;
  const uint32_t count = std::min<uint32_t>(bytes - offset, format.tag == audio::kFormatXboxAdpcm ? format.align : 128 * format.align);
  if (address + count > (uint64_t(1) << 32) || count > state.cachedData.size()) { throw Fault(context, data, bytes, "audio sample block exceeds guest memory"); }
  if (state.cachedAddress != address || state.cachedBytes != count) {
    memory_->Read(context, uint32_t(address), state.cachedData.data(), count);
    if (format.tag == audio::kFormatXboxAdpcm && !audio::DecodeAdpcm(std::span(state.cachedData).first(count), format.channels, state.decoded)) {
      throw Fault(context, uint32_t(address), count, "audio ADPCM block is invalid");
    }
    state.cachedAddress = address;
    state.cachedBytes = count;
  }
  if (format.tag == audio::kFormatXboxAdpcm) { return state.decoded[(frame % blockFrames) * format.channels + channel] / 32768.0f; }
  const auto decoded = audio::DecodePcm(format, std::span(state.cachedData).subspan((frame % blockFrames) * format.align, format.align), channel);
  if (!decoded) { throw Fault(context, uint32_t(address), count, "audio PCM frame is invalid"); }
  return *decoded;
}

std::array<std::array<float, 6>, 2> AudioProvider::Gains(const VoiceState& state, uint32_t channels) const {
  std::array<std::array<float, 6>, 2> gains{};
  const auto db = [](int64_t value) { return value <= -10000 ? 0.0f : std::pow(10.0f, float(value) / 2000); };
  float attenuation = 1, pan = 0;
  if (state.is3d) {
    std::array<float, 3> delta{}, front{}, top{};
    for (uint32_t n = 0; n < 3; ++n) {
      delta[n] = std::bit_cast<float>(state.position[n]) - std::bit_cast<float>(listenerPosition_[n]);
      front[n] = std::bit_cast<float>(listenerFront_[n]);
      top[n] = std::bit_cast<float>(listenerTop_[n]);
    }
    const float distance = std::hypot(delta[0], delta[1], delta[2]);
    const float minimum = state.minDistance ? std::bit_cast<float>(state.minDistance) : 1;
    const float maximum = state.maxDistance ? std::bit_cast<float>(state.maxDistance) : 1e9f;
    if (!std::isfinite(distance) || !std::isfinite(minimum) || !std::isfinite(maximum) || minimum <= 0 || maximum < minimum) { return gains; }
    if (distance > minimum) { attenuation = minimum / std::min(distance, maximum); }
    const std::array<float, 3> right{top[1] * front[2] - top[2] * front[1], top[2] * front[0] - top[0] * front[2], top[0] * front[1] - top[1] * front[0]};
    const float length = std::hypot(right[0], right[1], right[2]);
    if (distance > 0 && length > 0) { pan = std::clamp((delta[0] * right[0] + delta[1] * right[1] + delta[2] * right[2]) / (distance * length), -1.0f, 1.0f); }
    if (!std::isfinite(pan)) { return gains; }
  }
  std::array<int32_t, 32> volumes{};
  for (const auto& [bin, volume] : state.bins) { volumes[bin] = volume; }
  for (const auto& [bin, volume] : state.binVolumes) { volumes[bin] = volume; }
  const size_t count = state.bins.empty() ? 2 : state.bins.size();
  bool speakers = false;
  for (uint32_t n = 0; n < count; ++n) {
    const uint32_t bin = state.bins.empty() ? n : state.bins[n].first;
    speakers |= bin < 10 && bin != 3 && volumes[bin] > -10000;
  }
  for (uint32_t n = 0; n < count; ++n) {
    const uint32_t channel = channels == 1 ? 0 : n % 2;
    const uint32_t bin = state.bins.empty() ? n : state.bins[n].first;
    const float value = attenuation * db(state.volumeMb) * db(-int64_t(state.headroomMb)) * db(volumes[bin]);
    const auto add = [&](uint32_t speaker) {
      const float spatial = state.is3d && channels == 1 && speaker != 2 && speaker != 3 ? std::sqrt(1 + ((speaker & 1) ? pan : -pan)) : 1;
      const float level = value * spatial;
      if (std::isfinite(level)) { gains[channel][speaker] = std::max(gains[channel][speaker], level); }
    };
    if (!speakers) { add(0); add(1); }
    else if (bin < 6) { add(bin); }
    else if (bin < 10) { add(bin - 6 + ((bin >= 8) ? 2 : 0)); }
  }
  return gains;
}

void AudioProvider::RenderLocked(const X86Context& context, uint64_t start, size_t frames) {
  std::fill_n(silence_.begin(), frames * ::audio::kChannels, 0.0f);
  if (!sink_->NeedsSamples()) { return; }
  auto lease = memory_->Borrow();
  for (auto& [id, state] : states_) {
    const auto* voice = bank_.Find(id);
    if (!voice) { continue; }
    if (voice->IsStream() ? !voice->Active() || voice->Paused() || !voice->QueuedPackets() : !(voice->Status(start) & audio::kStatusPlaying)) { continue; }
    const auto& format = voice->GetFormat();
    const auto gains = Gains(state, format.channels);
    state.cachedAddress = UINT64_MAX;
    for (uint32_t n = 0; n < frames; ++n) {
      const auto cursor = voice->Sample(start + n);
      if (!cursor) { continue; }
      uint32_t data = state.data, bytes = voice->BufferBytes();
      if (state.stream) {
        const auto packet = tokens_.find(cursor->token);
        if (packet == tokens_.end()) { throw Fault(context, id, 0, "audio mixer lost its stream packet"); }
        data = packet->second.data;
        bytes = packet->second.bytes;
      }
      const uint32_t next = std::min(cursor->frame + 1, audio::FramesOf(format, bytes).value_or(1) - 1);
      for (const uint32_t channel : {0u, 1u}) {
        if (channel >= format.channels) { continue; }
        const float first = ReadSample(context, state, format, data, bytes, cursor->frame, channel);
        const float second = ReadSample(context, state, format, data, bytes, next, channel);
        const float sample = first + (second - first) * (float(cursor->fraction) / audio::kRate);
        for (const uint32_t speaker : {0u, 1u, 2u, 3u, 4u, 5u}) { silence_[n * ::audio::kChannels + speaker] += sample * gains[channel][speaker]; }
      }
    }
  }
}

void AudioProvider::Tick(const X86Context& context, uint64_t now) {
  if (!BelongsTo(runtime_, image_) || !image_.importsBound || image_.boundRuntime != &runtime_ || Memory::HasBorrow()) {
    throw Fault(context, context.esp, 0, "audio clock tick requires its bound image session without a lease");
  }
  std::lock_guard lock(mutex_);
  FeedTickLocked(context, now);
  AdvanceAllLocked(context, sink_->Snapshot().consumedFrames);
}

AudioProvider::SnapshotData AudioProvider::Snapshot() const {
  std::lock_guard lock(mutex_);
  SnapshotData snapshot;
  snapshot.device = device_;
  snapshot.voices = bank_.Count();
  snapshot.voiceLimitRejections = voiceLimitRejections_;
  snapshot.arenaFreeBytes = ArenaFreeBytes();
  snapshot.publishedCompletions = published_.load();
  snapshot.deliveredCallbacks = delivered_.load();
  snapshot.pendingCallbacks = pending_.size();
  return snapshot;
}

[[noreturn]] void AudioProvider::Unsupported(const X86Context& ctx, uint32_t id, const char* operation) const {
  char reason[96];
  std::snprintf(reason, sizeof(reason), "unsupported DirectSound operation %s", operation);
  throw Fault(ctx, InfoFor(id).address, 0, reason);
}

uint32_t AudioProvider::CreateBuffer(const X86Context& ctx, uint32_t descriptor, uint32_t output) {
  if (!output) { throw Fault(ctx, ctx.esp, 0, "DirectSound buffer creation requires an output pointer"); }
  memory_->CheckRange(ctx, output, 4);
  std::array<uint32_t, 5> header{};
  memory_->Read(ctx, descriptor, header.data(), sizeof(header));
  const uint32_t flags = header[1], bytes = header[2], wave = header[3], mixbins = header[4];
  const audio::Format format = ReadFormat(ctx, wave, "DirectSound buffer");
  auto bins = ReadMixBins(ctx, mixbins, 8);
  if (bank_.Count() >= audio::kMaxVoices) {
    if (voiceLimitRejections_ != UINT32_MAX) { ++voiceLimitRejections_; }
    return audio::kGuestErrorFail;
  }
  const uint32_t obj = ArenaAllocate(ctx, kBufferObjectBytes);
  uint32_t owned = 0;
  try {
    if (bytes) { owned = ArenaAllocate(ctx, bytes); }
  } catch (...) {
    ArenaFree(obj);
    throw;
  }
  if (bank_.CreateBuffer(obj, format, bytes) != audio::AudioError::kNone) {
    ArenaFree(owned);
    ArenaFree(obj);
    throw Fault(ctx, ctx.esp, 0, "audio buffer voice creation failed");
  }
  VoiceState state;
  state.obj = obj;
  state.data = owned;
  state.owned = owned;
  state.is3d = (flags & kFlag3d) != 0;
  state.headroomMb = (flags & kDefaultHeadroomFlags) ? 0 : kDefaultHeadroomMb;
  state.bins = std::move(bins);
  try {
    states_.emplace(obj, std::move(state));
  } catch (...) {
    bank_.Erase(obj);
    ArenaFree(owned);
    ArenaFree(obj);
    throw;
  }
  Write32(ctx, output, obj);
  return 0;
}

uint32_t AudioProvider::CreateStream(const X86Context& ctx, uint32_t descriptor, uint32_t output) {
  if (!output) { throw Fault(ctx, ctx.esp, 0, "DirectSound stream creation requires an output pointer"); }
  memory_->CheckRange(ctx, output, 4);
  std::array<uint32_t, 6> header{};
  memory_->Read(ctx, descriptor, header.data(), sizeof(header));
  const uint32_t flags = header[0], maxPackets = header[1], wave = header[2], callback = header[3], context = header[4], mixbins = header[5];
  const audio::Format format = ReadFormat(ctx, wave, "DirectSound stream");
  auto bins = ReadMixBins(ctx, mixbins, 8);
  if (bank_.Count() >= audio::kMaxVoices) {
    if (voiceLimitRejections_ != UINT32_MAX) { ++voiceLimitRejections_; }
    return audio::kGuestErrorFail;
  }
  const uint32_t obj = ArenaAllocate(ctx, kStreamObjectBytes);
  if (bank_.CreateStream(obj, format, maxPackets) != audio::AudioError::kNone) {
    ArenaFree(obj);
    throw Fault(ctx, ctx.esp, 0, "audio stream voice creation failed");
  }
  Write32(ctx, obj, BROWSER_AUDIO_ADDRESS(DSound_StreamVtable));
  Write32(ctx, obj + 4, BROWSER_AUDIO_ADDRESS(DSound_StreamVtable2));
  VoiceState state;
  state.obj = obj;
  state.stream = true;
  state.callback = callback;
  state.context = context;
  state.is3d = (flags & kFlag3d) != 0;
  state.headroomMb = (flags & kDefaultHeadroomFlags) ? 0 : kDefaultHeadroomMb;
  state.bins = std::move(bins);
  try {
    states_.emplace(obj, std::move(state));
  } catch (...) {
    bank_.Erase(obj);
    ArenaFree(obj);
    throw;
  }
  Write32(ctx, output, obj);
  return 0;
}

std::string AudioProvider::ReadName(const X86Context& ctx, uint32_t address) const {
  if (!address) { return {}; }
  std::string name;
  for (uint32_t offset = 0; offset < 4096;) {
    const uint32_t at = address + offset;
    const uint32_t span = std::min<uint32_t>(64, 4096 - (at & 4095));
    std::array<char, 64> chunk{};
    memory_->Read(ctx, at, chunk.data(), span);
    for (uint32_t n = 0; n < span; ++n) {
      if (!chunk[n]) { return name; }
      name.push_back(chunk[n]);
    }
    offset += span;
  }
  throw Fault(ctx, address, 4096, "audio effects image name exceeds the bounded length");
}

bool AudioProvider::I3dl2Source(const X86Context& ctx, VoiceState& state, uint32_t address) const {
  if (!address) { return true; }
  memory_->CheckRange(ctx, address, 36);
  memory_->Read(ctx, address, state.i3dl2Source.data(), sizeof(state.i3dl2Source));
  return true;
}

uint32_t AudioProvider::DownloadEffectsImage(const X86Context& ctx, const std::array<uint32_t, 8>& args) {
  const std::string name = ReadName(ctx, args[0]);
  std::vector<uint8_t> source;
  if (args[2] & 1) {
    for (const xbe::Section& section : image_.image.sections) {
      if (section.name != name) { continue; }
      if (memory_->IsCommitted(section.va)) {
        source.resize(section.vsize);
        if (section.vsize) { memory_->Read(ctx, section.va, source.data(), section.vsize); }
      } else {
        source.assign(section.vsize, 0);
      }
      break;
    }
  } else if (disc_ && !name.empty()) {
    std::vector<uint8_t> file;
    const auto path = EffectsDiscPath(name);
    if (path && disc_->Load(*path, file)) { source = std::move(file); }
  }
  crossFeed_ = false;
  for (const xbe::Section& section : image_.image.sections) {
    if (section.name != "dsstdfx") { continue; }
    if (!source.empty() && source.size() <= section.vsize) {
      if (memory_->IsCommitted(section.va)) {
        std::vector<uint8_t> resident(source.size());
        memory_->Read(ctx, section.va, resident.data(), resident.size());
        crossFeed_ = resident == source;
      } else {
        crossFeed_ = std::ranges::all_of(source, [](uint8_t byte) { return byte == 0; });
      }
    }
    break;
  }
  const audio::EffectsPlanResult planned = audio::PlanEffectsImage(source, ArenaFreeBytes());
  if (!planned.Ok()) { throw Fault(ctx, args[0], 0, "audio effects image exceeds the arena budget"); }
  const audio::EffectsImagePlan& plan = planned.plan;
  const auto rounded = [](uint64_t bytes) { return (bytes + 15) & ~uint64_t(15); };
  uint64_t required = rounded(plan.descriptorBytes);
  for (const audio::EffectPlan& effect : plan.effects) {
    for (const audio::EffectSegmentPlan& segment : effect.segments) { required += rounded(segment.allocationSize); }
  }
  if (required > ArenaFreeBytes()) { throw Fault(ctx, args[0], 0, "audio effects image exceeds the rounded arena budget"); }
  std::vector<uint32_t> segments;
  segments.reserve(plan.effects.size() * audio::kEffectSegmentsPerEffect);
  uint32_t descriptor = 0;
  try {
    descriptor = ArenaAllocate(ctx, uint32_t(plan.descriptorBytes));
    for (const audio::EffectPlan& effect : plan.effects) {
      for (const audio::EffectSegmentPlan& segment : effect.segments) {
        segments.push_back(ArenaAllocate(ctx, uint32_t(segment.allocationSize)));
      }
    }
  } catch (...) {
    for (const uint32_t segment : segments) { ArenaFree(segment); }
    ArenaFree(descriptor);
    throw;
  }
  Write32(ctx, descriptor, plan.count);
  Write32(ctx, descriptor + 4, plan.flags);
  uint32_t slot = descriptor + 8;
  size_t index = 0;
  for (const audio::EffectPlan& effect : plan.effects) {
    for (const audio::EffectSegmentPlan& segment : effect.segments) {
      Write32(ctx, slot, segments[index++]);
      Write32(ctx, slot + 4, segment.declaredSize);
      slot += 8;
    }
  }
  downloads_.push_back(descriptor);
  if (args[3]) {
    memory_->CheckRange(ctx, args[3], 4);
    Write32(ctx, args[3], descriptor);
  }
  return 0;
}

uint32_t AudioProvider::StreamRelease(const X86Context& ctx, VoiceState& state, uint64_t now, std::vector<PendingCallback>& deliver) {
  if (--state.refs) { return state.refs; }
  audio::Voice* voice = bank_.Find(state.obj);
  std::vector<audio::Completion> completions;
  voice->Advance(now, completions);
  voice->Flush(completions);
  for (audio::Completion& completion : completions) { completion.voice = state.obj; }
  QueueFlushCallbacksLocked(ctx, state.obj, deliver);
  PublishLocked(ctx, completions, &deliver, state.obj);
  if (bank_.Erase(state.obj) != audio::AudioError::kNone) {
    throw Fault(ctx, state.obj, 0, "stream release retained undelivered completions");
  }
  ArenaFree(state.obj);
  states_.erase(state.obj);
  return 0;
}

uint32_t AudioProvider::BufferRelease(const X86Context& ctx, VoiceState& state) {
  if (--state.refs) { return state.refs; }
  if (bank_.Erase(state.obj) != audio::AudioError::kNone) {
    throw Fault(ctx, state.obj, 0, "buffer release retained undelivered completions");
  }
  ArenaFree(state.owned);
  ArenaFree(state.obj);
  states_.erase(state.obj);
  return 0;
}

uint32_t AudioProvider::Handle(const X86Context& ctx, uint32_t id, uint64_t now, std::vector<PendingCallback>& deliver) {
  const EntryInfo& entry = InfoFor(id);
  const auto args = ReadArgs(ctx, entry.arguments);
  const auto voiceArgs = [&](bool stream) -> std::pair<VoiceState*, audio::Voice*> {
    VoiceState* state = FindState(args[0], stream);
    return {state, state ? bank_.Find(args[0]) : nullptr};
  };
  const auto flushVoice = [&](uint32_t obj, audio::Voice& voice, std::vector<audio::Completion>& completions) {
    for (audio::Completion& completion : completions) { completion.voice = obj; }
    QueueFlushCallbacksLocked(ctx, obj, deliver);
    PublishLocked(ctx, completions, &deliver, obj);
  };
  switch (id) {
  case 72: return 0;
  case 75: return 0;
  case 89: {
    if (!args[1]) { throw Fault(ctx, ctx.esp, 0, "GetSpeakerConfig requires an output pointer"); }
    memory_->CheckRange(ctx, args[1], 4);
    Write32(ctx, args[1], speakerConfig_ ? speakerConfig_() : kColdSpeakerConfig);
    return 0;
  }
  case 90: return 0;
  case 91: {
    headphones_ = args[1] != 0;
    return 0;
  }
  case 92: {
    if (args[1] < kMixBinCount) { binHeadroom_[args[1]] = args[2] & 7; }
    return 0;
  }
  case 121: return 0;
  case 123: return 0;
  case 124: {
    listenerFront_ = {args[1], args[2], args[3]};
    listenerTop_ = {args[4], args[5], args[6]};
    return 0;
  }
  case 125: {
    listenerPosition_ = {args[1], args[2], args[3]};
    return 0;
  }
  case 126: return 0;
  case 127: return 0;
  case 128: {
    if (!args[1]) { throw Fault(ctx, ctx.esp, 0, "SetI3DL2Listener requires a listener struct"); }
    memory_->CheckRange(ctx, args[1], 48);
    memory_->Read(ctx, args[1], listenerI3dl2_.data(), sizeof(listenerI3dl2_));
    return 0;
  }
  case 105: return DownloadEffectsImage(ctx, args);
  case 129: {
    if (!args[1]) { throw Fault(ctx, ctx.esp, 0, "DirectSoundCreate requires an output pointer"); }
    memory_->CheckRange(ctx, args[1], 4);
    if (!device_) { device_ = ArenaAllocate(ctx, kDeviceBytes); }
    Write32(ctx, args[1], device_);
    return 0;
  }
  case 130: return CreateBuffer(ctx, args[0], args[1]);
  case 122: return CreateBuffer(ctx, args[1], args[2]);
  case 131: return CreateStream(ctx, args[0], args[1]);
  case 104: {
    deliver.assign(pending_.begin(), pending_.end());
    pending_.clear();
    return 0;
  }
  case 97: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    const audio::AudioError error = voice->Play(now, args[3] & 3);
    if (error == audio::AudioError::kEmptyBuffer) { throw Fault(ctx, args[0], 0, "DirectSound buffer play without playable frames"); }
    if (error != audio::AudioError::kNone) { return audio::kGuestErrorFail; }
    return 0;
  }
  case 98: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    return voice->Stop(now) == audio::AudioError::kNone ? 0 : audio::kGuestErrorFail;
  }
  case 100: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    if (!args[1]) { throw Fault(ctx, ctx.esp, 0, "GetStatus requires an output pointer"); }
    memory_->CheckRange(ctx, args[1], 4);
    Write32(ctx, args[1], voice->Status(now));
    return 0;
  }
  case 99: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    return voice->SetLoopRegion(args[1], args[2]) == audio::AudioError::kNone ? 0 : audio::kGuestErrorInvalidCall;
  }
  case 101: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    if (args[1]) { memory_->CheckRange(ctx, args[1], 4); }
    if (args[2]) { memory_->CheckRange(ctx, args[2], 4); }
    const auto [play, write] = voice->CurrentPosition(now);
    if (args[1]) { Write32(ctx, args[1], play); }
    if (args[2]) { Write32(ctx, args[2], write); }
    return 0;
  }
  case 102: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    const audio::AudioError error = voice->SetCurrentPosition(now, args[1]);
    if (error == audio::AudioError::kInvalidPosition) {
      throw Fault(ctx, args[0], 0, "DirectSound buffer position past its end");
    }
    return error == audio::AudioError::kNone ? 0 : audio::kGuestErrorFail;
  }
  case 103: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    if (!args[3] || !args[4]) { throw Fault(ctx, ctx.esp, 0, "DirectSound buffer lock requires its primary outputs"); }
    memory_->CheckRange(ctx, args[3], 4);
    memory_->CheckRange(ctx, args[4], 4);
    if (args[5]) { memory_->CheckRange(ctx, args[5], 4); }
    if (args[6]) { memory_->CheckRange(ctx, args[6], 4); }
    audio::LockRange range;
    if (voice->Lock(args[1], args[2], args[7], range) != audio::AudioError::kNone) { return audio::kGuestErrorFail; }
    memory_->CheckRange(ctx, state->data, voice->BufferBytes());
    Write32(ctx, args[3], state->data + range.firstOffset);
    Write32(ctx, args[4], range.firstBytes);
    if (args[5]) { Write32(ctx, args[5], range.secondBytes ? state->data : 0); }
    if (args[6]) { Write32(ctx, args[6], range.secondBytes); }
    return 0;
  }
  case 73: {
    if (!FindState(args[0], false)) { return audio::kGuestErrorFail; }
    return 0;
  }
  case 74: {
    VoiceState* state = FindState(args[0], false);
    if (!state) { return audio::kGuestErrorFail; }
    return BufferRelease(ctx, *state);
  }
  case 120: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    const uint32_t data = args[1], bytes = args[2];
    if (data && bytes) { memory_->CheckRange(ctx, data, bytes); }
    if (data == state->data && bytes == voice->BufferBytes()) { return 0; }
    const uint32_t previousOwned = state->owned;
    uint32_t owned = 0;
    if (!data && bytes) {
      owned = ArenaAllocate(ctx, bytes);
      if (previousOwned) { ArenaFree(previousOwned); }
      state->data = state->owned = owned;
    } else {
      if (previousOwned && previousOwned != data) { ArenaFree(previousOwned); }
      state->data = data;
      state->owned = previousOwned == data ? previousOwned : 0;
    }
    voice->SetBufferLength(now, bytes);
    return 0;
  }
  case 93: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    state->volumeMb = int32_t(args[1]);
    return 0;
  }
  case 113: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    if (voice->SetFrequency(now, args[1]) != audio::AudioError::kNone) {
      throw Fault(ctx, args[0], 0, "DirectSound buffer frequency outside the modeled range");
    }
    return 0;
  }
  case 95: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    state->headroomMb = args[1];
    return 0;
  }
  case 114: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    state->bins = ReadMixBins(ctx, args[1], 8);
    return 0;
  }
  case 96: {
    auto [state, voice] = voiceArgs(false);
    if (!state) { return audio::kGuestErrorFail; }
    state->binVolumes = ReadMixBins(ctx, args[1], 32);
    return 0;
  }
  case 94: {
    if (!FindState(args[0], false)) { return audio::kGuestErrorFail; }
    return 0;
  }
  case 115: {
    VoiceState* state = FindState(args[0], false);
    if (!state) { return audio::kGuestErrorFail; }
    state->maxDistance = args[1];
    return 0;
  }
  case 116: {
    VoiceState* state = FindState(args[0], false);
    if (!state) { return audio::kGuestErrorFail; }
    state->minDistance = args[1];
    return 0;
  }
  case 117: {
    VoiceState* state = FindState(args[0], false);
    if (!state) { return audio::kGuestErrorFail; }
    state->position = {args[1], args[2], args[3]};
    return 0;
  }
  case 118: {
    if (!FindState(args[0], false)) { return audio::kGuestErrorFail; }
    return 0;
  }
  case 119: {
    VoiceState* state = FindState(args[0], false);
    if (!state) { return audio::kGuestErrorFail; }
    return I3dl2Source(ctx, *state, args[1]) ? 0 : audio::kGuestErrorFail;
  }
  case 76: {
    VoiceState* state = FindState(args[0], true);
    if (!state) { return audio::kGuestErrorFail; }
    return ++state->refs;
  }
  case 77: {
    VoiceState* state = FindState(args[0], true);
    if (!state) { return audio::kGuestErrorFail; }
    return StreamRelease(ctx, *state, now, deliver);
  }
  case 78: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    if (!args[1]) { throw Fault(ctx, ctx.esp, 0, "stream GetInfo requires an output pointer"); }
    memory_->CheckRange(ctx, args[1], 16);
    const audio::StreamInfo info = voice->GetStreamInfo();
    Write32(ctx, args[1], info.flags);
    Write32(ctx, args[1] + 4, info.packetSize);
    Write32(ctx, args[1] + 8, 0);
    Write32(ctx, args[1] + 12, 0);
    return 0;
  }
  case 81: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    if (!args[1]) { throw Fault(ctx, ctx.esp, 0, "stream GetStatus requires an output pointer"); }
    memory_->CheckRange(ctx, args[1], 4);
    Write32(ctx, args[1], voice->StreamReady() ? 1u : 0u);
    return 0;
  }
  case 82: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    if (!args[1]) { return 0; }
    std::array<uint32_t, 5> packet{};
    memory_->Read(ctx, args[1], packet.data(), sizeof(packet));
    const uint32_t data = packet[0], size = packet[1], completedPtr = packet[2], statusPtr = packet[3], packetContext = packet[4];
    if (size && !data) { throw Fault(ctx, args[1], 20, "stream packet has a size without payload"); }
    if (size) { memory_->CheckRange(ctx, data, size); }
    if (!state->callback && packetContext) {
      Unsupported(ctx, id, "CDirectSoundStream_Process event-signaled packet completion");
    }
    if (completedPtr) { memory_->CheckRange(ctx, completedPtr, 4); }
    if (statusPtr) { memory_->CheckRange(ctx, statusPtr, 4); }
    const uint64_t token = ++nextToken_;
    tokens_.emplace(token, PacketRoute{args[0], completedPtr, statusPtr, packetContext, data, size});
    try {
      const audio::AudioError error = voice->ProcessPacket(now, token, size);
      if (error == audio::AudioError::kPacketLimit) { throw Fault(ctx, args[0], 0, "stream packet budget exhausted"); }
      if (error != audio::AudioError::kNone) { throw Fault(ctx, args[1], 20, "stream packet payload is misaligned or overflowing"); }
    } catch (...) {
      tokens_.erase(token);
      throw;
    }
    if (completedPtr) { Write32(ctx, completedPtr, 0); }
    if (statusPtr) { Write32(ctx, statusPtr, audio::kPacketPending); }
    return 0;
  }
  case 79: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    const bool flushes = !voice->QueuedPackets() && voice->Active();
    std::vector<audio::Completion> completions;
    voice->Discontinuity(now, completions);
    for (audio::Completion& completion : completions) { completion.voice = args[0]; }
    if (flushes) {
      QueueFlushCallbacksLocked(ctx, args[0], deliver);
      PublishLocked(ctx, completions, &deliver, args[0]);
    } else {
      PublishLocked(ctx, completions, nullptr, 0);
    }
    return 0;
  }
  case 80: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    std::vector<audio::Completion> completions;
    voice->Flush(completions);
    flushVoice(args[0], *voice, completions);
    return 0;
  }
  case 84: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    std::vector<audio::Completion> completions;
    if (!voice->FlushEx(args[3] != 0, completions)) { return 0; }
    flushVoice(args[0], *voice, completions);
    return 0;
  }
  case 83: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    return voice->Pause(now, args[1] == 1) == audio::AudioError::kNone ? 0 : audio::kGuestErrorFail;
  }
  case 85: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    state->volumeMb = int32_t(args[1]);
    return 0;
  }
  case 106: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    if (voice->SetFrequency(now, args[1]) != audio::AudioError::kNone) {
      throw Fault(ctx, args[0], 0, "DirectSound stream frequency outside the modeled range");
    }
    return 0;
  }
  case 87: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    state->headroomMb = args[1];
    return 0;
  }
  case 107: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    state->bins = ReadMixBins(ctx, args[1], 8);
    return 0;
  }
  case 88: {
    auto [state, voice] = voiceArgs(true);
    if (!state) { return audio::kGuestErrorFail; }
    state->binVolumes = ReadMixBins(ctx, args[1], 32);
    return 0;
  }
  case 86: {
    if (!FindState(args[0], true)) { return audio::kGuestErrorFail; }
    return 0;
  }
  case 108: {
    VoiceState* state = FindState(args[0], true);
    if (!state) { return audio::kGuestErrorFail; }
    state->maxDistance = args[1];
    return 0;
  }
  case 109: {
    VoiceState* state = FindState(args[0], true);
    if (!state) { return audio::kGuestErrorFail; }
    state->minDistance = args[1];
    return 0;
  }
  case 110: {
    VoiceState* state = FindState(args[0], true);
    if (!state) { return audio::kGuestErrorFail; }
    state->position = {args[1], args[2], args[3]};
    return 0;
  }
  case 111: {
    if (!FindState(args[0], true)) { return audio::kGuestErrorFail; }
    return 0;
  }
  case 112: {
    VoiceState* state = FindState(args[0], true);
    if (!state) { return audio::kGuestErrorFail; }
    return I3dl2Source(ctx, *state, args[1]) ? 0 : audio::kGuestErrorFail;
  }
  }
  throw Fault(ctx, entry.address, 0, entry.name);
}

bool AudioProvider::Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) {
  if (id < BROWSER_AUDIO_FIRST_ID || id > BROWSER_AUDIO_LAST_ID) {
    throw Fault(ctx, ctx.esp, 0, "audio dispatch outside its id range");
  }
  Require(ctx, base);
  const uint32_t cleanup = 4 * (InfoFor(id).arguments + 1);
  if (uint64_t(ctx.esp) + cleanup > 0x100000000ull) {
    throw Fault(ctx, ctx.esp, cleanup, "audio stack cleanup overflow");
  }
  std::vector<PendingCallback> deliver;
  uint32_t result = 0;
  {
    std::lock_guard lock(mutex_);
    const auto clock = sink_->Snapshot();
    const uint64_t now = sink_->HasOutput() ? clock.submittedFrames : clock.consumedFrames;
    AdvanceAllLocked(ctx, now);
    result = Handle(ctx, id, now, deliver);
    FeedDispatchLocked(ctx);
  }
  Deliver(ctx, base, deliver);
  ctx.eax = result;
  ctx.esp += cleanup;
  return false;
}

std::shared_ptr<AudioProvider> InstallAudioProvider(Runtime& runtime, PreparedImage& image,
    std::shared_ptr<SilentAudioSink> sink, std::shared_ptr<const kernel::DiscIndex> disc) {
  return std::make_shared<AudioProvider>(runtime, image, std::move(sink), std::move(disc));
}

}
