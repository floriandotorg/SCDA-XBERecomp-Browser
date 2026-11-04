#include "browser/audio-core.h"
#include "audio/output.h"

#include <cstdio>
#include <array>

using namespace browser::audio;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    ++checks;                                                         \
    if (!(cond)) {                                                    \
      ++failures;                                                     \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                 \
  } while (0)

static const Format kPcmStereo = *Format::Make(kFormatPcm, 2, 48000, 4, 16);
static const Format kPcmMono8 = *Format::Make(kFormatPcm, 1, 22050, 1, 8);
static const Format kAdpcmStereo = *Format::Make(0x69, 2, 48000, 72, 4);
static const Format kAdpcmMono441 = *Format::Make(0x69, 1, 44100, 36, 4);

static void Formats() {
  static_assert(kRate == audio::kRate);
  CHECK(Format::Make(1, 2, 48000, 4, 16).has_value());
  CHECK(Format::Make(1, 1, 22050, 1, 8).has_value());
  CHECK(Format::Make(0x69, 2, 44100, 72, 4).has_value());
  CHECK(Format::Make(0x69, 1, 48000, 36, 4).has_value());
  CHECK(!Format::Make(2, 2, 48000, 4, 16).has_value());
  CHECK(!Format::Make(1, 2, 48000, 4, 24).has_value());
  CHECK(!Format::Make(1, 2, 48000, 2, 16).has_value());
  CHECK(!Format::Make(0x69, 2, 48000, 72, 16).has_value());
  CHECK(!Format::Make(0x69, 2, 48000, 36, 4).has_value());
  CHECK(!Format::Make(1, 0, 48000, 0, 16).has_value());
  CHECK(!Format::Make(1, 9, 48000, 18, 16).has_value());
  CHECK(!Format::Make(0x69, 9, 48000, 324, 4).has_value());
  CHECK(!Format::Make(1, 2, 0, 4, 16).has_value());
  CHECK(!Format::Make(1, 2, 384001, 4, 16).has_value());
  CHECK(Format::Make(1, 2, 384000, 4, 16).has_value());
  CHECK(!Format::Make(1, 2, 48000, 5, 16).has_value());
}

static void FrameMath() {
  CHECK(FramesOf(kPcmStereo, 1000) == std::optional<uint32_t>(250));
  CHECK(BytesOf(kPcmStereo, 250) == std::optional<uint32_t>(1000));
  CHECK(FramesOf(kPcmMono8, 100) == std::optional<uint32_t>(100));
  CHECK(FramesOf(kAdpcmStereo, 72) == std::optional<uint32_t>(64));
  CHECK(FramesOf(kAdpcmStereo, 71) == std::optional<uint32_t>(0));
  CHECK(FramesOf(kAdpcmStereo, 216) == std::optional<uint32_t>(192));
  CHECK(BytesOf(kAdpcmStereo, 64) == std::optional<uint32_t>(72));
  CHECK(BytesOf(kAdpcmStereo, 63) == std::optional<uint32_t>(0));
  CHECK(BytesOf(kAdpcmStereo, 128) == std::optional<uint32_t>(144));
  CHECK(FramesOf(kAdpcmMono441, 36) == std::optional<uint32_t>(64));
  CHECK(!BytesOf(kPcmStereo, 0xFFFFFFFFu).has_value());
  CHECK(FramesOf(kPcmStereo, 3) == std::optional<uint32_t>(0));
  CHECK(BytesOf(kPcmStereo, 0) == std::optional<uint32_t>(0));
}

static void BankLimits() {
  VoiceBank bank;
  for (uint32_t n = 0; n < kMaxVoices; ++n) {
    CHECK(bank.CreateBuffer(0x1000 + n * 0x40, kPcmStereo, 0) == AudioError::kNone);
  }
  CHECK(bank.Count() == kMaxVoices);
  CHECK(bank.CreateBuffer(0x9000, kPcmStereo, 0) == AudioError::kVoiceLimit);
  CHECK(bank.CreateStream(0x9000, kPcmStereo, 1) == AudioError::kVoiceLimit);
  CHECK(bank.CreateBuffer(0x1000, kPcmStereo, 0) == AudioError::kDuplicateVoice);
  CHECK(bank.Count() == kMaxVoices);
  CHECK(bank.Find(0x9999) == nullptr);
  CHECK(bank.Erase(0x9999) == AudioError::kUnknownVoice);
  CHECK(bank.Erase(0x1000) == AudioError::kNone);
  CHECK(bank.CreateStream(0x9000, kAdpcmStereo, 2) == AudioError::kNone);
  CHECK(bank.Count() == kMaxVoices);
  CHECK(bank.Ids().size() == kMaxVoices);
  VoiceBank empty;
  std::vector<Completion> out;
  empty.AdvanceAll(1000, out);
  CHECK(out.empty());
}

