#include "browser/audio-provider.h"
#include "browser/audio-entries.h"
#include "browser/timer-provider.h"
#include "kernel/disc_index.h"
#include "cpu/x86_ops.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

const auto check = [](bool value, const char* reason) {
  if (!value) { throw std::runtime_error(reason); }
};
thread_local const char* refuseStage = "unknown";
const auto refuse = [](const auto& operation) {
  bool refused = false;
  try { operation(); } catch (const browser::Fault&) { refused = true; }
  if (!refused) { std::fprintf(stderr, "refuse stage: %s\n", refuseStage); }
  check(refused, "invalid audio operation succeeded");
};
#define REFUSE_AT(stage, operation) refuseStage = stage; refuse(operation);

constexpr uint32_t kXnet = 0xF11000;
constexpr uint32_t kWsa = 0xF12000;
constexpr uint32_t kD3d = 0xF13000;
constexpr uint32_t kVtable = 0xF21000;
constexpr uint32_t kVtable2 = 0xF21040;
constexpr uint32_t kCallback = 0xF31000;
constexpr uint32_t kCallbackReentry = 0xF31010;
constexpr uint32_t kCallbackFault = 0xF31020;
constexpr uint32_t kWavePcm = 0x24000;
constexpr uint32_t kWaveAdpcm = 0x24020;
constexpr uint32_t kWaveBadTag = 0x24040;
constexpr uint32_t kWaveBadAlign = 0x24060;
constexpr uint32_t kBufferDesc = 0x24100;
constexpr uint32_t kStreamDesc = 0x24200;
constexpr uint32_t kPacketDesc = 0x24300;
constexpr uint32_t kMixBins = 0x24380;
constexpr uint32_t kPayload = 0x24400;
constexpr uint32_t kOut = 0x25000;
constexpr uint32_t kStack = 0x28000;
constexpr uint32_t kApuWindow = 0xFE801100;

X86_FUNC(sub_00F11000);
X86_FUNC(sub_00F12000);
X86_FUNC(sub_00F13000);
#define HARNESS_DECL(name, id, args) X86_FUNC(BROWSER_AUDIO_GUEST(name)); X86_FUNC(BROWSER_AUDIO_ORIGINAL(name));
BROWSER_AUDIO_ENTRIES(HARNESS_DECL)

X86_FUNC(__imp__sub_00F11000) { ctx.esp += 8; }
X86_FUNC(__imp__sub_00F12000) { ctx.esp += 12; }
X86_FUNC(__imp__sub_00F13000) { ctx.esp += 28; }
X86_FUNC(__imp__sub_000534B8) { ctx.esp += 8; }
X86_FUNC(__imp__sub_0005351A) { ctx.esp += 12; }

std::atomic<uint32_t> originalCalls = 0;
#define HARNESS_ORIGINAL(name, id, args) X86_FUNC(BROWSER_AUDIO_ORIGINAL(name)) { ++originalCalls; ctx.esp += 4; }
BROWSER_AUDIO_ENTRIES(HARNESS_ORIGINAL)

struct CallbackEvent {
  uint32_t context;
  uint32_t packetContext;
  uint32_t status;
};
std::mutex callbackLock;
std::vector<CallbackEvent> callbackEvents;
uint32_t reentryObject = 0;
uint32_t reentryOutput = 0;
uint32_t reentryResult = 0;

const auto recordCallback = [](X86Context& ctx, uint8_t* base) {
  auto& memory = browser::Memory::From(base);
  std::array<uint32_t, 3> args{};
  memory.Read(ctx, ctx.esp + 4, args.data(), sizeof(args));
  {
    std::lock_guard lock(callbackLock);
    callbackEvents.push_back({args[0], args[1], args[2]});
  }
};

X86_FUNC(guest_callback) {
  recordCallback(ctx, base);
  ctx.esp += 16;
}

X86_FUNC(guest_callback_reentry) {
  recordCallback(ctx, base);
  const uint32_t saved = ctx.esp;
  const uint32_t esp = saved - 0x100;
  auto& memory = browser::Memory::From(base);
  const std::array<uint32_t, 3> frame = {0xF3F100, reentryObject, reentryOutput};
  memory.Write(ctx, esp, frame.data(), sizeof(frame));
  ctx.esp = esp;
  BROWSER_AUDIO_GUEST(CDirectSoundStream_GetStatus)(ctx, base);
  reentryResult = ctx.eax;
  check(ctx.esp == esp + 12, "reentered stream GetStatus stack cleanup wrong");
  ctx.esp = saved + 16;
}

X86_FUNC(guest_callback_fault) {
  recordCallback(ctx, base);
  throw browser::Fault(ctx, 0xfa11, 0, "guest stream callback failure");
}

const auto writeWave = [](browser::Memory& memory, uint32_t address, uint16_t tag, uint16_t channels, uint32_t rate, uint16_t align, uint16_t bits) {
  std::array<uint8_t, 16> wave{};
  const auto put16 = [&](uint32_t offset, uint16_t value) { wave[offset] = uint8_t(value); wave[offset + 1] = uint8_t(value >> 8); };
  const auto put32 = [&](uint32_t offset, uint32_t value) { for (uint32_t n = 0; n < 4; ++n) { wave[offset + n] = uint8_t(value >> (n * 8)); } };
  put16(0, tag);
  put16(2, channels);
  put32(4, rate);
  put32(8, rate * align);
  put16(12, align);
  put16(14, bits);
  memory.Write({}, address, wave.data(), wave.size());
};

const auto writeVtables = [](browser::Memory& memory, bool broken) {
  const std::array<uint32_t, BROWSER_AUDIO_VTABLE_SLOTS> vtable = {
    BROWSER_AUDIO_ADDRESS(CDirectSoundStream_AddRef), BROWSER_AUDIO_ADDRESS(CDirectSoundStream_Release),
    BROWSER_AUDIO_ADDRESS(CDirectSoundStream_GetInfo), BROWSER_AUDIO_ADDRESS(CDirectSoundStream_GetStatus),
    BROWSER_AUDIO_ADDRESS(CDirectSoundStream_Process), BROWSER_AUDIO_ADDRESS(CDirectSoundStream_Discontinuity),
    BROWSER_AUDIO_ADDRESS(CDirectSoundStream_Flush), kXnet, kWsa, kD3d, kCallback, kWsa,
  };
  memory.Write({}, kVtable, vtable.data(), sizeof(vtable));
  auto second = vtable;
  second[0] = broken ? 0x00F3DEAD : kCallback;
  second[1] = BROWSER_AUDIO_ADDRESS(CDirectSoundStream_AddRef);
  memory.Write({}, kVtable2, second.data(), sizeof(second));
};

struct Fixture {
  std::atomic<uint64_t> source = 0;
  std::atomic<uint64_t> clockStep = 0;
  std::atomic<uint32_t> clockSamples = 0;
  browser::Runtime runtime;
  browser::PreparedImage image;
  browser::KernelDataExport tick{};
  std::shared_ptr<browser::TimerQueue> queue;
  std::shared_ptr<browser::SilentAudioSink> sink;
  std::shared_ptr<browser::AudioProvider> audio;
  std::shared_ptr<const browser::SdkTrace> trace;

  static std::vector<X86FuncMapping> Mappings(bool weakAudio) {
    std::vector<X86FuncMapping> result = {
      {kXnet, sub_00F11000}, {kWsa, sub_00F12000}, {kD3d, sub_00F13000},
      {kCallback, guest_callback}, {kCallbackReentry, guest_callback_reentry}, {kCallbackFault, guest_callback_fault},
    };
#define HARNESS_MAP(name, id, args) result.push_back({ BROWSER_AUDIO_ADDRESS(name), weakAudio ? BROWSER_AUDIO_ORIGINAL(name) : BROWSER_AUDIO_GUEST(name) });
    BROWSER_AUDIO_ENTRIES(HARNESS_MAP)
    return result;
  }

