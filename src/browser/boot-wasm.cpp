#include "browser/image.h"
#include "browser/kernel-provider.h"
#include "browser/thread-provider.h"
#include "browser/synchronization-provider.h"
#include "browser/platform-profile.h"
#include "browser/shutdown-provider.h"
#include "browser/dpc-provider.h"
#include "browser/timer-provider.h"
#include "browser/virtual-memory.h"
#include "browser/contiguous-memory.h"
#include "browser/string-provider.h"
#include "browser/section-provider.h"
#include "browser/file-provider.h"
#include "browser/raw-cache.h"
#include <algorithm>
#include "browser/sdk-provider.h"
#include "browser/diagnostic-graphics.h"
#include "browser/silent-audio.h"
#include "browser/audio-provider.h"
#include "build_identity.h"
#include <emscripten.h>
#include <emscripten/heap.h>
#include <array>
#include <bit>
#include <mutex>
#include <optional>
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
#include "browser/menu-input.h"
#ifdef BROWSER_MISSION
#include "browser/mission-input.h"
#include "browser/mission-clock.h"
#include <thread>
#endif
#include "browser/av-provider.h"
#include "browser/delay-provider.h"
#include "browser/menu-launch.h"
#include "browser/menu-route.h"
#endif
#ifdef BROWSER_MENU
#include "browser/menu-backend.h"
#include "browser/hooks.h"
#include "browser/host-settings.h"
#include "cpu/x86_ops.h"
#include "hook_manifest.h"

#define BROWSER_HOOK_DECLARE(hook, address) X86_EXTERN_FUNC(sub_##address);
BROWSER_GAME_HOOKS(BROWSER_HOOK_DECLARE)
#endif