const auto BankSounding = [] {
  VoiceBank bank;
  CHECK(!bank.Sounding(0));
  CHECK(bank.CreateBuffer(1, kPcmStereo, 400) == AudioError::kNone);
  CHECK(!bank.Sounding(0));
  CHECK(bank.Find(1)->Play(0, 0) == AudioError::kNone);
  CHECK(bank.Sounding(0));
  CHECK(!bank.Sounding(100));
  CHECK(bank.Find(1)->Play(100, kPlayLooping) == AudioError::kNone);
  CHECK(bank.Sounding(100000));
  bank.Find(1)->Stop(100001);
  CHECK(!bank.Sounding(100001));
  CHECK(bank.CreateStream(2, kPcmStereo, 2) == AudioError::kNone);
  CHECK(!bank.Sounding(100001));
  CHECK(bank.Find(2)->ProcessPacket(100001, 1, 400) == AudioError::kNone);
  CHECK(bank.Sounding(100001));
  CHECK(bank.Find(2)->Pause(100001, true) == AudioError::kNone);
  CHECK(!bank.Sounding(100001));
  CHECK(bank.Find(2)->Pause(100001, false) == AudioError::kNone);
  CHECK(bank.Sounding(100001));
  std::vector<Completion> completions;
  bank.AdvanceAll(100101, completions);
  CHECK(!bank.Sounding(100101));
};

static void BufferControl() {
  Voice v = Voice::Buffer(kPcmStereo, 4000);
  CHECK(!v.IsStream());
  CHECK(v.BufferBytes() == 4000);
  CHECK(v.Status(0) == 0);
  v.Play(0, 0);
  CHECK(v.Status(0) == (kStatusPlaying));
  v.Stop(100);
  CHECK(v.Status(100) == 0);
  v.Play(200, kPlayLooping);
  CHECK(v.Status(200) == (kStatusPlaying | kStatusLooping));
  CHECK(v.Pause(300, true) == AudioError::kInvalidState);
  std::vector<Completion> out;
  v.Advance(300, out);
  CHECK(out.empty());

  Voice bad = Voice::Buffer(kPcmStereo, 4000);
  CHECK(bad.SetLoopRegion(4001, 0) == AudioError::kInvalidLoop);
  CHECK(bad.SetLoopRegion(3900, 200) == AudioError::kInvalidLoop);
  CHECK(bad.SetLoopRegion(3900, 0) == AudioError::kNone);
  Voice tinyLoop = Voice::Buffer(kAdpcmStereo, 7200);
  CHECK(tinyLoop.SetLoopRegion(36, 1) == AudioError::kInvalidLoop);
  CHECK(tinyLoop.SetLoopRegion(36, 36) == AudioError::kNone);
  CHECK(bad.SetCurrentPosition(0, 4001) == AudioError::kInvalidPosition);
  CHECK(bad.SetCurrentPosition(0, 4000) == AudioError::kNone);
  CHECK(bad.SetFrequency(0, 0) == AudioError::kNone);
  CHECK(bad.SetFrequency(0, 384001) == AudioError::kInvalidFormat);
  CHECK(bad.SetFrequency(0, 96000) == AudioError::kNone);
  CHECK(bad.Frequency() == 96000);

  Voice emptyBuf = Voice::Buffer(kPcmStereo, 0);
  CHECK(emptyBuf.BufferBytes() == 0);
  CHECK(emptyBuf.Play(0, 0) == AudioError::kEmptyBuffer);
  CHECK(emptyBuf.Play(0, kPlayLooping) == AudioError::kEmptyBuffer);
  CHECK(emptyBuf.Status(0) == 0);
  LockRange range;
  CHECK(emptyBuf.Lock(0, 4, 0, range) == AudioError::kEmptyBuffer);
  CHECK(emptyBuf.SetBufferLength(0, 4000) == AudioError::kNone);
  CHECK(emptyBuf.Lock(0, 4, 0, range) == AudioError::kNone);
  CHECK(emptyBuf.SetBufferLength(50, 0) == AudioError::kNone);
  CHECK(emptyBuf.Lock(0, 4, 0, range) == AudioError::kEmptyBuffer);
}