  enum class Mode { Normal, WeakAudio, BrokenVtable, NoInstall, NoAudio, EffectsDisc };
  static std::shared_ptr<kernel::DiscIndex> EffectsDiscImage() {
    auto bytes = std::make_shared<std::vector<uint8_t>>(106496);
    const auto put = [&](uint32_t at, uint32_t value) { std::memcpy(bytes->data() + at, &value, 4); };
    std::memcpy(bytes->data() + 65536, "MICROSOFT*XBOX*MEDIA", 20);
    std::memcpy(bytes->data() + 65536 + 0x7ec, "MICROSOFT*XBOX*MEDIA", 20);
    put(65536 + 0x14, 40);
    put(65536 + 0x18, 64);
    put(81920 + 4, 50);
    put(81920 + 8, 0x1000);
    (*bytes)[81920 + 13] = 11;
    std::memcpy(bytes->data() + 81920 + 14, "dsstdfx.bin", 11);
    WriteEffectsTable(*bytes, 50 * 2048);
    auto disc = std::make_shared<kernel::DiscIndex>();
    std::string error;
    check(disc->Open(bytes->size(), [bytes](uint64_t at, void* out, uint32_t size) {
      std::memcpy(out, bytes->data() + at, size);
      return size;
    }, error), "invalid effects disc fixture");
    return disc;
  }
  static void WriteEffectsTable(std::vector<uint8_t>& bytes, uint32_t at) {
    const auto put = [&](uint32_t offset, uint32_t value) { std::memcpy(bytes.data() + at + offset, &value, 4); };
    put(0x818, 2);
    put(0x81C, 0x60000);
    put(0x824, 0x100);
    put(0x82C, 0x30);
    put(0x83C, 0x200);
    put(0x844, 0x40);
  }
  std::shared_ptr<kernel::DiscIndex> disc;
  explicit Fixture(Mode mode = Mode::Normal, std::shared_ptr<browser::AudioOutput> output = {}) : runtime(Mappings(mode == Mode::WeakAudio), {}, [this] {
    if (clockStep.load() && ++clockSamples > 256) { throw std::runtime_error("audio refill repeatedly resampled its advancing clock"); }
    return source.fetch_add(clockStep.load());
  }) {
    if (mode == Mode::EffectsDisc) { disc = EffectsDiscImage(); }
    image.memory = std::make_unique<browser::Memory>();
    image.memory->Ensure(0x10000, 4096);
    image.memory->Ensure(0x20000, 0x20000);
    image.memory->Ensure(0xF21000, 4096);
    writeVtables(*image.memory, mode == Mode::BrokenVtable);
    queue = browser::InstallTimerProvider(runtime, image);
    tick = browser::InstallTimerTickCount(runtime, image, queue);
    sink = std::make_shared<browser::SilentAudioSink>(runtime, image, std::move(output));
    if (mode != Mode::NoInstall && mode != Mode::NoAudio) {
      audio = browser::InstallAudioProvider(runtime, image, sink, disc);
    }
    browser::InstallTimerClockObserver(runtime, image, queue,
      [sink = sink, audio = audio](const X86Context& context, uint64_t now) {
        sink->Advance(context, now);
        if (audio) { audio->Tick(context, now); }
      });
    trace = browser::InstallSdkProvider(runtime, image, browser::SdkPolicy::Observer, {}, {}, {}, {}, audio);
    image.image.kernel_thunk = 0x10000;
    const std::array<uint32_t, 2> imports = {0x80000071, 0};
    image.memory->Write({}, 0x10000, imports.data(), sizeof(imports));
    image.imports = {{0x10000, 113}};
    browser::BindImageImports(image, runtime, std::span(&tick, 1));
    writeWave(*image.memory, kWavePcm, 1, 2, 48000, 4, 16);
    writeWave(*image.memory, kWaveAdpcm, 0x69, 1, 48000, 36, 4);
    writeWave(*image.memory, kWaveBadTag, 2, 2, 48000, 4, 16);
    writeWave(*image.memory, kWaveBadAlign, 1, 2, 48000, 8, 16);
  }
  ~Fixture() { runtime.Stop(); }

  void Run(const std::function<void(X86Context&, uint8_t*)>& operation) {
    const auto thread = browser::PrepareImageThread(image, runtime, uint32_t((image.threads.size() + 1) * 4), 0x10000);
    runtime.Launch(thread.context, image.memory->Base(), operation);
    if (const auto failure = runtime.Join()) { std::rethrow_exception(failure); }
  }
};

struct Guest {
  X86Context& ctx;
  uint8_t* base;
  browser::Memory& memory;
  uint32_t Call(X86Func* fn, std::initializer_list<uint32_t> args, uint32_t cleanupArgs) {
    std::array<uint32_t, 9> frame = {0xF3F000};
    uint32_t n = 0;
    for (const uint32_t arg : args) { frame[1 + n++] = arg; }
    memory.Write(ctx, kStack, frame.data(), (1 + n) * 4);
    ctx.esp = kStack;
    fn(ctx, base);
    check(ctx.esp == kStack + 4 * (1 + cleanupArgs), "audio entry stack cleanup not exact");
    return ctx.eax;
  }
  uint32_t Read(uint32_t address) {
    uint32_t value = 0;
    memory.Read(ctx, address, &value, 4);
    return value;
  }
  void Write(uint32_t address, uint32_t value) { memory.Write(ctx, address, &value, 4); }
  void WriteDesc(uint32_t address, std::initializer_list<uint32_t> values) {
    std::array<uint32_t, 8> words{};
    uint32_t n = 0;
    for (const uint32_t value : values) { words[n++] = value; }
    memory.Write(ctx, address, words.data(), n * 4);
  }
};

void installOwnership() {
  for (const auto mode : {Fixture::Mode::WeakAudio, Fixture::Mode::BrokenVtable}) {
    const auto original = originalCalls.load();
    refuseStage = "refuse #1";
    refuse([&] { Fixture fixture(mode); });
    check(originalCalls.load() == original, "rejected install observed retained originals");
  }
  {
    Fixture fixture(Fixture::Mode::NoInstall);
    const auto owner = fixture.image.diagnosticRuntime;
    refuseStage = "refuse #2";
    refuse([&] { browser::InstallAudioProvider(fixture.runtime, fixture.image, fixture.sink); });
    check(fixture.image.diagnosticRuntime == owner && !fixture.audio,
      "post-publication install rejection changed image ownership");
  }
  {
    Fixture fixture;
    refuseStage = "refuse #3";
    refuse([&] { browser::InstallAudioProvider(fixture.runtime, fixture.image, fixture.sink); });
    check(fixture.audio && fixture.audio->Snapshot().voices == 0, "double install rejection disturbed the live provider");
  }
  {
    Fixture first;
    Fixture second(Fixture::Mode::NoAudio);
    refuseStage = "refuse #4";
    refuse([&] { browser::InstallAudioProvider(second.runtime, second.image, first.sink); });
    refuseStage = "refuse #5";
    refuse([&] { browser::InstallAudioProvider(second.runtime, first.image, second.sink); });
  }
  {
    Fixture fixture(Fixture::Mode::NoAudio);
    fixture.Run([&](X86Context& context, uint8_t* base) {
      refuseStage = "refuse #6";
      refuse([&] { BROWSER_AUDIO_GUEST(DirectSoundDoWork)(context, base); });
      refuseStage = "refuse #7";
      refuse([&] { BROWSER_AUDIO_GUEST(DirectSoundCreate)(context, base); });
      check(originalCalls.load() == 0, "audio entry without provider reached retained hardware bodies");
      check(!fixture.image.memory->QueryPages(kApuWindow).mapped, "audio rejection mapped the APU window");
    });
  }
  {
    Fixture fixture;
    fixture.Run([&](X86Context& context, uint8_t* base) {
      const auto before = context;
      browser::Memory foreign;
      refuseStage = "refuse #8";
      refuse([&] { BROWSER_AUDIO_GUEST(DirectSoundDoWork)(context, foreign.Base()); });
      browser::Runtime foreignRuntime({}, {});
      auto alias = context;
      refuseStage = "refuse #9";
      refuse([&] { BROWSER_AUDIO_GUEST(DirectSoundDoWork)(alias, base); });
      alias.host = &foreignRuntime;
      refuseStage = "refuse #10";
      refuse([&] { BROWSER_AUDIO_GUEST(DirectSoundDoWork)(alias, base); });
      {
        auto lease = fixture.image.memory->Borrow();
        refuseStage = "refuse #11";
        refuse([&] { BROWSER_AUDIO_GUEST(DirectSoundDoWork)(context, base); });
      }
      check(!std::memcmp(&context, &before, sizeof(context)), "invalid audio admission mutated guest state");
      fixture.runtime.Cancel();
      refuseStage = "refuse #12";
      refuse([&] { BROWSER_AUDIO_GUEST(DirectSoundDoWork)(context, base); });
    });
  }
}