namespace {
#ifdef BROWSER_MENU
struct GameHook {
  const char* name;
  uint32_t address;
  X86Func* function;
};
#define BROWSER_HOOK_ENTRY(hook, address) {#hook, 0x##address, sub_##address},
constexpr GameHook gameHooks[] = {BROWSER_GAME_HOOKS(BROWSER_HOOK_ENTRY) {nullptr, 0, nullptr}};
constexpr uint32_t gameHookCount = std::size(gameHooks) - 1;
constexpr auto graphics_mode = browser::GraphicsMode::Rendering;
constexpr size_t worker_capacity = browser::kImageThreadCapacity;
#elif defined(BROWSER_MENU_CAPTURE)
constexpr auto graphics_mode = browser::GraphicsMode::StateCapture;
constexpr size_t worker_capacity = 2;
#else
constexpr auto graphics_mode = browser::GraphicsMode::Checkpoint;
constexpr size_t worker_capacity = 2;
#endif
struct StoryState {
#ifdef BROWSER_MISSION
  std::shared_ptr<browser::AudioOutput> audio = std::make_shared<browser::AudioOutput>();
  std::array<std::atomic<uint32_t>, 42> metrics{};
  std::shared_ptr<browser::MissionInput::SharedStorage> motion = std::make_shared<browser::MissionInput::SharedStorage>();
#endif
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  std::shared_ptr<browser::MenuInput::SharedStorage> input = std::make_shared<browser::MenuInput::SharedStorage>();
#endif
  std::shared_ptr<browser::TemporaryFilesystem> profile = std::make_shared<browser::TemporaryFilesystem>(32u << 20);
  std::shared_ptr<browser::RawCache> cache = std::make_shared<browser::RawCache>(browser::CacheFatx::StoryCapacity);
  std::optional<std::array<uint8_t, 4096>> launchPage;
  std::optional<browser::DiagnosticGraphics::PersistedDisplay> display;
  std::array<uint8_t, 16> driveKey{};
  std::array<uint8_t, 21> serial{};
  bool initialized = false;
  uint32_t restarts = 0;
  bool developmentStart = false;
};
struct Session {
#ifdef BROWSER_MISSION
  std::mutex simulationMutex;
  std::array<uint32_t, 4> simulation{};
  uint32_t simulationLevel = 0;
#endif
  std::shared_ptr<StoryState> story;
  browser::ContiguousMemoryAccess restoredAllocations;
  uint32_t savedDisplay = 0;
  browser::Runtime runtime;
  std::unique_ptr<browser::PreparedImage> image;
  std::vector<browser::KernelDataExport> dataExports;
  std::vector<browser::BoundImport> coverage;
  browser::ImageExecution execution;
  browser::ThreadSessionSnapshot threadSession;
  browser::Runtime::KernelTraceSnapshot kernelTrace;
  std::shared_ptr<const browser::SdkTrace> sdkObserver;
  browser::SdkTrace::SnapshotData sdkTrace;
  std::shared_ptr<browser::DiagnosticGraphics> graphics;
  browser::DiagnosticGraphics::SnapshotData graphicsTrace;
  std::shared_ptr<browser::SilentAudioSink> silentAudio;
  std::shared_ptr<browser::AudioProvider> audioProvider;
  browser::SilentAudioSnapshot audioTrace;
  browser::AudioProvider::SnapshotData audioProviderTrace;
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  std::shared_ptr<browser::MenuInput> input;
  std::shared_ptr<browser::MenuLaunch> launcher;
  std::shared_ptr<browser::MenuRoute> route;
  std::shared_ptr<browser::MenuMissionObservation> missionObserver;
  browser::MenuMissionObservation::SnapshotData missionTrace;
  std::optional<browser::MenuLaunchRequested> launchRequest;
#endif
#ifdef BROWSER_MENU
  browser::MenuBackend* renderer = nullptr;
#endif
  std::function<browser::DiscReadSnapshot()> snapshotDiscReads;
  browser::DiscReadSnapshot discReads;
  std::mutex fileOpensMutex;
  std::vector<browser::FileOpenDiagnostic> fileOpens;
  bool graphicsCheckpoint = false;
  std::string faultMessage;
  uint32_t faultAddress = 0;
#ifdef BROWSER_MISSION
  std::shared_ptr<browser::MissionInput> missionControls;
#endif
  Session(std::vector<uint8_t> bytes, std::span<const X86FuncMapping> mappings, std::span<const uint32_t> continuations, std::shared_ptr<StoryState> state = {})
      : story(std::move(state)), runtime(mappings, continuations,
#ifdef BROWSER_MISSION
      [this, origin = browser::MissionClock::HostOrigin(), clock = browser::MissionClock{}, sample = std::optional<uint64_t>{}]() mutable {
        for (;;) {
          while ((*story->motion)[2].load(std::memory_order_acquire)) {
            runtime.Checkpoint({});
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
          }
          thread_local double queried = -1;
          const double local = emscripten_get_now();
          if (sample && local >= queried && local - queried < 1) { return *sample; }
          const uint32_t inactive = (*story->motion)[4].load(std::memory_order_acquire);
          const double milliseconds = browser::MissionClock::HostMilliseconds(origin);
          if (!(milliseconds >= 0 && milliseconds < double(UINT32_MAX))) { throw std::runtime_error("mission host timer is out of range"); }
          const bool paused = (*story->motion)[2].load(std::memory_order_acquire);
          const uint32_t inactiveAfter = (*story->motion)[4].load(std::memory_order_acquire);
          const auto observed = clock.Observe(uint64_t(milliseconds), inactive, paused, inactiveAfter);
          if (!observed) { continue; }
          queried = local;
          sample = *observed;
          return *sample;
        }
      },
#else
      {},
#endif
      worker_capacity), image(browser::PrepareImage(std::move(bytes), {X86ImageBase, X86ImageEnd, X86EntryPoint})) {
    if (!runtime.Find(X86EntryPoint)) { throw std::runtime_error("generated entry body is absent"); }
#ifdef BROWSER_MISSION
    runtime.ReuseTimerSamples(1);
#endif
    browser::InstallSectionProvider(runtime, *image);
    browser::InstallKernelProvider(runtime, *image->memory);
    browser::QueryContiguousMemoryUsage contiguousUsage;
    browser::ContiguousMemoryAccess contiguousAccess;
    browser::InstallContiguousMemory(runtime, *image->memory, &contiguousUsage, &contiguousAccess);
#ifdef BROWSER_MENU
    restoredAllocations = contiguousAccess;
    browser::MenuBackendLimits rendererLimits;
#ifdef BROWSER_MISSION
    rendererLimits.maxResources = 512;
    rendererLimits.maxResidentBytes = 128u << 20;
    rendererLimits.batchDraws = true;
    rendererLimits.compactVertices = true;
    rendererLimits.indexedVertices = true;
    rendererLimits.residentVertices = true;
    rendererLimits.postCommands = true;
    rendererLimits.batchCommands = true;
    rendererLimits.maxVertices = 65536;
    rendererLimits.maxExpandedVertices = 196608;
#endif
    auto backend = std::make_unique<browser::MenuBackend>(runtime, *image, std::make_shared<browser::webgpu::Transport>(), rendererLimits);
    renderer = backend.get();
    graphics = std::make_shared<browser::DiagnosticGraphics>(runtime, *image, std::move(contiguousAccess), graphics_mode, std::move(backend));
    graphics->ConfigurePresentCheckpoint(BROWSER_MENU_TRACE_FRAMES);
#else
    graphics = std::make_shared<browser::DiagnosticGraphics>(runtime, *image, std::move(contiguousAccess), graphics_mode);
#endif
    browser::InstallVirtualMemory(runtime, *image, std::move(contiguousUsage));
    browser::InstallStringProvider(runtime, *image);
    std::function<void(browser::FileOpenDiagnostic)> reportFileOpen;
    std::function<void(browser::FileOperationDiagnostic)> reportFileOperation;
#ifdef BROWSER_MISSION
    reportFileOperation = [](browser::FileOperationDiagnostic operation) {
      MAIN_THREAD_EM_ASM({ Module.onFileOperation?.({ ordinal: $0 >>> 0, informationClass: $1 >>> 0, status: $2 >>> 0, length: $3 >>> 0, path: UTF8ToString($4) }); },
        operation.ordinal, operation.informationClass, operation.status, operation.length, operation.path.c_str());
    };
#endif
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
    reportFileOpen = [this](browser::FileOpenDiagnostic diagnostic) {
      std::lock_guard lock(fileOpensMutex);
      if (fileOpens.size() == 32) { fileOpens.erase(fileOpens.begin()); }
      fileOpens.push_back(std::move(diagnostic));
    };
#endif
    auto synchronizationRoutes = std::make_shared<browser::SynchronizationHandleRoutes>();
    const auto mountedDisc = browser::MountedIsoDisc();
    std::shared_ptr<browser::TemporaryFilesystem> profileStorage;
#ifdef BROWSER_MENU
    profileStorage = story ? story->profile : std::make_shared<browser::TemporaryFilesystem>(32u << 20);
#endif
    auto fileCloser = browser::InstallFileProvider(runtime, *image, mountedDisc, std::move(profileStorage), &dataExports, &snapshotDiscReads,
      std::move(reportFileOpen), &synchronizationRoutes->ownsFile, story ? story->cache : nullptr, std::move(reportFileOperation),
#ifdef BROWSER_MISSION
      32u << 20, 256
#else
      1u << 20
#endif
    );
#ifdef BROWSER_MISSION
    renderer->SetMetricsSink([this](const auto& frame) {
      const auto reads = snapshotDiscReads();
      const auto bounded = [](uint64_t value) { return uint32_t(std::min<uint64_t>(value, UINT32_MAX)); };
      std::array<uint32_t, 4> sampledSimulation;
      { std::lock_guard lock(simulationMutex); sampledSimulation = simulation; }
      auto& metrics = story->metrics;
      metrics[0].fetch_add(1, std::memory_order_acq_rel);
      const auto threads = browser::SnapshotThreadSession(*image);
      const auto audio = audioProvider->Snapshot();
      const auto stepping = missionControls ? missionControls->SnapshotSteps() : browser::MissionInput::StepSnapshot{};
      const auto& inventory = frame.inventory;
      const std::array<uint32_t, 41> values{bounded(emscripten_get_heap_size()), bounded(image->memory->BackingBytes()),
        bounded(runtime.ActiveWorkers()), frame.residentBytes, frame.resources, frame.pipelines, bounded(frame.frames),
        bounded(frame.commandWaitMicroseconds), bounded(frame.readbackWaitMicroseconds), bounded(frame.uploadWaitMicroseconds),
        bounded(frame.drawWaitMicroseconds), bounded(reads.assetBytes), bounded(reads.assetOperations), bounded(reads.waitMicroseconds),
        (*story->motion)[4].load(), story->restarts, threads.childrenCreated - std::min(threads.childrenCreated, threads.handlesClosed),
        threads.objectReferences, inventory.macMask, inventory.iluMask, inventory.textureModes, inventory.paths,
        inventory.formats[0], inventory.formats[1], inventory.formats[2], inventory.formats[3],
        inventory.formats[4], inventory.formats[5], inventory.formats[6], inventory.formats[7],
        sampledSimulation[0], sampledSimulation[1], sampledSimulation[2], sampledSimulation[3], audio.voices, audio.voiceLimitRejections,
        bounded(stepping.steps), bounded(stepping.skippedDraws), bounded(stepping.updateMicroseconds), bounded(stepping.renderMicroseconds), bounded(stepping.backlogTicks / 10)};
      for (uint32_t n = 0; n < values.size(); ++n) { metrics[n + 1].store(values[n], std::memory_order_relaxed); }
      metrics[0].fetch_add(1, std::memory_order_release);
    });
#endif
    auto synchronizationCloser = browser::InstallSynchronizationProvider(runtime, *image, synchronizationRoutes);
    dataExports.push_back(browser::InstallThreadProvider(runtime, *image, [fileCloser, synchronizationCloser](uint32_t handle) {
      return fileCloser(handle) || synchronizationCloser(handle);
    }, &synchronizationRoutes->ownsThread));
    browser::InstallShutdownProvider(runtime, *image);
    browser::InstallDpcProvider(runtime, *image);
    const auto timers = browser::InstallTimerProvider(runtime, *image);
    dataExports.push_back(browser::InstallTimerTickCount(runtime, *image, timers));
    silentAudio = std::make_shared<browser::SilentAudioSink>(runtime, *image
#ifdef BROWSER_MISSION
      , story ? story->audio : nullptr
#endif
    );
    dataExports.push_back(browser::InstallVirtualDriveKey(runtime, *image));
    const auto platform = browser::InstallPlatformProfile(runtime, *image);
    dataExports.insert(dataExports.end(), platform.begin(), platform.end());
    dataExports.push_back(browser::InstallSignatureKey(runtime, *image));
    if (story) {
      if (story->initialized) {
        image->memory->Write({}, 0xc0003000, story->driveKey.data(), story->driveKey.size());
        image->memory->Write({}, 0xc000003c, story->serial.data(), story->serial.size());
      } else {
        image->memory->Read({}, 0xc0003000, story->driveKey.data(), story->driveKey.size());
        image->memory->Read({}, 0xc000003c, story->serial.data(), story->serial.size());
        story->initialized = true;
      }
    }
    audioProvider = browser::InstallAudioProvider(runtime, *image, silentAudio, mountedDisc);
    browser::InstallTimerClockObserver(runtime, *image, timers,
      [sink = silentAudio, provider = audioProvider](const X86Context& context, uint64_t now) {
        sink->Advance(context, now);
        provider->Tick(context, now);
      });
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
    browser::InstallAvProvider(runtime, *image->memory, [this] { return savedDisplay; });
    browser::InstallDelayProvider(runtime);
    input = std::make_shared<browser::MenuInput>(runtime, *image, story ? story->input : nullptr);
    launcher = std::make_shared<browser::MenuLaunch>(runtime, *image, bool(story));
    if (story) {
      const auto cell = std::find_if(dataExports.begin(), dataExports.end(), [](const auto& item) { return item.ordinal == 164; });
      if (cell == dataExports.end()) { throw std::runtime_error("story restart requires LaunchDataPage"); }
      launcher->InstallFirmware(cell->address);
    }
    route = std::make_shared<browser::MenuRoute>(runtime, *image);
    missionObserver = std::make_shared<browser::MenuMissionObservation>(runtime, *image);
    constexpr auto observedReturns = browser::MenuMissionObservation::ReturnAddresses();
    runtime.InstallInvocationObserver(*image->memory, [observer = missionObserver](const X86Context& ctx, uint8_t* base, uint32_t target, uint64_t workerId) {
      observer->Observe(ctx, base, target, workerId);
    }, {observedReturns.begin(), observedReturns.end()});
    std::shared_ptr<browser::SdkGraphics> controls = input;
#ifdef BROWSER_MISSION
    missionControls = std::make_shared<browser::MissionInput>(runtime, *image, input, story->motion, [this](const auto& sample) {
      std::lock_guard lock(simulationMutex);
      if (simulationLevel != sample.level || (simulation[0] && sample.seconds < std::bit_cast<float>(simulation[1]))) {
        simulationLevel = sample.level;
        if (simulation[3] != UINT32_MAX) { ++simulation[3]; }
      }
      if (simulation[0] != UINT32_MAX) { ++simulation[0]; }
      simulation[1] = std::bit_cast<uint32_t>(sample.seconds);
      simulation[2] = std::bit_cast<uint32_t>(sample.deltaSeconds);
    });
    controls = missionControls;
    if (story->developmentStart) { missionControls->ConfigureDevelopmentStart(); }
#endif
    sdkObserver = browser::InstallSdkProvider(runtime, *image, browser::SdkPolicy::UnavailableXNet, graphics, controls, launcher, route, audioProvider);
#ifdef BROWSER_MENU
    graphics->Hooks().SetBackend(renderer, [renderer = renderer](browser::GraphicsEffect effect) { return renderer->Supports(effect); });
    browser::hooks::Bind(*image->memory);
    browser::hooks::BindGraphics(graphics->Hooks());
#endif
#else
    sdkObserver = browser::InstallSdkProvider(runtime, *image, browser::SdkPolicy::UnavailableXNet, graphics, {}, {}, {}, audioProvider);
#endif
  }
  void Restore(X86Context& context) {
    if (!story || !story->launchPage) { return; }
    const auto allocate = [&](uint32_t bytes) {
      const uint32_t address = restoredAllocations.allocate(context, bytes, 4096);
      if (!address) { throw browser::Fault(context, 0, bytes, "story restart exhausted contiguous storage"); }
      return address;
    };
    const auto page = allocate(4096);
    image->memory->Write(context, page, story->launchPage->data(), 4096);
    image->memory->Write(context, 0xc0000010, &page, 4);
    if (story->display) {
      const auto& display = *story->display;
      savedDisplay = allocate(uint32_t(display.pixels.size()) + 64);
      const std::array<uint32_t, 5> header = {(display.common & 0xffff0000) | 1u, (savedDisplay + 64) & 0x1fffffffu, 0, display.format, display.size};
      image->memory->Write(context, savedDisplay, header.data(), sizeof(header));
      image->memory->Write(context, savedDisplay + 64, display.pixels.data(), display.pixels.size());
    }
  }
  ~Session() {
    runtime.Stop();
#ifdef BROWSER_MENU
    browser::hooks::UnbindGraphics();
    browser::hooks::Unbind();
#endif
    if (graphics) { graphics->Shutdown(); }
  }
};
std::vector<uint8_t> input;
std::unique_ptr<Session> session;
std::shared_ptr<browser::AudioOutput> publishedAudioStorage;
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
std::shared_ptr<browser::MenuInput> publishedInputStorage;
#endif
uint32_t faultOrdinal = 0;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE const char* boot_build_identity() { return BROWSER_BUILD_ID; }
EMSCRIPTEN_KEEPALIVE uint32_t boot_build_identity_size() { return sizeof(BROWSER_BUILD_ID) - 1; }

EMSCRIPTEN_KEEPALIVE void image_reset() {
  session.reset();
  std::vector<uint8_t>{}.swap(input);
  faultOrdinal = 0;
}

EMSCRIPTEN_KEEPALIVE uint8_t* image_allocate(uint32_t size) {
  image_reset();
  if (!size || size > (64u << 20)) { return nullptr; }
  input.resize(size);
  return input.data();
}

EMSCRIPTEN_KEEPALIVE int image_prepare(uint32_t base, uint32_t end, uint32_t entry) {
  session.reset();
  faultOrdinal = 0;
  if (base != X86ImageBase || end != X86ImageEnd || entry != X86EntryPoint) { return 0; }
  try {
    size_t mappingCount = 0;
    while (X86FuncMappings[mappingCount].host) { ++mappingCount; }
    size_t continuationCount = 0;
    while (X86Continuations[continuationCount]) { ++continuationCount; }
    session = std::make_unique<Session>(std::move(input),
      std::span(X86FuncMappings, mappingCount), std::span(X86Continuations, continuationCount)
#ifdef BROWSER_MISSION
      , std::make_shared<StoryState>()
#endif
      );
    return 1;
  } catch (const std::runtime_error&) {
    return 0;
  }
}

EMSCRIPTEN_KEEPALIVE int image_development_boot() {
#ifdef BROWSER_MISSION
  if (!session || !session->story || session->story->launchPage || session->image->importsBound || session->runtime.ActiveWorkers()) { return 0; }
  try {
    session->missionControls->ConfigureDevelopmentStart();
    session->launcher->ConfigureDevelopmentStart();
    session->story->launchPage = browser::DevelopmentIcelandLaunchPage();
    session->story->developmentStart = true;
    return 1;
  } catch (const std::runtime_error&) { return 0; }
#else
  return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE int image_declare_import(uint32_t ordinal, uint32_t kind) {
  if (!session || kind > 1 || session->image->importsBound || session->runtime.ActiveWorkers()) { return 0; }
  try {
    session->runtime.Checkpoint({});
    for (const auto& data : session->dataExports) {
      if (data.ordinal == ordinal) { return kind == 1 ? 1 : 0; }
    }
    browser::DeclareDiagnosticImport(*session->image, session->runtime, ordinal, kind == 1);
    return 1;
  } catch (const std::runtime_error&) { return 0; }
}

EMSCRIPTEN_KEEPALIVE int image_execute() {
  if (!session || !session->image->importsBound) { return 0; }
  faultOrdinal = 0;
  session->graphicsCheckpoint = false;
  try {
    browser::ExecuteImageEntry(*session->image, session->runtime, session->execution, [](X86Context& context) { session->Restore(context); });
    session->threadSession = browser::SnapshotThreadSession(*session->image);
    session->kernelTrace = session->runtime.SnapshotKernelTrace();
    session->sdkTrace = session->sdkObserver->Snapshot();
    session->discReads = session->snapshotDiscReads();
    session->graphicsTrace = session->graphics->Snapshot();
    session->audioTrace = session->silentAudio->Snapshot();
    session->audioProviderTrace = session->audioProvider->Snapshot();
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
    session->missionTrace = session->missionObserver->Snapshot();
#endif
    return 1;
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  } catch (const browser::MenuLaunchRequested& request) {
    session->launchRequest = request;
    session->faultMessage = "Guest requested a new story runtime. Mission execution is not enabled in this menu build.";
#endif
  } catch (const browser::GraphicsCheckpoint& checkpoint) {
    session->graphicsCheckpoint = true;
    session->faultMessage = checkpoint.what();
  } catch (const browser::UnsupportedImport& fault) {
    faultOrdinal = fault.ordinal;
    session->faultAddress = fault.address;
    session->faultMessage = fault.what();
  } catch (const browser::Fault& fault) {
    faultOrdinal = browser::DiagnosticDataOrdinal(*session->image, fault.address);
    session->faultAddress = fault.address;
    session->faultMessage = fault.what();
  } catch (const std::exception& fault) {
    session->faultMessage = fault.what();
  }
  if (session->execution.entered) { session->threadSession = browser::SnapshotThreadSession(*session->image); }
  session->kernelTrace = session->runtime.SnapshotKernelTrace();
  session->sdkTrace = session->sdkObserver->Snapshot();
  session->discReads = session->snapshotDiscReads();
#ifdef BROWSER_MISSION
  if (!session->launchRequest && !session->graphicsCheckpoint && !session->discReads.lastPath.empty()) {
    session->faultMessage += " Last ISO read: " + session->discReads.lastPath + " offset=" + std::to_string(session->discReads.lastOffset) + " bytes=" + std::to_string(session->discReads.lastBytes) + ".";
  }
#endif
  session->graphicsTrace = session->graphics->Snapshot();
  session->audioTrace = session->silentAudio->Snapshot();
  session->audioProviderTrace = session->audioProvider->Snapshot();
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  session->missionTrace = session->missionObserver->Snapshot();
#endif
  return 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t image_restart_count() {
  return session && session->story ? session->story->restarts : 0;
}

EMSCRIPTEN_KEEPALIVE int image_can_restart() {
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  return session && session->story && session->launchRequest && session->launchRequest->page ? 1 : 0;
#else
  return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE int image_restart() {
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  if (!image_can_restart() || session->runtime.ActiveWorkers()) { return 0; }
  auto state = session->story;
  if (state->restarts == UINT32_MAX) { return 0; }
  state->launchPage = session->launchRequest->page;
  state->display = session->graphics->PersistedDisplaySnapshot();
  auto bytes = session->image->image.file;
  const auto declarations = session->image->diagnosticImports;
  size_t mappingCount = 0, continuationCount = 0;
  while (X86FuncMappings[mappingCount].host) { ++mappingCount; }
  while (X86Continuations[continuationCount]) { ++continuationCount; }
  try {
#ifdef BROWSER_MENU
    session->renderer->Retire();
#endif
    session.reset();
    session = std::make_unique<Session>(std::move(bytes), std::span(X86FuncMappings, mappingCount),
      std::span(X86Continuations, continuationCount), state);
    for (const auto& item : declarations) {
      browser::DeclareDiagnosticImport(*session->image, session->runtime, item.ordinal, item.kind == browser::ImportKind::DataGuard);
    }
    session->coverage = browser::InspectImageImports(*session->image, session->runtime, session->dataExports);
    browser::BindImageImports(*session->image, session->runtime, session->dataExports);
    ++state->restarts;
    return 1;
  } catch (...) {
    session.reset();
    return 0;
  }
#else
  return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE const std::atomic<uint32_t>* image_progress_address() {
  static_assert(sizeof(std::atomic<uint32_t>) == 4 && std::atomic<uint32_t>::is_always_lock_free);
  return session ? session->runtime.ProgressAddress() : nullptr;
}

EMSCRIPTEN_KEEPALIVE uint32_t image_trace_count() { return session ? session->kernelTrace.count : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_trace_word(uint32_t index, uint32_t field) {
  if (!session || index >= session->kernelTrace.count) { return 0; }
  const auto& event = session->kernelTrace.events[index];
  const std::array<uint32_t, 13> words = {
    uint32_t(event.sequence), uint32_t(event.sequence >> 32),
    uint32_t(event.callId), uint32_t(event.callId >> 32),
    uint32_t(event.workerId), uint32_t(event.workerId >> 32),
    event.ordinal, event.fsBase, event.eax, event.esp, event.lastIcall, uint32_t(event.phase)
  };
  return field < words.size() ? words[field] : 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t image_sdk_trace_count() { return session ? session->sdkTrace.count : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_sdk_trace_word(uint32_t index, uint32_t field) {
  if (!session || index >= session->sdkTrace.count) { return 0; }
  const auto& event = session->sdkTrace.events[index];
  std::array<uint32_t, 31> words = {
    uint32_t(event.sequence), uint32_t(event.sequence >> 32),
    uint32_t(event.workerId), uint32_t(event.workerId >> 32),
    event.id, event.fsBase, event.threadHandle, event.esp, event.returnAddress, event.argumentCount,
    uint32_t(event.behavior), event.id == 3 ? 13u : 0u
  };
  std::copy(event.arguments.begin(), event.arguments.end(), words.begin() + 12);
  std::copy(event.presentation.begin(), event.presentation.end(), words.begin() + 18);
  return field < words.size() ? words[field] : 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t image_disc_reads_words() { return session ? 8 : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_disc_reads_word(uint32_t index) {
  if (!session || index >= 8) { return 0; }
  const auto& reads = session->discReads;
  const std::array<uint64_t, 4> values = {reads.assetBytes, reads.assetOperations, reads.executableBytes, reads.executableOperations};
  return uint32_t(values[index / 2] >> ((index % 2) * 32));
}

EMSCRIPTEN_KEEPALIVE uintptr_t image_audio_address() {
#ifdef BROWSER_MISSION
  if (!session || !session->story) { return 0; }
  publishedAudioStorage = session->story->audio;
  return reinterpret_cast<uintptr_t>(publishedAudioStorage->header.data());
#else
  return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE uintptr_t image_audio_samples_address() {
  return publishedAudioStorage ? reinterpret_cast<uintptr_t>(publishedAudioStorage->samples.data()) : 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t image_audio_info(uint32_t field) {
  if (!session) { return 0; }
  const auto& snapshot = session->audioTrace;
  switch (field) {
    case 0: return audio::kRate;
    case 1: return audio::kChannels;
    case 2: return browser::SilentAudioSink::kCapacityFrames;
    case 3: return snapshot.paused;
    case 4: return uint32_t(snapshot.submittedFrames);
    case 5: return uint32_t(snapshot.submittedFrames >> 32);
    case 6: return uint32_t(snapshot.consumedFrames);
    case 7: return uint32_t(snapshot.consumedFrames >> 32);
    case 8: return uint32_t(snapshot.queuedFrames);
    case 9: return uint32_t(snapshot.queuedFrames >> 32);
    case 10: return session->audioProviderTrace.voices;
    case 11: return session->audioProviderTrace.arenaFreeBytes;
    case 12: return uint32_t(session->audioProviderTrace.publishedCompletions);
    case 13: return uint32_t(session->audioProviderTrace.publishedCompletions >> 32);
    case 14: return uint32_t(session->audioProviderTrace.deliveredCallbacks);
    case 15: return uint32_t(session->audioProviderTrace.deliveredCallbacks >> 32);
    case 16: return uint32_t(session->audioProviderTrace.pendingCallbacks);
    case 17: return session->audioProviderTrace.device;
    default: return 0;
  }
}
EMSCRIPTEN_KEEPALIVE uintptr_t image_input_address() {
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  if (!session || !session->input || (publishedInputStorage && publishedInputStorage != session->input &&
      publishedInputStorage->SharedAddress() != session->input->SharedAddress())) { return 0; }
  publishedInputStorage = session->input;
  return reinterpret_cast<uintptr_t>(publishedInputStorage->SharedAddress());
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uintptr_t image_gamepad_address() {
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  return session && session->input && publishedInputStorage == session->input ?
    reinterpret_cast<uintptr_t>(publishedInputStorage->GamepadAddress()) : 0;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_gamepad_words() {
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  return session && session->input && publishedInputStorage == session->input ? publishedInputStorage->GamepadWords() : 0;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_metrics_words() {
#ifdef BROWSER_MISSION
  return session && session->story ? uint32_t(session->story->metrics.size()) : 0;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uintptr_t image_metrics_address() {
#ifdef BROWSER_MISSION
  return session && session->story ? reinterpret_cast<uintptr_t>(session->story->metrics.data()) : 0;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uintptr_t image_motion_address() {
#ifdef BROWSER_MISSION
  return session && session->story ? reinterpret_cast<uintptr_t>(session->story->motion->data()) : 0;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_input_words() {
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  return session && session->input ? session->input->SharedWords() : 0;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uintptr_t image_settings_address() {
#ifdef BROWSER_MENU
  return reinterpret_cast<uintptr_t>(browser::host_settings::Shared().words.data());
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_settings_words() {
#ifdef BROWSER_MENU
  return browser::host_settings::kWords;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uintptr_t image_hook_status_address() {
#ifdef BROWSER_MENU
  return reinterpret_cast<uintptr_t>(browser::HookStatus().data());
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_hook_status_words() {
#ifdef BROWSER_MENU
  return browser::kHookStatusWords;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_hook_count() {
#ifdef BROWSER_MENU
  return gameHookCount;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_hook_address(uint32_t index) {
#ifdef BROWSER_MENU
  return index < gameHookCount ? gameHooks[index].address : 0;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE const char* image_hook_name(uint32_t index) {
#ifdef BROWSER_MENU
  return index < gameHookCount ? gameHooks[index].name : nullptr;
#else
  return nullptr;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_hook_bound(uint32_t index) {
#ifdef BROWSER_MENU
  return session && index < gameHookCount && session->runtime.Find(gameHooks[index].address) == gameHooks[index].function;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_launch_word(uint32_t field, uint32_t index) {
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  if (!session || !session->launchRequest) { return 0; }
  const auto& request = *session->launchRequest;
  if (field == 0) { return request.path.size(); }
  if (field == 1) { return index < request.path.size() ? static_cast<uint8_t>(request.path[index]) : 0; }
  if (field == 2) { return request.hasData; }
  if (field == 3) { return request.hasData && index < request.data.size() ? request.data[index] : 0; }
#endif
  return 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t image_mission_count() {
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  return session ? uint32_t(session->missionTrace.count) : 0;
#else
  return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE uint32_t image_mission_total_word(uint32_t field) {
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  return session && field < 2 ? uint32_t(session->missionTrace.total >> (field * 32)) : 0;
#else
  return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE uint32_t image_mission_word(uint32_t index, uint32_t field) {
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  if (!session || index >= session->missionTrace.count) { return 0; }
  const auto& snapshot = session->missionTrace;
  const auto& event = snapshot.events[(snapshot.total - snapshot.count + index) % snapshot.events.size()];
  const uint32_t words[] = {uint32_t(event.sequence), uint32_t(event.sequence >> 32), uint32_t(event.workerId), uint32_t(event.workerId >> 32),
    uint32_t(event.state), event.request.valid ? 1u : 0u, event.request.menuPage, event.request.mode, event.request.difficulty,
    uint32_t(event.command.size()), uint32_t(event.request.map.size()), uint32_t(event.request.profile.size())};
  return field < std::size(words) ? words[field] : 0;
#else
  return 0;
#endif
}

EMSCRIPTEN_KEEPALIVE uint32_t image_graphics_checkpoint() { return session && session->graphicsCheckpoint ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_graphics_rendering() {
#ifdef BROWSER_MENU
  return 1;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_file_open_count() {
  if (!session || session->runtime.ActiveWorkers()) { return 0; }
  std::lock_guard lock(session->fileOpensMutex);
  return session->fileOpens.size();
}
EMSCRIPTEN_KEEPALIVE uint32_t image_file_open_word(uint32_t index, uint32_t field) {
  if (!session || session->runtime.ActiveWorkers()) { return 0; }
  std::lock_guard lock(session->fileOpensMutex);
  if (index >= session->fileOpens.size()) { return 0; }
  const auto& entry = session->fileOpens[index];
  const std::array<uint32_t, 8> words{entry.status, entry.root, entry.disposition, entry.options,
    entry.access, entry.attributes, uint32_t(entry.allocationBytes), uint32_t(entry.allocationBytes >> 32)};
  return field < words.size() ? words[field] : 0;
}
EMSCRIPTEN_KEEPALIVE const char* image_file_open_path(uint32_t index, uint32_t field) {
  if (!session || session->runtime.ActiveWorkers()) { return nullptr; }
  std::lock_guard lock(session->fileOpensMutex);
  if (index >= session->fileOpens.size() || field > 1) { return nullptr; }
  const auto& entry = session->fileOpens[index];
  return field ? entry.resolvedPath.c_str() : entry.originalPath.c_str();
}
EMSCRIPTEN_KEEPALIVE uint32_t image_renderer_info(uint32_t field) {
#ifdef BROWSER_MENU
  if (!session || !session->renderer) { return 0; }
  const auto snapshot = session->renderer->Snapshot();
  const std::array<uint32_t, 10> words = {snapshot.pipelineCount, snapshot.resourceCount,
    uint32_t(snapshot.residentBytes), uint32_t(snapshot.residentBytes >> 32),
    uint32_t(snapshot.submittedCommands), uint32_t(snapshot.submittedCommands >> 32),
    uint32_t(snapshot.completedCommands), uint32_t(snapshot.completedCommands >> 32),
    uint32_t(snapshot.presentedFrames), uint32_t(snapshot.presentedFrames >> 32)};
  return field < words.size() ? words[field] : 0;
#else
  return 0;
#endif
}
EMSCRIPTEN_KEEPALIVE uint32_t image_persisted_display_word(uint32_t field) {
  return session && field < session->graphicsTrace.persistedDisplay.size() ? session->graphicsTrace.persistedDisplay[field] : 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t image_graphics_info(uint32_t field) {
  if (!session) { return 0; }
  const auto& trace = session->graphicsTrace;
  const std::array<uint32_t, 6> words = {uint32_t(trace.initialized), uint32_t(trace.total), uint32_t(trace.total >> 32),
    uint32_t(trace.count), uint32_t(trace.resources.size()), trace.commandWords};
  return field < words.size() ? words[field] : 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t image_graphics_capture_word(uint32_t group, uint32_t index) {
  if (!session || session->runtime.ActiveWorkers()) { return 0; }
  if (group == 0) {
    const auto snapshot = session->graphics->Snapshot();
    return index == UINT32_MAX ? snapshot.commands.size() : index < snapshot.commands.size() ? snapshot.commands[index] : 0;
  }
  const auto state = session->graphics->SnapshotState();
  if (group == 1) { return index == UINT32_MAX ? state.rs.size() : index < state.rs.size() ? state.rs[index] : 0; }
  if (group == 2) { return index == UINT32_MAX ? state.simple.size() : index < state.simple.size() ? state.simple[index] : 0; }
  if (group == 3) { return index == UINT32_MAX ? state.tss.size() * gpu::kTssCount : index < state.tss.size() * gpu::kTssCount ? state.tss[index / gpu::kTssCount][index % gpu::kTssCount] : 0; }
  if (group == 4) { return index == UINT32_MAX ? 2 : index == 0 ? state.vs_handle : index == 1 && state.ps ? state.ps->handle : 0; }
  if (group == 5) { return index == UINT32_MAX ? state.vs ? state.vs->decl.size() : 0 : state.vs && index < state.vs->decl.size() ? state.vs->decl[index] : 0; }
  if (group == 6) { return index == UINT32_MAX ? state.vs ? state.vs->program.size() : 0 : state.vs && index < state.vs->program.size() ? state.vs->program[index] : 0; }
  if (group == 7) { return index == UINT32_MAX ? state.ps ? std::size(state.ps->def) : 0 : state.ps && index < std::size(state.ps->def) ? state.ps->def[index] : 0; }
  if (group == 8) { return index == UINT32_MAX ? state.vsc.size() * 4 : index < state.vsc.size() * 4 ? std::bit_cast<uint32_t>(state.vsc[index / 4][index % 4]) : 0; }
  if (group == 9) { return index == UINT32_MAX ? state.psc.size() * 4 : index < state.psc.size() * 4 ? std::bit_cast<uint32_t>(state.psc[index / 4][index % 4]) : 0; }
  if (group == 10) {
    const std::array<uint32_t, 10> words{state.rt.header, state.zs.header, state.index_buffer, state.base_vertex,
      state.viewport.x, state.viewport.y, state.viewport.w, state.viewport.h,
      std::bit_cast<uint32_t>(state.viewport.min_z), std::bit_cast<uint32_t>(state.viewport.max_z)};
    return index == UINT32_MAX ? words.size() : index < words.size() ? words[index] : 0;
  }
  if (group == 11) {
    if (index == UINT32_MAX) { return 24; }
    if (index >= 24) { return 0; }
    const uint32_t header = state.textures[index / 6];
    std::array<uint32_t, 6> words{header};
    if (header) { session->image->memory->Read({}, header, words.data() + 1, 20); }
    return words[index % 6];
  }
  if (group == 12) {
    if (index == UINT32_MAX) { return state.streams.size() * 2; }
    if (index >= state.streams.size() * 2) { return 0; }
    const auto& stream = state.streams[index / 2];
    return index % 2 ? stream.stride : stream.vb;
  }
  if (group == 14) {
    if (index == UINT32_MAX) { return 8; }
    if (index >= 8) { return 0; }
    const auto& target = index < 4 ? state.rt : state.zs;
    const std::array<uint32_t, 4> words{target.header, target.data, target.format, target.size};
    return words[index % 4];
  }
  if (group == 13) {
    const std::array<uint32_t, 3> words{state.index_buffer, state.index_data, state.base_vertex};
    return index == UINT32_MAX ? words.size() : index < words.size() ? words[index] : 0;
  }
  return 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t image_graphics_event_word(uint32_t index, uint32_t field) {
  if (!session || index >= session->graphicsTrace.count) { return 0; }
  const auto& event = session->graphicsTrace.events[index];
  const std::array<uint32_t, 9> words = {uint32_t(event.sequence), uint32_t(event.sequence >> 32), uint32_t(event.request),
    event.arguments[0], event.arguments[1], event.arguments[2], event.arguments[3], event.arguments[4], event.arguments[5]};
  return field < words.size() ? words[field] : 0;
}
EMSCRIPTEN_KEEPALIVE uint32_t image_graphics_resource_word(uint32_t index, uint32_t field) {
  if (!session || index >= session->graphicsTrace.resources.size()) { return 0; }
  const auto& resource = session->graphicsTrace.resources[index];
  const std::array<uint32_t, 6> words = {resource.header, resource.data, resource.bytes, resource.format, resource.size, uint32_t(resource.unavailable)};
  return field < words.size() ? words[field] : 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t image_thread_id() { return session ? browser::ImageExecutionThreadId(*session->image, session->execution) : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_entered() { return session && session->execution.entered; }
EMSCRIPTEN_KEEPALIVE uint32_t image_returned() { return session && session->execution.returned; }
EMSCRIPTEN_KEEPALIVE uint32_t image_tls_callbacks() { return session ? session->image->tlsCallbacks.size() : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_fault_address() { return session ? session->faultAddress : 0; }
EMSCRIPTEN_KEEPALIVE const char* image_fault_message() { return session ? session->faultMessage.data() : nullptr; }
EMSCRIPTEN_KEEPALIVE uint32_t image_fault_message_size() { return session ? session->faultMessage.size() : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_register(uint32_t index) {
  if (!session) { return 0; }
  const auto& ctx = session->execution.context;
  const std::array<uint32_t, 11> values = {ctx.eax, ctx.ecx, ctx.edx, ctx.ebx, ctx.esp, ctx.ebp, ctx.esi, ctx.edi, ctx.fs_base, ctx.last_icall, ctx.thread_handle};
  return index < values.size() ? values[index] : 0;
}

EMSCRIPTEN_KEEPALIVE int image_bind() {
  faultOrdinal = 0;
  if (!session) { return 0; }
  session->coverage.clear();
  try {
    session->coverage = browser::InspectImageImports(*session->image, session->runtime, session->dataExports);
    browser::BindImageImports(*session->image, session->runtime, session->dataExports);
    return 1;
  } catch (const browser::Fault& fault) {
    faultOrdinal = fault.address;
    return 0;
  } catch (const std::runtime_error&) {
    return 0;
  }
}

EMSCRIPTEN_KEEPALIVE uint32_t image_coverage_count() { return session ? session->coverage.size() : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_coverage_slot(uint32_t index) { return session && index < session->coverage.size() ? session->coverage[index].slot : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_coverage_ordinal(uint32_t index) { return session && index < session->coverage.size() ? session->coverage[index].ordinal : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_coverage_target(uint32_t index) { return session && index < session->coverage.size() ? session->coverage[index].target : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_coverage_kind(uint32_t index) { return session && index < session->coverage.size() ? uint32_t(session->coverage[index].kind) : 0; }

EMSCRIPTEN_KEEPALIVE uint32_t image_sections() { return session ? session->image->image.sections.size() : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_imports() { return session ? session->image->imports.size() : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_tls_bytes() { return session ? session->image->tlsTemplate.size() : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_backing() { return session ? session->image->memory->BackingBytes() : 0; }
EMSCRIPTEN_KEEPALIVE int image_entry_mapped() { return session && session->runtime.Find(X86EntryPoint); }
EMSCRIPTEN_KEEPALIVE int image_bound() { return session && session->image->importsBound; }
EMSCRIPTEN_KEEPALIVE uint32_t image_fault_ordinal() { return faultOrdinal; }
EMSCRIPTEN_KEEPALIVE uint32_t image_thread_session_word(uint32_t field) {
  if (!session || !session->execution.entered) { return 0; }
  const auto& snapshot = session->threadSession;
  const std::array<uint32_t, 9> words = {snapshot.childrenCreated, snapshot.childrenCompleted, snapshot.handlesClosed, snapshot.childrenSuspended,
    snapshot.objectReferences, uint32_t(snapshot.requestedStackBytes), uint32_t(snapshot.requestedStackBytes >> 32),
    uint32_t(snapshot.committedBytes), uint32_t(snapshot.committedBytes >> 32)};
  return field < words.size() ? words[field] : 0;
}

EMSCRIPTEN_KEEPALIVE uint32_t image_active_workers() { return session ? session->runtime.ActiveWorkers() : 0; }

}