static void BufferPositions() {
  Voice v = Voice::Buffer(kPcmStereo, 4000);
  v.Play(0, 0);
  CHECK(v.PlayFrames(0) == 0);
  CHECK(v.PlayFrames(500) == 500);
  CHECK(v.PlayFrames(999) == 999);
  std::vector<Completion> out;
  v.Advance(1500, out);
  CHECK(out.empty());
  CHECK(v.Status(1500) == 0);
  CHECK(v.PlayFrames(1500) == 0);
  CHECK(v.CurrentPosition(1500).first == 0);

  v.Play(2000, 0);
  CHECK(v.Status(2000) == kStatusPlaying);
  CHECK(v.PlayFrames(2000) == 0);
  CHECK(v.PlayFrames(2999) == 999);
  CHECK(v.Status(20000) == 0);
  CHECK(v.PlayFrames(20000) == 0);

  Voice loop = Voice::Buffer(kPcmStereo, 4000);
  loop.Play(0, kPlayLooping);
  CHECK(loop.Status(50500) == (kStatusPlaying | kStatusLooping));
  CHECK(loop.PlayFrames(50500) == 500);
  loop.Stop(50500);
  CHECK(loop.PlayFrames(60000) == 500);
  loop.Play(60000, kPlayLooping | kPlayFromStart);
  CHECK(loop.PlayFrames(60000) == 0);
  CHECK(loop.PlayFrames(61500) == 500);

  Voice region = Voice::Buffer(kPcmStereo, 4000);
  CHECK(region.SetLoopRegion(400, 800) == AudioError::kNone);
  region.Play(0, kPlayLooping);
  CHECK(region.PlayFrames(250) == 250);
  CHECK(region.PlayFrames(350) == 150);
  CHECK(region.PlayFrames(550) == 150);
  CHECK(region.PlayFrames(299) == 299);

  Voice freq = Voice::Buffer(kPcmStereo, 48000 * 4 * 4);
  CHECK(freq.Play(0, 0) == AudioError::kNone);
  CHECK(freq.SetFrequency(0, 24000) == AudioError::kNone);
  CHECK(freq.PlayFrames(48000) == 24000);
  CHECK(freq.SetFrequency(48000, 44100) == AudioError::kNone);
  CHECK(freq.PlayFrames(48000) == 24000);
  CHECK(freq.PlayFrames(96000) == 68100);

  Voice exact = Voice::Buffer(*Format::Make(1, 2, 44100, 4, 16), 44100 * 4 * 4);
  exact.Play(0, 0);
  CHECK(exact.PlayFrames(1) == 0);
  CHECK(exact.PlayFrames(48000) == 44100);
  CHECK(exact.PlayFrames(96000) == 88200);
  CHECK(exact.PlayFrames(480) == 441);

  Voice regress = Voice::Buffer(kPcmStereo, 48000 * 4);
  regress.Play(100, 0);
  CHECK(regress.PlayFrames(50) == 0);
  std::vector<Completion> out2;
  regress.Advance(50, out2);
  CHECK(out2.empty());
  CHECK(regress.PlayFrames(150) == 50);

  Voice adp = Voice::Buffer(kAdpcmStereo, 7200);
  adp.Play(0, 0);
  CHECK(adp.PlayFrames(320) == 320);
  auto pos = adp.CurrentPosition(320);
  CHECK(pos.first == 360);
  CHECK(pos.second == (360 + 288) % 7200);
  auto posPcm = v.CurrentPosition(2050);
  CHECK(posPcm.first == 200);
  CHECK(posPcm.second == (200 + 1024) % 4000);
}

static void BufferLock() {
  Voice v = Voice::Buffer(kPcmStereo, 4000);
  LockRange range;
  CHECK(v.Lock(3900, 200, 0, range) == AudioError::kNone);
  CHECK(range.firstOffset == 3900 && range.firstBytes == 100);
  CHECK(range.secondOffset == 0 && range.secondBytes == 100);
  CHECK(v.Lock(0, 0, kLockEntireBuffer, range) == AudioError::kNone);
  CHECK(range.firstOffset == 0 && range.firstBytes == 4000);
  CHECK(range.secondBytes == 0);
  CHECK(v.Lock(4100, 100, 0, range) == AudioError::kNone);
  CHECK(range.firstOffset == 100 && range.firstBytes == 100);
  CHECK(range.secondBytes == 0);
  CHECK(v.Lock(0, 4000, 0, range) == AudioError::kNone);
  CHECK(range.firstBytes == 4000 && range.secondBytes == 0);
  CHECK(v.Lock(100, 8000, 0, range) == AudioError::kNone);
  CHECK(range.firstBytes == 3900 && range.secondBytes == 4100);
}