void createIdentity() {
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    const auto before = fixture.audio->Snapshot();
    check(before.device == 0 && before.voices == 0 && before.publishedCompletions == 0 && before.deliveredCallbacks == 0,
      "fresh audio provider published state");
    const auto noMutation = [&] {
      const auto snapshot = fixture.audio->Snapshot();
      check(snapshot.device == before.device && snapshot.voices == before.voices && snapshot.publishedCompletions == 0,
        "rejected create mutated provider state");
    };
    guest.WriteDesc(kBufferDesc, {0, 0, 0, kWavePcm, 0});
    refuseStage = "refuse #13";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, 0}, 2); });
    refuseStage = "refuse #14";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {0x20010, kOut}, 2); });
    refuseStage = "refuse #15";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {0x24FF0, kOut}, 2); });
    refuseStage = "refuse #16";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {0xFFFFFFFC, kOut}, 2); });
    guest.WriteDesc(kBufferDesc, {0, 0, 0, kWaveBadTag, 0});
    refuseStage = "refuse #18";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2); });
    guest.WriteDesc(kBufferDesc, {0, 0, 0, kWaveBadAlign, 0});
    refuseStage = "refuse #19";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2); });
    guest.WriteDesc(kBufferDesc, {0, 0, 0, 0, 0});
    refuseStage = "refuse #20";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2); });
    noMutation();
    refuseStage = "refuse #21";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreate), {0, 0, 0}, 3); });
    noMutation();
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreate), {0, kOut, 0}, 3) == 0, "DirectSoundCreate failed");
    const uint32_t device = guest.Read(kOut);
    check(device >= browser::AudioProvider::kArenaBase && device < browser::AudioProvider::kArenaBase + browser::AudioProvider::kArenaCapacity,
      "device object outside the audio arena");
    for (uint32_t n = 0; n < browser::AudioProvider::kDeviceBytes; n += 4) {
      check(guest.Read(device + n) == 0, "device object not zeroed");
    }
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreate), {0, kOut + 4, 0}, 3) == 0 && guest.Read(kOut + 4) == device,
      "DirectSoundCreate is not a session singleton");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_Release), {device}, 1) == 0, "device release failed");
    check(guest.Read(kOut + 4) == device, "device release retired the singleton");
    uint32_t speaker = 0xdeadbeef;
    fixture.image.memory->Write(context, kOut + 8, &speaker, 4);
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_GetSpeakerConfig), {device, kOut + 8}, 2) == 0 && guest.Read(kOut + 8) == 0,
      "speaker config is not the cold boot stereo value");
    refuseStage = "refuse #22";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_GetSpeakerConfig), {device, 0}, 2); });
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundUseFullHRTF), {}, 0) == 0, "HRTF selection no-op parity failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_CommitDeferredSettings), {device}, 1) == 0, "commit deferred failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_SetMixBinHeadroom), {device, 3, 700}, 3) == 0, "mix bin headroom failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_SetMixBinHeadroom), {device, 33, 700}, 3) == 0, "out of range headroom bin not ignored");
    guest.WriteDesc(kBufferDesc, {0, 0, 0, kWavePcm, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut + 12}, 2) == 0, "empty buffer creation must stay valid pending SetBufferData");
    const uint32_t empty = guest.Read(kOut + 12);
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_CreateSoundBuffer), {device, kBufferDesc, kOut + 16, 0}, 4) == 0,
      "CreateSoundBuffer sibling failed");
    for (uint32_t n = 0; n < browser::AudioProvider::kBufferObjectBytes; n += 4) {
      check(guest.Read(empty + n) == 0, "buffer object not zeroed");
    }
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {empty}, 1) == 0, "buffer release failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {guest.Read(kOut + 16)}, 1) == 0, "sibling buffer release failed");
    check(fixture.audio->Snapshot().device == device, "buffer churn disturbed the device");
    check(originalCalls.load() == 0 && !fixture.image.memory->QueryPages(kApuWindow).mapped,
      "create path reached retained bodies or hardware");
  });
}

void deviceControls() {
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreate), {0, kOut, 0}, 3) == 0, "create failed");
    const uint32_t device = guest.Read(kOut);
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_EnableHeadphones), {device, 1}, 2) == 0, "EnableHeadphones failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_SetRolloffFactor), {device, 0x3F800000, 0}, 3) == 0, "SetRolloffFactor failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_SetDopplerFactor), {device, 0x3F800000, 0}, 3) == 0, "SetDopplerFactor failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_SetVelocity), {device, 0, 0, 0, 1}, 5) == 0, "device SetVelocity failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_SetPosition), {device, 0x3F800000, 0, 0, 1}, 5) == 0, "device SetPosition failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_SetOrientation), {device, 0x3F800000, 0, 0, 0, 0x3F800000, 0, 1}, 8) == 0,
      "SetOrientation failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_SetEffectData), {device, kPayload, 0x20, kPayload + 16, 4, 0}, 6) == 0,
      "SetEffectData failed");
    guest.WriteDesc(kOut + 16, {1000, 2000, 0x3F800000, 0x40000000, 0x3F000000, 3000, 0x3DCCCCCD, 4000});
    guest.WriteDesc(kOut + 48, {0x3E4CCCCD, 0x3F666666, 0x42C80000, 0x45BB8000, 0, 0, 0, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_SetI3DL2Listener), {device, kOut + 16, 0}, 3) == 0, "SetI3DL2Listener failed");
    REFUSE_AT("refuse #35", [&] { guest.Call(BROWSER_AUDIO_GUEST(IDirectSound_SetI3DL2Listener), {device, 0, 0}, 3); });
    check(fixture.audio->Snapshot().voices == 0, "device controls leaked voices");
    check(originalCalls.load() == 0 && !fixture.image.memory->QueryPages(kApuWindow).mapped,
      "device controls reached retained bodies or hardware");
  });
}

void voicePositionalControls() {
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    guest.WriteDesc(kBufferDesc, {0, 0x10, 9600, kWavePcm, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2) == 0, "3D buffer create failed");
    const uint32_t buffer = guest.Read(kOut);
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetPosition), {buffer, 0x3F800000, 0, 0, 1}, 5) == 0, "buffer SetPosition failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetVelocity), {buffer, 0, 0, 0, 1}, 5) == 0, "buffer SetVelocity failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetMinDistance), {buffer, 0x3F800000, 0}, 3) == 0, "buffer SetMinDistance failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetMaxDistance), {buffer, 0x42C80000, 0}, 3) == 0, "buffer SetMaxDistance failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetFilter), {buffer, kPayload}, 2) == 0, "buffer SetFilter failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetI3DL2Source), {buffer, 0, 0}, 3) == 0,
      "null I3DL2 source must be a no-op success");
    guest.WriteDesc(kOut + 16, {1000, 2000, 3000, 0, 250, 0x3F000000, 500, 0x3E800000});
    guest.Write(kOut + 48, 0);
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetI3DL2Source), {buffer, kOut + 16, 0}, 3) == 0, "buffer SetI3DL2Source failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetPosition), {kPayload, 0, 0, 0, 1}, 5) != 0,
      "positional setter accepted a foreign voice");
    guest.WriteDesc(kStreamDesc, {0x10, 2, kWavePcm, kCallback, 0xA5A5, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut + 4}, 2) == 0, "3D stream create failed");
    const uint32_t stream = guest.Read(kOut + 4);
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_SetPosition), {stream, 0x3F800000, 0, 0, 1}, 5) == 0, "stream SetPosition failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_SetVelocity), {stream, 0, 0, 0, 1}, 5) == 0, "stream SetVelocity failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_SetMinDistance), {stream, 0x3F800000, 0}, 3) == 0, "stream SetMinDistance failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_SetMaxDistance), {stream, 0x42C80000, 0}, 3) == 0, "stream SetMaxDistance failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_SetFilter), {stream, kPayload}, 2) == 0, "stream SetFilter failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_SetI3DL2Source), {stream, 0, 0}, 3) == 0,
      "stream null I3DL2 source must be a no-op success");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_SetI3DL2Source), {stream, kOut + 16, 0}, 3) == 0,
      "stream SetI3DL2Source failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Release), {stream}, 1) == 0, "stream release failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {buffer}, 1) == 0, "buffer release failed");
    check(originalCalls.load() == 0 && !fixture.image.memory->QueryPages(kApuWindow).mapped,
      "positional controls reached retained bodies or hardware");
  });
}

