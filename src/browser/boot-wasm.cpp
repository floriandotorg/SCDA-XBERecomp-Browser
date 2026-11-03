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
#include "browser/sdk-provider.h"
#include "browser/diagnostic-graphics.h"
#include "browser/silent-audio.h"
#include "browser/audio-provider.h"
#include "build_identity.h"
#include <emscripten.h>
#include <array>
#include <bit>
#include <mutex>
#include <optional>
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
#include "browser/menu-input.h"
#include "browser/av-provider.h"
#include "browser/delay-provider.h"
#include "browser/menu-launch.h"
#include "browser/menu-route.h"
#endif
#ifdef BROWSER_MENU
#include "browser/menu-backend.h"
#endif

namespace {
#ifdef BROWSER_MENU
constexpr auto graphics_mode = browser::GraphicsMode::Rendering;
constexpr size_t worker_capacity = browser::kImageThreadCapacity;
#elif defined(BROWSER_MENU_CAPTURE)
constexpr auto graphics_mode = browser::GraphicsMode::StateCapture;
constexpr size_t worker_capacity = 2;
#else
constexpr auto graphics_mode = browser::GraphicsMode::Checkpoint;
constexpr size_t worker_capacity = 2;
#endif
struct Session {
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
  Session(std::vector<uint8_t> bytes, std::span<const X86FuncMapping> mappings, std::span<const uint32_t> continuations)
    : runtime(mappings, continuations, {}, worker_capacity), image(browser::PrepareImage(std::move(bytes), {X86ImageBase, X86ImageEnd, X86EntryPoint})) {
    if (!runtime.Find(X86EntryPoint)) { throw std::runtime_error("generated entry body is absent"); }
    browser::InstallSectionProvider(runtime, *image);
    browser::InstallKernelProvider(runtime, *image->memory);
    browser::QueryContiguousMemoryUsage contiguousUsage;
    browser::ContiguousMemoryAccess contiguousAccess;
    browser::InstallContiguousMemory(runtime, *image->memory, &contiguousUsage, &contiguousAccess);
#ifdef BROWSER_MENU
    auto backend = std::make_unique<browser::MenuBackend>(runtime, *image, std::make_shared<browser::webgpu::Transport>());
    renderer = backend.get();
    graphics = std::make_shared<browser::DiagnosticGraphics>(runtime, *image, std::move(contiguousAccess), graphics_mode, std::move(backend));
    graphics->ConfigurePresentCheckpoint(BROWSER_MENU_TRACE_FRAMES);
#else
    graphics = std::make_shared<browser::DiagnosticGraphics>(runtime, *image, std::move(contiguousAccess), graphics_mode);
#endif
    browser::InstallVirtualMemory(runtime, *image, std::move(contiguousUsage));
    browser::InstallStringProvider(runtime, *image);
    std::function<void(browser::FileOpenDiagnostic)> reportFileOpen;
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
    profileStorage = std::make_shared<browser::TemporaryFilesystem>(32u << 20);
#endif
    auto fileCloser = browser::InstallFileProvider(runtime, *image, mountedDisc, std::move(profileStorage), &dataExports, &snapshotDiscReads,
      std::move(reportFileOpen), &synchronizationRoutes->ownsFile);
    auto synchronizationCloser = browser::InstallSynchronizationProvider(runtime, *image, synchronizationRoutes);
    dataExports.push_back(browser::InstallThreadProvider(runtime, *image, [fileCloser, synchronizationCloser](uint32_t handle) {
      return fileCloser(handle) || synchronizationCloser(handle);
    }, &synchronizationRoutes->ownsThread));
    browser::InstallShutdownProvider(runtime, *image);
    browser::InstallDpcProvider(runtime, *image);
    const auto timers = browser::InstallTimerProvider(runtime, *image);
    dataExports.push_back(browser::InstallTimerTickCount(runtime, *image, timers));
    silentAudio = std::make_shared<browser::SilentAudioSink>(runtime, *image);
    dataExports.push_back(browser::InstallVirtualDriveKey(runtime, *image));
    const auto platform = browser::InstallPlatformProfile(runtime, *image);
    dataExports.insert(dataExports.end(), platform.begin(), platform.end());
    dataExports.push_back(browser::InstallSignatureKey(runtime, *image));
    audioProvider = browser::InstallAudioProvider(runtime, *image, silentAudio, mountedDisc);
    browser::InstallTimerClockObserver(runtime, *image, timers,
      [sink = silentAudio, provider = audioProvider](const X86Context& context, uint64_t now) {
        sink->Advance(context, now);
        provider->Tick(context, now);
      });
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
    browser::InstallAvProvider(runtime, *image->memory);
    browser::InstallDelayProvider(runtime);
    input = std::make_shared<browser::MenuInput>(runtime, *image);
    launcher = std::make_shared<browser::MenuLaunch>(runtime, *image);
    route = std::make_shared<browser::MenuRoute>(runtime, *image);
    missionObserver = std::make_shared<browser::MenuMissionObservation>(runtime, *image);
    runtime.InstallInvocationObserver(*image->memory, [observer = missionObserver](const X86Context& ctx, uint8_t* base, uint32_t target, uint64_t workerId) {
      observer->Observe(ctx, base, target, workerId);
    });
    sdkObserver = browser::InstallSdkProvider(runtime, *image, browser::SdkPolicy::UnavailableXNet, graphics, input, launcher, route, audioProvider);
#else
    sdkObserver = browser::InstallSdkProvider(runtime, *image, browser::SdkPolicy::UnavailableXNet, graphics, {}, {}, {}, audioProvider);
#endif
  }
  ~Session() {
    runtime.Stop();
    if (graphics) { graphics->Shutdown(); }
  }
};
std::vector<uint8_t> input;
std::unique_ptr<Session> session;
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
      std::span(X86FuncMappings, mappingCount), std::span(X86Continuations, continuationCount));
    return 1;
  } catch (const std::runtime_error&) {
    return 0;
  }
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
    browser::ExecuteImageEntry(*session->image, session->runtime, session->execution);
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
  session->graphicsTrace = session->graphics->Snapshot();
  session->audioTrace = session->silentAudio->Snapshot();
  session->audioProviderTrace = session->audioProvider->Snapshot();
#if defined(BROWSER_MENU) || defined(BROWSER_MENU_CAPTURE)
  session->missionTrace = session->missionObserver->Snapshot();
#endif
  return 0;
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
  if (!session || !session->input || (publishedInputStorage && publishedInputStorage != session->input)) { return 0; }
  publishedInputStorage = session->input;
  return reinterpret_cast<uintptr_t>(publishedInputStorage->SharedAddress());
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