static void StreamPackets() {
  Voice v = Voice::Stream(kAdpcmStereo, 2);
  CHECK(v.IsStream());
  CHECK(!v.Active());
  CHECK(!v.Paused());
  CHECK(!v.Ending());
  CHECK(v.StreamReady());
  CHECK(v.MaxPackets() == 2);
  StreamInfo info = v.GetStreamInfo();
  CHECK(info.flags == 0x3);
  CHECK(info.packetSize == 72);
  CHECK(v.Status(0) == 0);

  CHECK(v.ProcessPacket(0, 0xAAAA0001, 144) == AudioError::kNone);
  CHECK(v.Active());
  CHECK(v.QueuedPackets() == 1);
  CHECK(v.StreamReady());
  CHECK(v.ProcessPacket(0, 0xAAAA0002, 72) == AudioError::kNone);
  CHECK(!v.StreamReady());

  std::vector<Completion> out;
  v.Advance(64, out);
  CHECK(out.empty());
  CHECK(v.QueuedPackets() == 2);
  v.Advance(128, out);
  CHECK(out.size() == 1);
  CHECK(out[0].token == 0xAAAA0001);
  CHECK(out[0].completedBytes == 144);
  CHECK(out[0].status == 0);
  CHECK(v.QueuedPackets() == 1);
  CHECK(v.StreamReady());
  out.clear();
  v.Advance(128, out);
  CHECK(out.empty());
  v.Advance(192, out);
  CHECK(out.size() == 1);
  CHECK(out[0].token == 0xAAAA0002);
  CHECK(out[0].completedBytes == 72);
  CHECK(v.QueuedPackets() == 0);
  CHECK(v.Active());

  CHECK(v.ProcessPacket(200, 0xBBBB0001, 400) == AudioError::kInvalidFormat);
  CHECK(v.QueuedPackets() == 0);

  Voice fold = Voice::Stream(kPcmStereo, 4);
  CHECK(fold.ProcessPacket(0, 0xF01D, 400) == AudioError::kNone);
  CHECK(fold.ProcessPacket(0, 0xF02D, 400) == AudioError::kNone);
  CHECK(fold.SetFrequency(150, 48000) == AudioError::kNone);
  CHECK(fold.QueuedPackets() == 1);
  std::vector<Completion> foldOut;
  fold.Advance(150, foldOut);
  CHECK(foldOut.size() == 1);
  CHECK(foldOut[0].token == 0xF01D);
  CHECK(foldOut[0].status == 0);
  CHECK(foldOut[0].completedBytes == 400);
  foldOut.clear();
  fold.Advance(250, foldOut);
  CHECK(foldOut.size() == 1 && foldOut[0].token == 0xF02D);

  Voice pcm = Voice::Stream(kPcmStereo, 4);
  CHECK(pcm.ProcessPacket(0, 1, 400) == AudioError::kNone);
  std::vector<Completion> out3;
  pcm.Advance(99, out3);
  CHECK(out3.empty());
  pcm.Advance(100, out3);
  CHECK(out3.size() == 1 && out3[0].completedBytes == 400 && out3[0].status == 0);

  Voice capped = Voice::Stream(kPcmStereo, 40);
  for (uint32_t n = 0; n < kMaxPacketsPerVoice; ++n) {
    CHECK(capped.ProcessPacket(0, n, 4) == AudioError::kNone);
  }
  CHECK(capped.QueuedPackets() == kMaxPacketsPerVoice);
  CHECK(capped.ProcessPacket(0, 0xFFFF, 4) == AudioError::kPacketLimit);
  CHECK(capped.QueuedPackets() == kMaxPacketsPerVoice);
  CHECK(!capped.StreamReady());
}