void effectsDownload() {
  Fixture fixture(Fixture::Mode::EffectsDisc);
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    auto& memory = *fixture.image.memory;
    const auto writeName = [&](uint32_t address, const char* text) {
      memory.Write(context, address, text, std::strlen(text) + 1);
    };
    const auto writeTable = [&](uint32_t at) {
      std::vector<uint8_t> table(0x1000);
      Fixture::WriteEffectsTable(table, 0);
      memory.Write(context, at, table.data(), table.size());
    };
    const auto expectPlannedDescriptor = [&](uint32_t desc) {
      check(desc >= browser::AudioProvider::kArenaBase && desc < browser::AudioProvider::kArenaBase + browser::AudioProvider::kArenaCapacity,
        "effects descriptor outside the audio arena");
      check(desc % 16 == 0, "effects descriptor is not arena aligned");
      check(guest.Read(desc) == 2 && guest.Read(desc + 4) == 0x60000, "effects descriptor header wrong");
      const std::array<uint32_t, 8> sizes = {0x100, 0x30, 0, 0x200, 0x40, 0, 0, 0};
      for (uint32_t n = 0; n < 8; ++n) {
        const uint32_t segment = guest.Read(desc + 8 + n * 8);
        check(segment >= browser::AudioProvider::kArenaBase && segment % 16 == 0, "effect segment outside the audio arena");
        check(guest.Read(desc + 8 + n * 8 + 4) == sizes[n], "effect segment declared size wrong");
        check(guest.Read(segment) == 0, "effect segment not zero filled");
      }
    };
    const uint32_t arenaStart = fixture.audio->Snapshot().arenaFreeBytes;
    writeTable(0x28000);
    fixture.image.image.sections.push_back({"fxsec", 0x38, 0x28000, 0x1000, 0, 0});
    writeName(kPayload, "fxsec");
    check(guest.Call(BROWSER_AUDIO_GUEST(XAudioDownloadEffectsImage), {kPayload, 0, 1, kOut}, 4) == 0,
      "loaded section effects download failed");
    expectPlannedDescriptor(guest.Read(kOut));
    check(fixture.audio->Snapshot().arenaFreeBytes == arenaStart - 1232, "effects arena accounting wrong");
    writeName(kPayload, "d:\\dsstdfx.bin");
    check(guest.Call(BROWSER_AUDIO_GUEST(XAudioDownloadEffectsImage), {kPayload, kOut + 64, 0, kOut + 4}, 4) == 0,
      "disc effects download failed");
    expectPlannedDescriptor(guest.Read(kOut + 4));
    check(guest.Read(kOut + 4) != guest.Read(kOut), "effects downloads must not alias; retention requires fresh ownership");
    for (const char* path : {"dsstdfx.bin", "\\??\\D:\\dsstdfx.bin", "\\Device\\CdRom0\\dsstdfx.bin", "D:/dsstdfx.bin"}) {
      writeName(kPayload, path);
      check(guest.Call(BROWSER_AUDIO_GUEST(XAudioDownloadEffectsImage), {kPayload, 0, 0, kOut + 4}, 4) == 0,
        "CD-ROM effects path failed");
      expectPlannedDescriptor(guest.Read(kOut + 4));
    }
    for (const char* path : {"C:\\dsstdfx.bin", "D:dsstdfx.bin", "\\Device\\Harddisk0\\dsstdfx.bin"}) {
      writeName(kPayload, path);
      check(guest.Call(BROWSER_AUDIO_GUEST(XAudioDownloadEffectsImage), {kPayload, 0, 0, kOut + 4}, 4) == 0 &&
        guest.Read(guest.Read(kOut + 4)) == 0, "non-CD-ROM effects path acquired disc bytes");
    }
    memory.Map(0xF22000, 4096);
    fixture.image.image.sections.push_back({"unloadedfx", 0x38, 0xF22000, 0x1000, 0, 0});
    writeName(kPayload, "unloadedfx");
    check(guest.Call(BROWSER_AUDIO_GUEST(XAudioDownloadEffectsImage), {kPayload, 0, 1, kOut + 8}, 4) == 0,
      "unloaded section download must follow native committed-reservation parity");
    const uint32_t unloaded = guest.Read(kOut + 8);
    check(unloaded != 0 && guest.Read(unloaded) == 0, "unloaded section must yield a count-0 descriptor");
    check(guest.Read(unloaded + 8) == 0 && guest.Read(unloaded + 12) == 0 && guest.Read(unloaded + 36) == 0,
      "count-0 descriptor entry must stay zero filled");
    writeName(kPayload, "nosuchfile.bin");
    check(guest.Call(BROWSER_AUDIO_GUEST(XAudioDownloadEffectsImage), {kPayload, 0, 0, kOut + 12}, 4) == 0,
      "missing disc file must yield count-0 success");
    check(guest.Read(guest.Read(kOut + 12)) == 0, "missing file descriptor count wrong");
    check(guest.Read(kOut + 12) != guest.Read(kOut + 8), "count-0 downloads must not alias retained descriptors");
    check(guest.Call(BROWSER_AUDIO_GUEST(XAudioDownloadEffectsImage), {0, 0, 0, kOut + 16}, 4) == 0, "null name must succeed");
    writeTable(0x2A000);
    guest.Write(0x2A000 + 0x818, 1);
    guest.Write(0x2A000 + 0x824, 0x400000);
    fixture.image.image.sections.push_back({"hugefx", 0x38, 0x2A000, 0x1000, 0, 0});
    writeName(kPayload, "hugefx");
    const uint32_t arenaBeforeFault = fixture.audio->Snapshot().arenaFreeBytes;
    guest.Write(kOut + 20, 0xdeadbeef);
    REFUSE_AT("refuse #36", [&] { guest.Call(BROWSER_AUDIO_GUEST(XAudioDownloadEffectsImage), {kPayload, 0, 1, kOut + 20}, 4); });
    check(fixture.audio->Snapshot().arenaFreeBytes == arenaBeforeFault, "budget-exceeded download left partial arena ownership");
    check(guest.Read(kOut + 20) == 0xdeadbeef, "budget-exceeded download wrote the output pointer");
    check(originalCalls.load() == 0 && !fixture.image.memory->QueryPages(kApuWindow).mapped,
      "effects download reached retained bodies or hardware");
  });
}

