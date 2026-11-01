#include "browser/image.h"
#include "browser/kernel-provider.h"
#include "browser/thread-provider.h"
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
#include "build_identity.h"
#include <emscripten.h>
#include <array>

namespace {
struct Session {
  browser::Runtime runtime;
  std::unique_ptr<browser::PreparedImage> image;
  std::vector<browser::KernelDataExport> dataExports;
  std::vector<browser::BoundImport> coverage;
  browser::ImageExecution execution;
  browser::Runtime::KernelTraceSnapshot kernelTrace;
  std::shared_ptr<const browser::SdkTrace> sdkObserver;
  browser::SdkTrace::SnapshotData sdkTrace;
  std::shared_ptr<browser::DiagnosticGraphics> graphics;
  browser::DiagnosticGraphics::SnapshotData graphicsTrace;
  std::shared_ptr<browser::SilentAudioSink> silentAudio;
  browser::SilentAudioSnapshot audioTrace;
  std::function<browser::DiscReadSnapshot()> snapshotDiscReads;
  browser::DiscReadSnapshot discReads;
  bool graphicsCheckpoint = false;
  std::string faultMessage;
  uint32_t faultAddress = 0;
  Session(std::vector<uint8_t> bytes, std::span<const X86FuncMapping> mappings, std::span<const uint32_t> continuations)
    : runtime(mappings, continuations), image(browser::PrepareImage(std::move(bytes), {X86ImageBase, X86ImageEnd, X86EntryPoint})) {
    if (!runtime.Find(X86EntryPoint)) { throw std::runtime_error("generated entry body is absent"); }
    browser::InstallSectionProvider(runtime, *image);
    browser::InstallKernelProvider(runtime, *image->memory);
    browser::QueryContiguousMemoryUsage contiguousUsage;
    browser::ContiguousMemoryAccess contiguousAccess;
    browser::InstallContiguousMemory(runtime, *image->memory, &contiguousUsage, &contiguousAccess);
    graphics = std::make_shared<browser::DiagnosticGraphics>(runtime, *image, std::move(contiguousAccess), browser::GraphicsMode::Checkpoint);
    browser::InstallVirtualMemory(runtime, *image, std::move(contiguousUsage));
    browser::InstallStringProvider(runtime, *image);
    auto fileCloser = browser::InstallFileProvider(runtime, *image, browser::MountedIsoDisc(), {}, &dataExports, &snapshotDiscReads);
    browser::InstallThreadProvider(runtime, *image, std::move(fileCloser));
    browser::InstallShutdownProvider(runtime, *image);
    browser::InstallDpcProvider(runtime, *image);
    const auto timers = browser::InstallTimerProvider(runtime, *image);
    dataExports.push_back(browser::InstallTimerTickCount(runtime, *image, timers));
    silentAudio = std::make_shared<browser::SilentAudioSink>(runtime, *image);
    browser::InstallTimerClockObserver(runtime, *image, timers,
      [sink = silentAudio](const X86Context& context, uint64_t now) { sink->Advance(context, now); });
    dataExports.push_back(browser::InstallVirtualDriveKey(runtime, *image));
    const auto platform = browser::InstallPlatformProfile(runtime, *image);
    dataExports.insert(dataExports.end(), platform.begin(), platform.end());
    sdkObserver = browser::InstallSdkProvider(runtime, *image, browser::SdkPolicy::UnavailableXNet, graphics);
  }
  ~Session() { runtime.Stop(); }
};
std::vector<uint8_t> input;
std::unique_ptr<Session> session;
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
    session->kernelTrace = session->runtime.SnapshotKernelTrace();
    session->sdkTrace = session->sdkObserver->Snapshot();
    session->discReads = session->snapshotDiscReads();
    session->graphicsTrace = session->graphics->Snapshot();
    session->audioTrace = session->silentAudio->Snapshot();
    return 1;
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
  } catch (const std::runtime_error& fault) {
    session->faultMessage = fault.what();
  }
  session->kernelTrace = session->runtime.SnapshotKernelTrace();
  session->sdkTrace = session->sdkObserver->Snapshot();
  session->discReads = session->snapshotDiscReads();
  session->graphicsTrace = session->graphics->Snapshot();
  session->audioTrace = session->silentAudio->Snapshot();
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
    default: return 0;
  }
}
EMSCRIPTEN_KEEPALIVE uint32_t image_graphics_checkpoint() { return session && session->graphicsCheckpoint ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE uint32_t image_graphics_info(uint32_t field) {
  if (!session) { return 0; }
  const auto& trace = session->graphicsTrace;
  const std::array<uint32_t, 6> words = {uint32_t(trace.initialized), uint32_t(trace.total), uint32_t(trace.total >> 32),
    uint32_t(trace.count), uint32_t(trace.resources.size()), trace.commandWords};
  return field < words.size() ? words[field] : 0;
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
EMSCRIPTEN_KEEPALIVE uint32_t image_active_workers() { return session ? session->runtime.ActiveWorkers() : 0; }

}