static void StreamPauseEnding() {
  Voice v = Voice::Stream(kPcmStereo, 4);
  CHECK(v.ProcessPacket(0, 10, 400) == AudioError::kNone);
  CHECK(v.ProcessPacket(0, 11, 400) == AudioError::kNone);
  CHECK(v.Pause(50, true) == AudioError::kNone);
  CHECK(v.Paused());
  std::vector<Completion> out;
  v.Advance(5000, out);
  CHECK(out.empty());
  CHECK(v.QueuedPackets() == 2);
  CHECK(v.Pause(5000, false) == AudioError::kNone);
  CHECK(!v.Paused());
  v.Advance(5100, out);
  CHECK(out.size() == 1 && out[0].token == 10);

  Voice end = Voice::Stream(kPcmStereo, 4);
  CHECK(end.ProcessPacket(0, 20, 400) == AudioError::kNone);
  std::vector<Completion> out2;
  end.Discontinuity(0, out2);
  CHECK(out2.empty());
  CHECK(end.Ending());
  end.Advance(100, out2);
  CHECK(out2.size() == 1 && out2[0].status == 0);
  CHECK(!end.Active());
  CHECK(!end.Ending());
  CHECK(!end.Paused());

  Voice dry = Voice::Stream(kPcmStereo, 4);
  CHECK(dry.ProcessPacket(0, 30, 400) == AudioError::kNone);
  std::vector<Completion> outDry;
  dry.Discontinuity(200, outDry);
  CHECK(outDry.size() == 1);
  CHECK(outDry[0].token == 30 && outDry[0].status == 0 && outDry[0].completedBytes == 400);
  CHECK(!dry.Active());
  CHECK(!dry.Ending());

  Voice unpausedEnd = Voice::Stream(kPcmStereo, 4);
  CHECK(unpausedEnd.ProcessPacket(0, 40, 400) == AudioError::kNone);
  CHECK(unpausedEnd.Pause(0, true) == AudioError::kNone);
  std::vector<Completion> outEnd;
  unpausedEnd.Discontinuity(10, outEnd);
  CHECK(outEnd.empty());
  CHECK(unpausedEnd.Ending());
  std::vector<Completion> out4;
  unpausedEnd.Advance(10000, out4);
  CHECK(out4.empty());
  CHECK(unpausedEnd.Pause(10000, false) == AudioError::kNone);
  unpausedEnd.Advance(10100, out4);
  CHECK(out4.size() == 1);
  CHECK(!unpausedEnd.Active() && !unpausedEnd.Paused() && !unpausedEnd.Ending());
}

static void StreamFlush() {
  Voice v = Voice::Stream(kPcmStereo, 4);
  CHECK(v.ProcessPacket(0, 50, 400) == AudioError::kNone);
  CHECK(v.ProcessPacket(0, 51, 800) == AudioError::kNone);
  std::vector<Completion> out;
  v.Advance(50, out);
  CHECK(out.empty());
  v.Flush(out);
  CHECK(out.size() == 2);
  CHECK(out[0].token == 50 && out[0].status == kPacketFlushed && out[0].completedBytes == 0);
  CHECK(out[1].token == 51 && out[1].status == kPacketFlushed && out[1].completedBytes == 0);
  CHECK(v.QueuedPackets() == 0);
  CHECK(!v.Active() && !v.Paused() && !v.Ending());

  Voice consumed = Voice::Stream(kPcmStereo, 4);
  CHECK(consumed.ProcessPacket(0, 60, 400) == AudioError::kNone);
  CHECK(consumed.ProcessPacket(0, 61, 400) == AudioError::kNone);
  std::vector<Completion> out2;
  consumed.Advance(100, out2);
  CHECK(out2.size() == 1 && out2[0].completedBytes == 400);
  CHECK(consumed.ProcessPacket(100, 62, 400) == AudioError::kNone);
  out2.clear();
  consumed.Flush(out2);
  CHECK(out2.size() == 2);
  CHECK(out2[0].token == 61 && out2[0].status == kPacketFlushed && out2[0].completedBytes == 0);
  CHECK(out2[1].token == 62 && out2[1].status == kPacketFlushed && out2[1].completedBytes == 0);

  Voice pendingFlush = Voice::Stream(kPcmStereo, 4);
  CHECK(pendingFlush.ProcessPacket(0, 65, 400) == AudioError::kNone);
  CHECK(pendingFlush.ProcessPacket(0, 66, 400) == AudioError::kNone);
  CHECK(pendingFlush.SetFrequency(100, 48000) == AudioError::kNone);
  std::vector<Completion> outPf;
  pendingFlush.Flush(outPf);
  CHECK(outPf.size() == 2);
  CHECK(outPf[0].token == 65 && outPf[0].status == 0 && outPf[0].completedBytes == 400);
  CHECK(outPf[1].token == 66 && outPf[1].status == kPacketFlushed && outPf[1].completedBytes == 0);

  Voice inact = Voice::Stream(kPcmStereo, 4);
  std::vector<Completion> out3;
  CHECK(!inact.FlushEx(true, out3));
  CHECK(out3.empty());
  CHECK(inact.FlushEx(false, out3));
  CHECK(out3.empty());
  CHECK(inact.ProcessPacket(0, 70, 400) == AudioError::kNone);
  CHECK(inact.FlushEx(true, out3));
  CHECK(out3.size() == 1 && out3[0].status == kPacketFlushed);

  Voice rel = Voice::Stream(kPcmStereo, 4);
  CHECK(rel.ProcessPacket(0, 80, 400) == AudioError::kNone);
  CHECK(rel.ProcessPacket(0, 81, 400) == AudioError::kNone);
  std::vector<Completion> out5;
  rel.Release(out5);
  CHECK(out5.size() == 2);
  CHECK(out5[0].status == kPacketFlushed && out5[1].status == kPacketFlushed);
  CHECK(rel.QueuedPackets() == 0);
}