void bufferLifecycle() {
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    auto& sink = *fixture.sink;
    const auto drive = [&](uint64_t now) { fixture.source = now; fixture.queue->Drive(context, base); };
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreate), {0, kOut, 0}, 3) == 0, "create failed");
    guest.WriteDesc(kBufferDesc, {0, 0, 96000, kWavePcm, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut + 4}, 2) == 0, "buffer create failed");
    const uint32_t buffer = guest.Read(kOut + 4);
    const auto freeAfterCreate = fixture.audio->Snapshot().arenaFreeBytes;
    check(freeAfterCreate < browser::AudioProvider::kArenaCapacity, "buffer payload did not consume arena");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetStatus), {buffer, kOut + 20}, 2) == 0 && guest.Read(kOut + 20) == 0,
      "fresh buffer reported playing");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetCurrentPosition), {buffer, kOut + 24, kOut + 28}, 3) == 0 &&
      guest.Read(kOut + 24) == 0 && guest.Read(kOut + 28) == 1024 % 96000, "fresh buffer positions wrong");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Play), {buffer, 0, 0, 0}, 4) == 0, "play failed");
    check(sink.Snapshot().queuedFrames == browser::SilentAudioSink::kCapacityFrames, "playing buffer did not feed the sink clock");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetStatus), {buffer, kOut + 20}, 2) == 0 && guest.Read(kOut + 20) == 1,
      "playing status bit missing");
    drive(125000);
    check(sink.Snapshot().consumedFrames == 600, "sink consumption not real clock driven");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetCurrentPosition), {buffer, kOut + 24, kOut + 28}, 3) == 0 &&
      guest.Read(kOut + 24) == 2400 && guest.Read(kOut + 28) == 3424, "buffer cursors do not derive from consumed frames");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetCurrentPosition), {buffer, 4000}, 2) == 0, "set position failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetCurrentPosition), {buffer, kOut + 24, 0}, 3) == 0 && guest.Read(kOut + 24) == 4000,
      "position override lost");
    refuseStage = "refuse #24";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetCurrentPosition), {buffer, 96004}, 2); });
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetLoopRegion), {buffer, 4000, 8000}, 3) == 0, "loop region failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetLoopRegion), {buffer, 90000, 8000}, 3) == 0x88780032,
      "out of range loop region not a guest error");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Play), {buffer, 0, 0, 1}, 4) == 0, "looping play failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetStatus), {buffer, kOut + 20}, 2) == 0 && guest.Read(kOut + 20) == 5,
      "looping status bits wrong");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Lock), {buffer, 95000, 2000, kOut + 32, kOut + 36, kOut + 40, kOut + 44, 0}, 8) == 0,
      "lock failed");
    const uint32_t first = guest.Read(kOut + 32), firstBytes = guest.Read(kOut + 36);
    check(guest.Read(kOut + 40) != 0 && guest.Read(kOut + 44) == 1000 && firstBytes == 1000, "lock wrap split wrong");
    check(first >= browser::AudioProvider::kArenaBase && first - browser::AudioProvider::kArenaBase < browser::AudioProvider::kArenaCapacity,
      "lock pointer outside the audio arena");
    check(guest.Read(kOut + 40) == first - 95000, "lock wrap second pointer not the payload base");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Lock), {buffer, 100, 200, kOut + 32, kOut + 36, kOut + 40, kOut + 44, 1}, 8) == 0 &&
      guest.Read(kOut + 36) == 95900 && guest.Read(kOut + 44) == 100, "entire buffer lock wrong");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Unlock), {buffer, first, 1000, guest.Read(kOut + 40), 1000}, 5) == 0, "unlock failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetVolume), {buffer, uint32_t(-600)}, 2) == 0, "volume failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetFrequency), {buffer, 24000}, 2) == 0, "frequency failed");
    refuseStage = "refuse #25";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetFrequency), {buffer, 500000}, 2); });
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetHeadroom), {buffer, 300}, 2) == 0, "headroom failed");
    guest.WriteDesc(kMixBins, {2, kMixBins + 16});
    guest.WriteDesc(kMixBins + 16, {0, 0});
    guest.WriteDesc(kMixBins + 24, {1, uint32_t(-300)});
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetMixBins), {buffer, kMixBins}, 2) == 0, "mix bins failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetMixBinVolumes), {buffer, kMixBins}, 2) == 0, "mix bin volumes failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetMixBins), {buffer, 0}, 2) == 0, "null mix bins failed");
    drive(5000000);
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetStatus), {buffer, kOut + 20}, 2) == 0 && guest.Read(kOut + 20) == 5,
      "looping buffer stopped");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Stop), {buffer}, 1) == 0, "stop failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetStatus), {buffer, kOut + 20}, 2) == 0 && guest.Read(kOut + 20) == 0,
      "stopped buffer reported playing");
    drive(20000000);
    drive(40000000);
    const auto frozen = sink.Snapshot().consumedFrames;
    drive(60000000);
    check(sink.Snapshot().consumedFrames == frozen, "silent clock advanced without a sounding voice");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetBufferData), {buffer, kPayload, 4800}, 3) == 0, "set buffer data failed");
    check(fixture.audio->Snapshot().arenaFreeBytes > freeAfterCreate, "owned payload not freed on guest data replace");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetBufferData), {buffer, kPayload, 4800}, 3) == 0, "idempotent set buffer data failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Lock), {buffer, 0, 24, kOut + 32, kOut + 36, 0, 0, 0}, 8) == 0 &&
      guest.Read(kOut + 32) == kPayload && guest.Read(kOut + 36) == 24, "guest payload lock wrong");
    refuseStage = "refuse #26";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetBufferData), {buffer, kPayload, 0x30000}, 3); });
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetBufferData), {buffer, 0, 4800}, 3) == 0, "owned realloc failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Lock), {buffer, 0, 24, kOut + 32, kOut + 36, 0, 0, 0}, 8) == 0 &&
      guest.Read(kOut + 32) != kPayload, "reallocated owned payload still aliases guest data");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Play), {buffer, 0, 0, 2}, 4) == 0, "restart from beginning failed");
    drive(70000000);
    const auto freedBefore = fixture.audio->Snapshot().arenaFreeBytes;
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {buffer}, 1) == 0, "final release failed");
    check(fixture.audio->Snapshot().arenaFreeBytes > freedBefore, "final release did not free the owned payload");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetStatus), {buffer, kOut + 20}, 2) == 0x80004005,
      "released buffer still resolves");
    check(originalCalls.load() == 0, "buffer lifecycle reached retained bodies");
  });
}

void streamPackets() {
  callbackEvents.clear();
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    auto& sink = *fixture.sink;
    const auto drive = [&](uint64_t now) { fixture.source = now; fixture.queue->Drive(context, base); };
    guest.WriteDesc(kStreamDesc, {0, 4, kWavePcm, kCallback, 0xA5A5, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut}, 2) == 0, "stream create failed");
    const uint32_t stream = guest.Read(kOut);
    check(guest.Read(stream) == kVtable && guest.Read(stream + 4) == kVtable2, "stream object lacks the retained vtables");
    for (uint32_t n = 8; n < browser::AudioProvider::kStreamObjectBytes; n += 4) {
      check(guest.Read(stream + n) == 0, "stream object not zeroed");
    }
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_GetInfo), {stream, kOut + 4}, 2) == 0 &&
      guest.Read(kOut + 4) == 3 && guest.Read(kOut + 8) == 4 && guest.Read(kOut + 12) == 0 && guest.Read(kOut + 16) == 0,
      "stream info wrong");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_GetStatus), {stream, kOut + 20}, 2) == 0 && guest.Read(kOut + 20) == 1,
      "empty stream not ready");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_AddRef), {stream}, 1) == 2, "addref count wrong");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Release), {stream}, 1) == 1, "release count wrong");
    guest.WriteDesc(kPacketDesc, {kPayload, 4800, kOut + 24, kOut + 28, 0x5A5A});
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3) == 0, "packet queue failed");
    check(guest.Read(kOut + 24) == 0 && guest.Read(kOut + 28) == 0x8000000A, "packet pending publication wrong");
    check(fixture.audio->Snapshot().pendingCallbacks == 0, "packet callback delivered before completion");
    drive(125000);
    check(guest.Read(kOut + 28) == 0x8000000A && guest.Read(kOut + 24) == 0, "packet completed before its real duration");
    drive(250000);
    check(guest.Read(kOut + 28) == 0 && guest.Read(kOut + 24) == 4800, "packet completion did not publish real bytes");
    check(fixture.audio->Snapshot().publishedCompletions == 1 && fixture.audio->Snapshot().pendingCallbacks == 1,
      "completion publication accounting wrong");
    {
      std::lock_guard lock(callbackLock);
      check(callbackEvents.empty(), "packet callback delivered outside DoWork");
    }
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundDoWork), {}, 0) == 0, "DoWork failed");
    {
      std::lock_guard lock(callbackLock);
      check(callbackEvents.size() == 1 && callbackEvents[0].context == 0xA5A5 && callbackEvents[0].packetContext == 0x5A5A &&
        callbackEvents[0].status == 0, "DoWork callback arguments wrong");
    }
    check(fixture.audio->Snapshot().deliveredCallbacks == 1, "delivery accounting wrong");
    guest.WriteDesc(kPacketDesc, {kPayload, 4800, kOut + 24, kOut + 28, 0x5B5B});
    guest.WriteDesc(kPacketDesc + 32, {kPayload, 2400, kOut + 32, kOut + 36, 0x5C5C});
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3) == 0, "second packet failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc + 32, 0}, 3) == 0, "third packet failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Pause), {stream, 1}, 2) == 0, "pause failed");
    const auto pausedConsumed = sink.Snapshot().consumedFrames;
    drive(400000);
    drive(400000000);
    check(guest.Read(kOut + 28) == 0x8000000A, "paused stream completed a packet");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Pause), {stream, 0}, 2) == 0, "resume failed");
    drive(400250000);
    check(guest.Read(kOut + 28) == 0 && guest.Read(kOut + 24) == 4800 && guest.Read(kOut + 36) == 0x8000000A,
      "resume did not continue packet timing");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Flush), {stream}, 1) == 0, "flush failed");
    check(guest.Read(kOut + 36) == 0x80004004 && guest.Read(kOut + 32) == 0, "flush publication wrong");
    {
      std::lock_guard lock(callbackLock);
      check(callbackEvents.size() == 3 && callbackEvents[1].status == 0 && callbackEvents[2].status == 0x80004004 &&
        callbackEvents[2].packetContext == 0x5C5C, "flush callback did not run on the flushing call");
    }
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_FlushEx), {stream, 0, 0, 1}, 4) == 0, "inactive FlushEx must be a no-op");
    guest.WriteDesc(kPacketDesc, {kPayload, 4800, kOut + 24, kOut + 28, 0x5D5D});
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3) == 0, "requeue failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Discontinuity), {stream}, 1) == 0, "discontinuity failed");
    drive(400500000);
    check(guest.Read(kOut + 28) == 0 && guest.Read(kOut + 24) == 4800, "ending packet did not complete with success");
    {
      std::lock_guard lock(callbackLock);
      check(callbackEvents.size() == 3, "ending callback delivered without a pump");
    }
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundDoWork), {}, 0) == 0, "DoWork failed");
    {
      std::lock_guard lock(callbackLock);
      check(callbackEvents.size() == 4 && callbackEvents[3].status == 0, "discontinuity completion lost");
    }
    guest.WriteDesc(kPacketDesc, {kPayload, 4800, kOut + 24, kOut + 28, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3) == 0, "queue before release failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Release), {stream}, 1) == 0, "final stream release failed");
    check(guest.Read(kOut + 28) == 0x80004004, "release did not flush pending packets");
    {
      std::lock_guard lock(callbackLock);
      check(callbackEvents.size() == 5 && callbackEvents[4].status == 0x80004004, "release did not run the flushed callback");
    }
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_GetStatus), {stream, kOut + 20}, 2) == 0x80004005,
      "released stream still resolves");
    check(fixture.audio->Snapshot().voices == 0, "released stream retained its voice");
    check(originalCalls.load() == 0 && !fixture.image.memory->QueryPages(kApuWindow).mapped,
      "stream path reached retained bodies or hardware");
  });
}

void streamRules() {
  callbackEvents.clear();
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    guest.WriteDesc(kStreamDesc, {0, 1, kWaveAdpcm, 0, 0, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut}, 2) == 0, "adpcm stream create failed");
    const uint32_t stream = guest.Read(kOut);
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_GetInfo), {stream, kOut + 4}, 2) == 0 && guest.Read(kOut + 8) == 36,
      "adpcm stream packet size not the 64 frame block");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_GetStatus), {stream, kOut + 20}, 2) == 0 && guest.Read(kOut + 20) == 1,
      "single packet stream not ready");
    guest.WriteDesc(kPacketDesc, {kPayload, 720, kOut + 24, kOut + 28, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3) == 0, "adpcm packet failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_GetStatus), {stream, kOut + 20}, 2) == 0 && guest.Read(kOut + 20) == 0,
      "full stream reported ready");
    guest.WriteDesc(kPacketDesc, {0, 720, kOut + 24, kOut + 28, 0});
    refuseStage = "refuse #27";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3); });
    guest.WriteDesc(kPacketDesc, {kPayload, 100, kOut + 24, kOut + 28, 0});
    refuseStage = "refuse #28";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3); });
    guest.WriteDesc(kPacketDesc, {kPayload, 721, kOut + 24, kOut + 28, 0});
    refuseStage = "refuse #29";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3); });
    check(fixture.audio->Snapshot().publishedCompletions == 0, "rejected packets published completions");
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundDoWork), {}, 0) == 0, "DoWork failed");
    {
      std::lock_guard lock(callbackLock);
      check(callbackEvents.empty(), "callback-less stream delivered callbacks");
    }
    fixture.source = 250000000;
    fixture.queue->Drive(context, base);
    check(guest.Read(kOut + 28) == 0 && guest.Read(kOut + 24) == 720, "callback-less completion not published");
    guest.WriteDesc(kStreamDesc, {0, 1, kWavePcm, 0, 0x7777, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut + 40}, 2) == 0, "event stream create failed");
    const uint32_t eventStream = guest.Read(kOut + 40);
    guest.WriteDesc(kPacketDesc, {kPayload, 4800, kOut + 24, kOut + 28, 0x1234});
    refuseStage = "refuse #30";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {eventStream, kPacketDesc, 0}, 3); });
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Pause), {stream, 1}, 2) == 0, "pause failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Play), {stream, 0, 0, 0}, 4) == 0x80004005,
      "buffer method on a stream voice not a guest error");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {0x24400, kPacketDesc, 0}, 3) == 0x80004005,
      "unknown voice not a guest error");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_AddRef), {eventStream}, 1) == 2, "event stream addref failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Release), {eventStream}, 1) == 1, "event stream release failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Release), {eventStream}, 1) == 0, "event stream final release failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Release), {stream}, 1) == 0, "stream final release failed");
    check(originalCalls.load() == 0, "stream rules reached retained bodies");
  });
}

void packetBudget() {
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    guest.WriteDesc(kStreamDesc, {0, 64, kWavePcm, 0, 0, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut}, 2) == 0, "stream create failed");
    const uint32_t stream = guest.Read(kOut);
    guest.WriteDesc(kPacketDesc, {kPayload, 4, 0, 0, 0});
    for (uint32_t n = 0; n < 32; ++n) {
      check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3) == 0, "bounded packet queue failed");
    }
    refuseStage = "refuse #31";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3); });
    check(fixture.audio->Snapshot().voices == 1, "packet budget rejection lost the stream");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Release), {stream}, 1) == 0, "release after budget failure failed");
    const auto freeBefore = fixture.audio->Snapshot().arenaFreeBytes;
    std::array<uint32_t, browser::audio::kMaxVoices> buffers{};
    for (uint32_t n = 0; n < buffers.size(); ++n) {
      guest.WriteDesc(kBufferDesc, {0, 0, 0, kWavePcm, 0});
      check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut + 4}, 2) == 0, "voice budget create failed");
      buffers[n] = guest.Read(kOut + 4);
    }
    const auto full = fixture.audio->Snapshot();
    guest.Write(kOut + 4, 0xdeadbeef);
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut + 4}, 2) == browser::audio::kGuestErrorFail,
      "voice budget must return a recoverable SDK failure");
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut + 4}, 2) == browser::audio::kGuestErrorFail,
      "stream voice budget must return a recoverable SDK failure");
    const auto rejected = fixture.audio->Snapshot();
    check(guest.Read(kOut + 4) == 0xdeadbeef && rejected.voices == full.voices && rejected.arenaFreeBytes == full.arenaFreeBytes,
      "voice budget failure published output or consumed storage");
    check(rejected.voiceLimitRejections == full.voiceLimitRejections + 2, "voice budget rejections are not observable");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {buffers[0]}, 1) == 0, "budget recovery release failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut + 4}, 2) == 0, "stream could not reuse a released voice");
    check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Release), {guest.Read(kOut + 4)}, 1) == 0, "recovered stream release failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut + 4}, 2) == 0, "buffer could not reuse a released voice");
    buffers[0] = guest.Read(kOut + 4);
    for (const auto buffer : buffers) { check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {buffer}, 1) == 0, "budget fixture release failed"); }
    for (uint32_t n = 0; n < 256; ++n) {
      check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut + 4}, 2) == 0, "audio lifetime stress create failed");
      check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {guest.Read(kOut + 4)}, 1) == 0, "audio lifetime stress release failed");
    }
    check(fixture.audio->Snapshot().voices == 0 && fixture.audio->Snapshot().arenaFreeBytes == freeBefore,
      "audio lifetime stress leaked voices or storage");
  });
}