static void BankAdvance() {
  VoiceBank bank;
  CHECK(bank.CreateStream(0x2000, kPcmStereo, 2) == AudioError::kNone);
  CHECK(bank.CreateStream(0x1000, kPcmStereo, 2) == AudioError::kNone);
  Voice* a = bank.Find(0x2000);
  Voice* b = bank.Find(0x1000);
  CHECK(a && b);
  CHECK(a->ProcessPacket(0, 90, 400) == AudioError::kNone);
  CHECK(b->ProcessPacket(0, 91, 400) == AudioError::kNone);
  std::vector<Completion> out;
  bank.AdvanceAll(100, out);
  CHECK(out.size() == 2);
  CHECK(out[0].voice == 0x1000 && out[0].token == 91);
  CHECK(out[1].voice == 0x2000 && out[1].token == 90);
  CHECK(out[0].status == 0 && out[1].status == 0);
}

static void CandidateRegressions() {
  Voice capped = Voice::Stream(kPcmStereo, 40);
  for (uint32_t n = 0; n < kMaxPacketsPerVoice; ++n) {
    CHECK(capped.ProcessPacket(0, n + 1, 4) == AudioError::kNone);
  }
  CHECK(capped.SetFrequency(1000, 48000) == AudioError::kNone);
  CHECK(capped.QueuedPackets() == 0);
  CHECK(capped.RetainedRecords() == kMaxPacketsPerVoice);
  CHECK(capped.ProcessPacket(1000, 0x100, 4) == AudioError::kPacketLimit);
  std::vector<Completion> out;
  capped.Advance(1000, out);
  CHECK(out.size() == kMaxPacketsPerVoice);
  CHECK(out[0].status == 0 && out[0].token == 1);
  CHECK(out[31].token == 32);
  CHECK(capped.ProcessPacket(1000, 0x100, 4) == AudioError::kNone);

  Voice steady = Voice::Stream(kPcmStereo, 8);
  CHECK(steady.ProcessPacket(0, 1, 4) == AudioError::kNone);
  CHECK(steady.ProcessPacket(0, 2, 4) == AudioError::kNone);
  std::vector<Completion> drain;
  for (uint32_t n = 1; n <= 2000; ++n) {
    steady.Advance(n, drain);
    drain.clear();
    CHECK(steady.ProcessPacket(n, n + 2, 4) == AudioError::kNone);
  }
  CHECK(steady.QueuedPackets() == 2);
  CHECK(steady.RetainedRecords() <= kMaxPacketsPerVoice + 1);

  Voice hugeLoop = Voice::Buffer(kPcmStereo, 4000);
  CHECK(hugeLoop.Play(0, kPlayLooping) == AudioError::kNone);
  CHECK(hugeLoop.PlayFrames(UINT64_MAX) == 615);
  CHECK(hugeLoop.Status(UINT64_MAX) == (kStatusPlaying | kStatusLooping));
  Voice hugeFreq = Voice::Buffer(kPcmStereo, 4000);
  CHECK(hugeFreq.Play(0, kPlayLooping) == AudioError::kNone);
  CHECK(hugeFreq.SetFrequency(0, 384000) == AudioError::kNone);
  CHECK(hugeFreq.PlayFrames(UINT64_MAX) == 920);
  Voice hugeFin = Voice::Buffer(kPcmStereo, 4000);
  CHECK(hugeFin.Play(0, 0) == AudioError::kNone);
  CHECK(hugeFin.Status(UINT64_MAX) == 0);
  CHECK(hugeFin.PlayFrames(UINT64_MAX) == 0);
  CHECK(hugeFin.CurrentPosition(UINT64_MAX).first == 0);
  Voice hugeStream = Voice::Stream(kPcmStereo, 4);
  CHECK(hugeStream.ProcessPacket(0, 55, 400) == AudioError::kNone);
  std::vector<Completion> hugeOut;
  hugeStream.Advance(UINT64_MAX, hugeOut);
  CHECK(hugeOut.size() == 1);
  CHECK(hugeOut[0].token == 55 && hugeOut[0].completedBytes == 400 && hugeOut[0].status == 0);
  CHECK(hugeStream.PlayFrames(UINT64_MAX) == 0);
  CHECK(hugeStream.Status(UINT64_MAX) == 0);

  Voice ready = Voice::Stream(kPcmStereo, 4);
  for (uint32_t round = 0; round < 8; ++round) {
    for (uint32_t n = 0; n < 4; ++n) {
      CHECK(ready.ProcessPacket(round * 1000 + n * 10, round * 4 + n + 1, 4) == AudioError::kNone);
    }
    CHECK(ready.SetFrequency(round * 1000 + 100, 48000) == AudioError::kNone);
  }
  CHECK(ready.QueuedPackets() == 0);
  CHECK(ready.RetainedRecords() == kMaxPacketsPerVoice);
  CHECK(!ready.StreamReady());
  CHECK(ready.ProcessPacket(10000, 0xFF, 4) == AudioError::kPacketLimit);
  std::vector<Completion> readyOut;
  ready.Advance(10000, readyOut);
  CHECK(readyOut.size() == kMaxPacketsPerVoice);
  for (uint32_t n = 0; n < kMaxPacketsPerVoice; ++n) {
    CHECK(readyOut[n].token == n + 1);
    CHECK(readyOut[n].status == 0);
    CHECK(readyOut[n].completedBytes == 4);
  }
  CHECK(ready.StreamReady());
  CHECK(ready.ProcessPacket(10000, 0xFF, 4) == AudioError::kNone);

  Voice mixed = Voice::Stream(kPcmStereo, 2);
  CHECK(mixed.ProcessPacket(0, 0xA1, 4) == AudioError::kNone);
  CHECK(mixed.ProcessPacket(0, 0xA2, 4) == AudioError::kNone);
  CHECK(!mixed.StreamReady());
  std::vector<Completion> mixedOut;
  mixed.Advance(1, mixedOut);
  CHECK(mixedOut.size() == 1 && mixedOut[0].token == 0xA1);
  CHECK(mixed.StreamReady());
  CHECK(mixed.SetFrequency(2, 48000) == AudioError::kNone);
  CHECK(mixed.QueuedPackets() == 0);
  CHECK(mixed.RetainedRecords() == 1);
  CHECK(mixed.StreamReady());
  CHECK(mixed.ProcessPacket(2, 0xA3, 4) == AudioError::kNone);
  CHECK(mixed.RetainedRecords() == 2);
  CHECK(mixed.StreamReady());
  CHECK(mixed.ProcessPacket(2, 0xA4, 4) == AudioError::kNone);
  CHECK(!mixed.StreamReady());
  CHECK(mixed.RetainedRecords() == 3);

  Voice deadLoop = Voice::Buffer(kPcmStereo, 4000);
  CHECK(deadLoop.Play(0, kPlayLooping) == AudioError::kNone);
  CHECK(deadLoop.Status(10) == (kStatusPlaying | kStatusLooping));
  CHECK(deadLoop.SetBufferLength(10, 0) == AudioError::kNone);
  CHECK(deadLoop.Status(20) == 0);
  std::vector<Completion> deadOut;
  deadLoop.Advance(30, deadOut);
  CHECK(deadLoop.Status(30) == 0);
  CHECK(deadLoop.PlayFrames(30) == 0);
  CHECK(deadOut.empty());
  CHECK(deadLoop.SetBufferLength(40, 4000) == AudioError::kNone);
  CHECK(deadLoop.Play(40, kPlayLooping) == AudioError::kNone);
  CHECK(deadLoop.Status(50) == (kStatusPlaying | kStatusLooping));

  VoiceBank bank;
  CHECK(bank.CreateStream(0x3000, kPcmStereo, 2) == AudioError::kNone);
  CHECK(bank.Find(0x3000)->ProcessPacket(0, 77, 4) == AudioError::kNone);
  CHECK(bank.Erase(0x3000) == AudioError::kRetainedCompletions);
  std::vector<Completion> rel;
  bank.Find(0x3000)->Release(rel);
  CHECK(rel.size() == 1 && rel[0].token == 77 && rel[0].status == kPacketFlushed);
  CHECK(bank.Erase(0x3000) == AudioError::kNone);
  CHECK(bank.CreateBuffer(0x4000, kPcmStereo, 4000) == AudioError::kNone);
  CHECK(bank.Erase(0x4000) == AudioError::kNone);
}