void callbacksReentryFailure() {
  callbackEvents.clear();
  {
    Fixture fixture;
    fixture.Run([&](X86Context& context, uint8_t* base) {
      Guest guest{context, base, *fixture.image.memory};
      guest.WriteDesc(kStreamDesc, {0, 2, kWavePcm, kCallbackReentry, 0xB1B1, 0});
      check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut}, 2) == 0, "stream create failed");
      const uint32_t stream = guest.Read(kOut);
      reentryObject = stream;
      reentryOutput = kOut + 60;
      guest.WriteDesc(kPacketDesc, {kPayload, 4800, kOut + 24, kOut + 28, 0xC1C1});
      check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3) == 0, "packet failed");
      fixture.source = 250000;
      fixture.queue->Drive(context, base);
      check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundDoWork), {}, 0) == 0, "DoWork failed");
      check(reentryResult == 0 && guest.Read(kOut + 60) == 1, "callback reentry into the provider failed");
      {
        std::lock_guard lock(callbackLock);
        check(callbackEvents.size() == 1 && callbackEvents[0].context == 0xB1B1 && callbackEvents[0].packetContext == 0xC1C1,
          "reentry callback arguments wrong");
      }
      check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Release), {stream}, 1) == 0, "release failed");
    });
  }
  {
    callbackEvents.clear();
    Fixture fixture;
    bool propagated = false;
    const auto thread = [&] {
      const auto thread = browser::PrepareImageThread(fixture.image, fixture.runtime, uint32_t((fixture.image.threads.size() + 1) * 4), 0x10000);
      fixture.runtime.Launch(thread.context, fixture.image.memory->Base(), [&](X86Context& context, uint8_t* base) {
        Guest guest{context, base, *fixture.image.memory};
        guest.WriteDesc(kStreamDesc, {0, 2, kWavePcm, kCallbackFault, 0xD1D1, 0});
        check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut}, 2) == 0, "stream create failed");
        const uint32_t stream = guest.Read(kOut);
        guest.WriteDesc(kPacketDesc, {kPayload, 4800, kOut + 24, kOut + 28, 0xE1E1});
        check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3) == 0, "packet failed");
        fixture.source = 250000;
        fixture.queue->Drive(context, base);
        BROWSER_AUDIO_GUEST(DirectSoundDoWork)(context, base);
      });
      try {
        if (const auto failure = fixture.runtime.Join()) { std::rethrow_exception(failure); }
      } catch (const browser::Fault& fault) {
        propagated = std::string(fault.what()).find("guest stream callback failure") != std::string::npos;
      }
    };
    thread();
    check(propagated, "faulting guest callback was swallowed as success");
  }
}

void emptyAndFrames() {
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    guest.WriteDesc(kBufferDesc, {0, 0, 0, kWavePcm, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2) == 0, "empty buffer create failed");
    const uint32_t empty = guest.Read(kOut);
    refuseStage = "refuse #33";
    refuse([&] { guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Play), {empty, 0, 0, 0}, 4); });
    check(fixture.sink->Snapshot().submittedFrames == 0, "rejected play fed the sink");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Lock), {empty, 0, 0, kOut + 32, kOut + 36, 0, 0, 0}, 8) == 0x80004005,
      "empty buffer lock not a guest error");
    guest.WriteDesc(kBufferDesc, {0, 0, 3600, kWaveAdpcm, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut + 4}, 2) == 0, "adpcm buffer create failed");
    const uint32_t adpcm = guest.Read(kOut + 4);
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Play), {adpcm, 0, 0, 0}, 4) == 0, "adpcm play failed");
    fixture.source = 133334;
    fixture.queue->Drive(context, base);
    check(fixture.sink->Snapshot().consumedFrames == 640, "adpcm consumption not clock exact");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_GetCurrentPosition), {adpcm, kOut + 24, kOut + 28}, 3) == 0 &&
      guest.Read(kOut + 24) == 360 && guest.Read(kOut + 28) == 504,
      "adpcm cursors do not use the 64 frame block math");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {adpcm}, 1) == 0, "adpcm release failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {empty}, 1) == 0, "empty release failed");
  });
}

void sessionIsolation() {
  uint32_t firstDevice = 0;
  {
    Fixture fixture;
    fixture.Run([&](X86Context& context, uint8_t* base) {
      Guest guest{context, base, *fixture.image.memory};
      check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreate), {0, kOut, 0}, 3) == 0, "create failed");
      firstDevice = guest.Read(kOut);
      guest.WriteDesc(kBufferDesc, {0, 0, 9600, kWavePcm, 0});
      check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut + 4}, 2) == 0, "buffer create failed");
    });
  }
  {
    Fixture fixture;
    const auto snapshot = fixture.audio->Snapshot();
    check(snapshot.device == 0 && snapshot.voices == 0 && snapshot.publishedCompletions == 0 && snapshot.deliveredCallbacks == 0,
      "fresh session inherited audio state");
    fixture.Run([&](X86Context& context, uint8_t* base) {
      Guest guest{context, base, *fixture.image.memory};
      check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreate), {0, kOut, 0}, 3) == 0, "second session create failed");
      check(guest.Read(kOut) == firstDevice, "fresh arena did not reuse its deterministic base");
      uint32_t word = 0;
      fixture.image.memory->Read(context, firstDevice, &word, 4);
      check(word == 0, "fresh device object not zeroed");
    });
  }
}

void bufferDataOwnership() {
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    const uint32_t full = fixture.audio->Snapshot().arenaFreeBytes;
    guest.WriteDesc(kBufferDesc, {0, 0, 9600, kWavePcm, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2) == 0, "owned buffer create failed");
    const uint32_t buffer = guest.Read(kOut);
    check(fixture.audio->Snapshot().arenaFreeBytes < full, "provider payload did not consume the arena");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Lock), {buffer, 0, 9600, kOut + 4, kOut + 8, 0, 0, 0}, 8) == 0,
      "owned payload lock failed");
    const uint32_t owned = guest.Read(kOut + 4);
    check(owned >= browser::AudioProvider::kArenaBase && owned < browser::AudioProvider::kArenaBase + (4u << 20),
      "provider payload is outside its arena");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Unlock), {buffer, owned, 9600, 0, 0}, 5) == 0, "unlock failed");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetBufferData), {buffer, owned, 4800}, 3) == 0,
      "owned payload resize failed");
    check(fixture.audio->Snapshot().arenaFreeBytes < full, "owned payload resize dropped the live payload");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Lock), {buffer, 0, 4800, kOut + 4, kOut + 8, 0, 0, 0}, 8) == 0 &&
      guest.Read(kOut + 4) == owned && guest.Read(kOut + 8) == 4800, "resized buffer does not lock the owned payload");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {buffer}, 1) == 0, "owned buffer release failed");
    check(fixture.audio->Snapshot().arenaFreeBytes == full, "release leaked the retained provider payload");
    guest.WriteDesc(kBufferDesc, {0, 0, 9600, kWavePcm, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2) == 0, "second buffer create failed");
    const uint32_t replaced = guest.Read(kOut);
    const uint32_t beforeReplace = fixture.audio->Snapshot().arenaFreeBytes;
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetBufferData), {replaced, kPayload, 4800}, 3) == 0,
      "guest data replacement failed");
    check(fixture.audio->Snapshot().arenaFreeBytes > beforeReplace, "guest data replacement kept the old provider payload");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Lock), {replaced, 0, 4800, kOut + 4, kOut + 8, 0, 0, 0}, 8) == 0 &&
      guest.Read(kOut + 4) == kPayload && guest.Read(kOut + 8) == 4800, "replaced buffer does not lock guest data");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {replaced}, 1) == 0, "replaced buffer release failed");
    check(fixture.audio->Snapshot().arenaFreeBytes == full, "replaced buffer release was not clean");
  });
}

void mixBinBounds() {
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    guest.WriteDesc(kBufferDesc, {0, 0, 9600, kWavePcm, kOut + 16});
    guest.WriteDesc(kOut + 16, {8, 0xFFFFFFC8u});
    bool bounded = false;
    try {
      guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2);
    } catch (const browser::Fault& fault) {
      bounded = std::string(fault.what()).find("mix bin") != std::string::npos;
    }
    check(bounded, "mix bin pair range wrap was not bounded explicitly");
    check(fixture.audio->Snapshot().voices == 0, "wrapping mix bin list created a voice");
    guest.WriteDesc(kOut + 16, {2, kOut + 32});
    guest.WriteDesc(kOut + 32, {3, 0});
    guest.WriteDesc(kOut + 40, {7, 0xFFFFFA00u});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2) == 0, "valid mix bin list rejected");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Release), {guest.Read(kOut)}, 1) == 0, "mix bin buffer release failed");
  });
}

void advancingRefillClock() {
  Fixture fixture;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    guest.WriteDesc(kBufferDesc, {0, 0, 4800, kWavePcm, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2) == 0, "advancing clock buffer creation failed");
    fixture.clockStep = 10000;
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Play), {guest.Read(kOut), 0, 0, 1}, 4) == 0,
      "looping playback did not return under an advancing clock");
    const auto audio = fixture.sink->Snapshot();
    check(audio.queuedFrames == browser::SilentAudioSink::kCapacityFrames && audio.submittedFrames >= audio.queuedFrames,
      "advancing clock refill did not fill its real bounded queue");
    check(fixture.clockSamples < 10, "one refill consumed an unbounded number of clock samples");
  });
}

void audibleBuffer() {
  const auto output = std::make_shared<browser::AudioOutput>();
  Fixture fixture(Fixture::Mode::Normal, output);
  output->header[3] = 1;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    guest.WriteDesc(kBufferDesc, {0, 0, 0, kWavePcm, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2) == 0, "audible buffer creation failed");
    const uint32_t buffer = guest.Read(kOut);
    guest.Write(kPayload, 0xc0004000);
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetBufferData), {buffer, kPayload, 4}, 3) == 0, "audible payload rejected");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetHeadroom), {buffer, 0}, 2) == 0, "audible headroom rejected");
    check(guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Play), {buffer, 0, 0, 1}, 4) == 0, "audible buffer playback failed");
    check(output->header[0] > 0 && output->samples[0] == 0.5f && output->samples[1] == -0.5f,
      "real PCM playback must reach the browser output with independent stereo channels");
    check(output->samples[2] == 0 && output->samples[3] == 0 && output->samples[4] == 0 && output->samples[5] == 0,
      "stereo playback wrote unrelated native speaker channels");
    guest.WriteDesc(kMixBins, {2, kPayload + 16});
    guest.WriteDesc(kPayload + 16, {0, 6000, 1, 6000});
    guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetMixBinVolumes), {buffer, kMixBins}, 2);
    guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetVolume), {buffer, uint32_t(-10000)}, 2);
    const uint32_t before = output->header[0];
    output->header[1] = before;
    fixture.source = 10000000;
    fixture.queue->Drive(context, base);
    check(output->header[0] > before && output->samples[(before % browser::AudioOutput::kFrames) * 6] == 0,
      "a muted voice must stay silent even with positive mix-bin gain");
  });
}

void audibleStreamAfterPriming() {
  const auto output = std::make_shared<browser::AudioOutput>();
  Fixture fixture(Fixture::Mode::Normal, output);
  output->header[3] = 1;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    guest.WriteDesc(kBufferDesc, {0, 0, 0, kWavePcm, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateBuffer), {kBufferDesc, kOut}, 2) == 0, "priming buffer creation failed");
    const uint32_t buffer = guest.Read(kOut);
    guest.Write(kPayload, 0);
    guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_SetBufferData), {buffer, kPayload, 4}, 3);
    guest.Call(BROWSER_AUDIO_GUEST(IDirectSoundBuffer_Play), {buffer, 0, 0, 1}, 4);
    check(fixture.sink->Snapshot().queuedFrames == 8192, "audio lookahead was not primed");
    guest.WriteDesc(kStreamDesc, {0, 4, kWavePcm, 0, 0, 0});
    check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut + 4}, 2) == 0, "audible stream creation failed");
    const uint32_t stream = guest.Read(kOut + 4);
    guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_SetHeadroom), {stream, 0}, 2);
    for (uint32_t n = 0; n < 2; ++n) {
      guest.Write(kPayload + 64, n ? 0x30001000 : 0x20002000);
      guest.WriteDesc(kPacketDesc, {kPayload + 64, 4, kOut + 24, kOut + 28, 0});
      check(guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3) == 0, "short speech packet rejected");
      const uint32_t before = output->header[0];
      output->header[1] = before;
      fixture.source = (n + 1) * 100000;
      fixture.queue->Drive(context, base);
      const uint32_t offset = before % browser::AudioOutput::kFrames * 6;
      check(output->header[0] > before && output->samples[offset] == (n ? 0.125f : 0.25f) &&
        output->samples[offset + 1] == (n ? 0.375f : 0.25f),
        "short speech/music packets disappeared behind previously queued audio");
      guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_GetStatus), {stream, kOut + 20}, 2);
      check(guest.Read(kOut + 24) == 4 && guest.Read(kOut + 28) == 0, "mixed stream packet did not complete exactly");
    }
  });
}

void idleStreamReservations() {
  const auto output = std::make_shared<browser::AudioOutput>();
  Fixture fixture(Fixture::Mode::Normal, output);
  output->header[3] = 1;
  fixture.Run([&](X86Context& context, uint8_t* base) {
    Guest guest{context, base, *fixture.image.memory};
    guest.WriteDesc(kStreamDesc, {0, 4, kWaveAdpcm, 0, 0, 0});
    uint32_t stream = 0;
    for (uint32_t n = 0; n < 96; ++n) {
      check(guest.Call(BROWSER_AUDIO_GUEST(DirectSoundCreateStream), {kStreamDesc, kOut}, 2) == 0,
        "idle reserved stream objects prevented later music/dialogue allocation");
      stream = guest.Read(kOut);
    }
    guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_SetHeadroom), {stream, 0}, 2);
    const std::array<uint8_t, 36> block{0xe8, 3};
    fixture.image.memory->Write(context, kPayload, block.data(), block.size());
    guest.WriteDesc(kPacketDesc, {kPayload, 36, kOut + 24, kOut + 28, 0});
    guest.Call(BROWSER_AUDIO_GUEST(CDirectSoundStream_Process), {stream, kPacketDesc, 0}, 3);
    check(output->header[0] && output->samples[0] == 1000 / 32768.0f,
      "a late allocated ADPCM dialogue stream did not reach the mixer");
    check(fixture.audio->Snapshot().voiceLimitRejections == 0, "idle stream reservation recorded a playback failure");
  });
}

int main() {
  idleStreamReservations();
  audibleStreamAfterPriming();
  audibleBuffer();
  advancingRefillClock();
  installOwnership();
  createIdentity();
  deviceControls();
  voicePositionalControls();
  effectsDownload();
  bufferLifecycle();
  bufferDataOwnership();
  mixBinBounds();
  streamPackets();
  streamRules();
  packetBudget();
  callbacksReentryFailure();
  emptyAndFrames();
  sessionIsolation();
  std::puts("audio-provider-contracts-ok");
  return 0;
}