static void MixerCursors() {
  const auto halfRate = *Format::Make(1, 1, 24000, 2, 16);
  auto buffer = Voice::Buffer(halfRate, 8);
  CHECK(!buffer.Sample(0));
  CHECK(buffer.Play(100, kPlayLooping) == AudioError::kNone);
  CHECK(buffer.Sample(100)->frame == 0 && buffer.Sample(100)->fraction == 0);
  CHECK(buffer.Sample(101)->frame == 0 && buffer.Sample(101)->fraction == 24000);
  CHECK(buffer.Sample(102)->frame == 1 && buffer.Sample(102)->fraction == 0);
  CHECK(buffer.Sample(108)->frame == 0 && buffer.Sample(108)->fraction == 0);
  CHECK(buffer.Stop(103) == AudioError::kNone);
  CHECK(!buffer.Sample(104));
  CHECK(buffer.Play(110, kPlayFromStart) == AudioError::kNone);
  CHECK(buffer.Sample(117)->frame == 3 && buffer.Sample(117)->fraction == 24000);
  CHECK(!buffer.Sample(118));
  auto stream = Voice::Stream(halfRate, 2);
  CHECK(stream.ProcessPacket(0, 10, 4) == AudioError::kNone);
  CHECK(stream.ProcessPacket(0, 20, 4) == AudioError::kNone);
  CHECK(stream.Sample(3)->token == 10 && stream.Sample(3)->frame == 1 && stream.Sample(3)->fraction == 24000);
  CHECK(stream.Sample(4)->token == 20 && stream.Sample(4)->frame == 0);
  CHECK(!stream.Sample(8));
  CHECK(stream.QueuedPackets() == 2);
  std::vector<Completion> completed;
  stream.Advance(4, completed);
  CHECK(completed.size() == 1 && completed[0].token == 10);
  CHECK(stream.Sample(5)->token == 20 && stream.Sample(5)->fraction == 24000);
  CHECK(stream.Pause(5, true) == AudioError::kNone);
  CHECK(!stream.Sample(6));
  CHECK(stream.Pause(10, false) == AudioError::kNone);
  CHECK(stream.Sample(10)->token == 20 && stream.Sample(10)->fraction == 24000);
}

static void AudioDecoding() {
  const std::array<uint8_t, 4> pcm{0, 128, 255, 127};
  CHECK(DecodePcm(kPcmStereo, pcm, 0) == -1.0f);
  CHECK(DecodePcm(kPcmStereo, pcm, 1) == 32767.0f / 32768);
  CHECK(DecodePcm(kPcmMono8, pcm, 0) == -1.0f);
  CHECK(DecodePcm(kPcmMono8, std::span(pcm).subspan(1), 0) == 0.0f);
  std::array<uint8_t, 72> block{};
  block[0] = 0xe8;
  block[1] = 3;
  block[4] = 0x18;
  block[5] = 0xfc;
  block[8] = 0x11;
  block[12] = 0x99;
  std::array<int16_t, 130> decoded{};
  CHECK(DecodeAdpcm(block, 2, decoded));
  CHECK(decoded[0] == 1000 && decoded[1] == -1000);
  CHECK(decoded[2] == 1001 && decoded[3] == -1001);
  CHECK(decoded[4] == 1002 && decoded[5] == -1002);
  CHECK(!DecodeAdpcm(std::span(block).first(71), 2, decoded));
  CHECK(!DecodeAdpcm(block, 0, decoded));
  CHECK(!DecodeAdpcm(block, 9, decoded));
  CHECK(!DecodeAdpcm(block, 2, std::span(decoded).first(129)));
  block.fill(0x77);
  block[0] = 0;
  block[1] = 0;
  block[2] = 255;
  block[4] = 0;
  block[5] = 0;
  block[6] = 255;
  CHECK(DecodeAdpcm(block, 2, decoded));
  CHECK(decoded[2] == 32767 && decoded[129] == 32767);
}

int main() {
  AudioDecoding();
  MixerCursors();
  Formats();
  FrameMath();
  BankLimits();
  BankSounding();
  BufferControl();
  BufferPositions();
  BufferLock();
  StreamPackets();
  StreamPauseEnding();
  StreamFlush();
  BankAdvance();
  CandidateRegressions();
  if (failures) {
    std::printf("audio-core-contracts-failed checks=%d failures=%d\n", checks, failures);
    return 1;
  }
  std::printf("audio-core-contracts-ok checks=%d voices=%u packets=%u rate=%u\n", checks, kMaxVoices, kMaxPacketsPerVoice,
              kRate);
  return 0;
}
