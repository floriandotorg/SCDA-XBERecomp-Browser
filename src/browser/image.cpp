#include "browser/image.h"
#include <algorithm>
#include <array>

namespace browser {

std::unique_ptr<PreparedImage> PrepareImage(std::vector<uint8_t> authenticatedBytes, ImageLayout layout) {
  auto prepared = std::make_unique<PreparedImage>();
  auto& image = prepared->image;
  std::string error;
  if (!image.Parse(std::move(authenticatedBytes), error)) { throw std::runtime_error(error); }
  if (image.base != layout.base || uint64_t(image.base) + image.image_size != layout.end || image.entry != layout.entry) {
    throw std::runtime_error("original image layout does not match generated code");
  }
  if (image.base % 4096 || uint64_t(image.image_size) + 4095 > UINT32_MAX) {
    throw std::runtime_error("unsupported original image alignment");
  }
  const auto requirePreload = [&](uint32_t address, uint32_t size) {
    const auto* section = image.SectionAt(address);
    if (!section || !(section->flags & xbe::kPreload) || uint64_t(address) + size > uint64_t(section->va) + section->vsize) {
      throw std::runtime_error("startup metadata requires an unloaded section");
    }
  };
  requirePreload(image.entry, 1);
  for (uint32_t n = 0; n < 4096; ++n) {
    const uint32_t slot = image.kernel_thunk + n * 4;
    requirePreload(slot, 4);
    uint32_t value = 0;
    if (!image.Read32(slot, value)) { throw std::runtime_error("original import bytes unavailable"); }
    if (!value) { break; }
    prepared->imports.push_back({slot, value & 0x7fffffff});
  }
  if (image.tls_addr) {
    requirePreload(image.tls_addr, 24);
    requirePreload(image.tls.index_addr, 4);
    const uint32_t initialized = image.tls.data_end - image.tls.data_start;
    prepared->tlsTemplate.resize(initialized + image.tls.zero_fill);
    if (initialized) {
      requirePreload(image.tls.data_start, initialized);
      const auto offset = image.Offset(image.tls.data_start);
      std::copy_n(image.file.data() + offset, initialized, prepared->tlsTemplate.data());
    }
    if (image.tls.callbacks) {
      for (uint32_t n = 0; n <= 256; ++n) {
        const uint32_t slot = image.tls.callbacks + n * 4;
        requirePreload(slot, 4);
        uint32_t callback = 0;
        if (!image.Read32(slot, callback)) { throw std::runtime_error("original TLS callback bytes unavailable"); }
        if (!callback) { break; }
        requirePreload(callback, 1);
        prepared->tlsCallbacks.push_back(callback);
      }
    }
  }
  prepared->memory = std::make_unique<Memory>();
  auto& memory = *prepared->memory;
  memory.Map(image.base, (image.image_size + 4095) & ~4095u);
  memory.Commit(image.base, image.header_size);
  memory.Write({}, image.base, image.file.data(), image.header_size);
  const uint32_t sectionTable = image.U32(0x120);
  for (size_t n = 0; n < image.sections.size(); ++n) {
    const auto& section = image.sections[n];
    const uint32_t referenceCount = (section.flags & xbe::kPreload) ? 1 : 0;
    if (referenceCount) {
      memory.Commit(section.va, section.vsize);
      memory.Write({}, section.va, image.file.data() + section.raw, section.rsize);
    }
    memory.Write({}, sectionTable + uint32_t(n) * 56 + 24, &referenceCount, 4);
  }
  prepared->authenticatedSource_ = std::make_shared<const xbe::Image>(image);
  return prepared;
}

void DeclareDiagnosticImport(PreparedImage& prepared, Runtime& runtime, uint32_t ordinal, bool data) {
  runtime.Checkpoint({});
  if (!prepared.memory || prepared.importsBound || runtime.ActiveWorkers() || !ordinal || ordinal >= 400 ||
      (prepared.diagnosticRuntime && prepared.diagnosticRuntime != &runtime) ||
      (prepared.threadProviderRuntime && prepared.threadProviderRuntime != &runtime) ||
      (prepared.synchronizationProviderRuntime && prepared.synchronizationProviderRuntime != &runtime)) {
    throw Fault({}, ordinal, 0, "diagnostic imports require an unpublished stopped image");
  }
  const auto imported = std::find_if(prepared.imports.begin(), prepared.imports.end(),
    [&](const auto& entry) { return entry.ordinal == ordinal; });
  if (imported == prepared.imports.end()) { return; }
  for (const auto& entry : prepared.diagnosticImports) {
    if (entry.ordinal == ordinal) { throw Fault({}, ordinal, 0, "duplicate diagnostic import declaration"); }
  }
  if (runtime.KernelTarget(ordinal)) {
    if (data) { throw Fault({}, ordinal, 0, "data declaration conflicts with a function"); }
    return;
  }
  prepared.diagnosticImports.reserve(prepared.diagnosticImports.size() + 1);
  const uint32_t target = data ? 0xc1000000 + ordinal * 4096 : 0xfe000000 + ordinal * 4;
  if (data) {
    prepared.memory->Map(target, 4096);
  } else {
    auto* memory = prepared.memory.get();
    runtime.RegisterKernel(ordinal, [&runtime, memory, ordinal](X86Context& context, uint8_t* base) {
      if (context.host != &runtime || base != memory->Base()) { throw Fault(context, ordinal, 0, "foreign diagnostic import session"); }
      runtime.Checkpoint(context);
      throw UnsupportedImport(context, ordinal);
    });
  }
  prepared.diagnosticImports.push_back({imported->slot, ordinal, data ? ImportKind::DataGuard : ImportKind::FunctionTrap, target});
  prepared.diagnosticRuntime = &runtime;
}

uint32_t DiagnosticDataOrdinal(const PreparedImage& prepared, uint32_t address) {
  for (const auto& entry : prepared.diagnosticImports) {
    if (entry.kind == ImportKind::DataGuard && address >= entry.target && uint64_t(address) < uint64_t(entry.target) + 4096) {
      return entry.ordinal;
    }
  }
  return 0;
}

std::vector<BoundImport> InspectImageImports(PreparedImage& prepared, Runtime& runtime, std::span<const KernelDataExport> dataExports) {
  runtime.Checkpoint({});
  if (prepared.diagnosticRuntime && prepared.diagnosticRuntime != &runtime) {
    throw Fault({}, 0, 0, "diagnostic imports belong to a different runtime");
  }
  if (prepared.threadProviderRuntime && prepared.threadProviderRuntime != &runtime) {
    throw Fault({}, 259, 0, "thread provider belongs to a different runtime");
  }
  if (prepared.synchronizationProviderRuntime && prepared.synchronizationProviderRuntime != &runtime) {
    throw Fault({}, 192, 0, "synchronization provider belongs to a different runtime");
  }
  if (runtime.ActiveWorkers() || prepared.importsBound || !prepared.memory || dataExports.size() > 399) {
    throw std::runtime_error("image imports require an unpublished stopped runtime");
  }
  auto& memory = *prepared.memory;
  auto lease = memory.Borrow();
  const uint32_t table = prepared.image.kernel_thunk;
  const size_t tableBytes = (prepared.imports.size() + 1) * 4;
  const auto tableStart = reinterpret_cast<uintptr_t>(memory.Span({}, table, tableBytes));
  std::array<uint32_t, 400> dataTargets{};
  for (const auto& data : dataExports) {
    if (!data.ordinal || data.ordinal >= dataTargets.size() || dataTargets[data.ordinal] ||
        !data.address || !data.size || runtime.KernelTarget(data.ordinal)) {
      throw Fault({}, data.ordinal, 0, "invalid or ambiguous kernel data export");
    }
    const auto dataStart = reinterpret_cast<uintptr_t>(memory.Span({}, data.address, data.size));
    if (dataStart < tableStart + tableBytes && tableStart < dataStart + data.size) {
      throw Fault({}, data.ordinal, 0, "kernel data export aliases the import table");
    }
    dataTargets[data.ordinal] = data.address;
  }
  std::vector<BoundImport> ledger;
  ledger.reserve(prepared.imports.size());
  for (const auto& import : prepared.imports) {
    uint32_t original = 0;
    memory.Read({}, import.slot, &original, 4);
    if (original != (0x80000000 | import.ordinal)) {
      throw Fault({}, import.slot, 4, "original import slot was modified");
    }
    const uint32_t data = dataTargets.at(import.ordinal);
    const auto diagnostic = std::find_if(prepared.diagnosticImports.begin(), prepared.diagnosticImports.end(),
      [&](const auto& entry) { return entry.ordinal == import.ordinal; });
    if (diagnostic != prepared.diagnosticImports.end()) {
      if (data) { throw Fault({}, import.ordinal, 0, "diagnostic import conflicts with initialized data"); }
      if (diagnostic->kind == ImportKind::DataGuard && runtime.KernelTarget(import.ordinal)) {
        throw Fault({}, import.ordinal, 0, "diagnostic data guard conflicts with a function");
      }
      ledger.push_back({import.slot, import.ordinal, diagnostic->kind, diagnostic->target});
    } else {
      const uint32_t target = data ? data : runtime.KernelTarget(import.ordinal);
      ledger.push_back({import.slot, import.ordinal, !target ? ImportKind::Missing : data ? ImportKind::Data : ImportKind::Function, target});
    }
  }
  uint32_t terminator = 0;
  memory.Read({}, table + uint32_t(tableBytes) - 4, &terminator, 4);
  if (terminator) { throw Fault({}, table, tableBytes, "original import terminator was modified"); }
  runtime.Checkpoint({});
  return ledger;
}

void BindImageImports(PreparedImage& prepared, Runtime& runtime, std::span<const KernelDataExport> dataExports) {
  if (!prepared.memory) { throw std::runtime_error("image imports require prepared memory"); }
  auto lease = prepared.memory->Borrow();
  const auto revision = runtime.KernelRevision();
  auto ledger = InspectImageImports(prepared, runtime, dataExports);
  for (const auto& import : ledger) {
    if (!import.target) { throw Fault({}, import.ordinal, 0, "unavailable image import ordinal"); }
  }
  runtime.Checkpoint({});
  auto& memory = *prepared.memory;
  runtime.PublishImageMemory(memory, revision, [&] {
    for (const auto& import : ledger) { memory.Write({}, import.slot, &import.target, 4); }
    prepared.boundImports = std::move(ledger);
    prepared.importsBound = true;
    prepared.boundRuntime = &runtime;
  });
}

ImageThreadRetirement::~ImageThreadRetirement() {
  if (image.threadRetirement) { image.threadRetirement(context.fs_base); }
}

void ExecuteImageEntry(PreparedImage& prepared, Runtime& runtime, ImageExecution& execution) {
  if (execution.entered || !prepared.threads.empty()) { throw Fault({}, 0, 0, "initial image thread already prepared"); }
  if (!prepared.tlsCallbacks.empty()) {
    throw Fault({}, prepared.tlsCallbacks.front(), 0, "Xbox TLS callback invocation contract is not integrated");
  }
  if (!runtime.Find(prepared.image.entry)) { throw Fault({}, prepared.image.entry, 0, "generated entry body is absent"); }
  const auto thread = PrepareImageThread(prepared, runtime, 0x10, prepared.image.stack_commit);
  runtime.Launch(thread.context, prepared.memory->Base(), [&](X86Context& context, uint8_t* base) {
    ImageThreadRetirement retirement{prepared, context};
    execution.entered = true;
    try {
      const uint32_t status = runtime.Call(context, base, prepared.image.entry, {0});
      runtime.Checkpoint(context);
      CompleteImageThread(prepared, runtime, context, status);
      execution.returned = true;
      execution.context = context;
    } catch (...) {
      execution.context = context;
      throw;
    }
  });
  if (const auto failure = runtime.Join()) {
    try { std::rethrow_exception(failure); }
    catch (const Fault& fault) {
      execution.context = *fault.context;
      throw;
    }
  }
  runtime.Checkpoint(execution.context);
}

uint32_t ImageExecutionThreadId(const PreparedImage& prepared, const ImageExecution& execution) {
  std::lock_guard lock(prepared.threadLock);
  for (const auto& thread : prepared.threads) {
    if (thread.kpcr == execution.context.fs_base) { return thread.id; }
  }
  return 0;
}

void CompleteImageThread(PreparedImage& prepared, Runtime& runtime, const X86Context& context, uint32_t status) {
  if (!runtime.OwnsCurrentContext(context) || !prepared.importsBound || prepared.boundRuntime != &runtime) {
    throw Fault(context, context.fs_base, 0, "thread completion requires its owned image worker");
  }
  std::lock_guard lock(prepared.threadLock);
  const auto thread = std::find_if(prepared.threads.begin(), prepared.threads.end(),
    [&](const auto& entry) { return entry.kpcr == context.fs_base; });
  if (thread == prepared.threads.end()) { throw Fault(context, context.fs_base, 0, "thread completion storage is absent"); }
  auto lease = prepared.memory->Borrow();
  prepared.memory->CheckRange(context, thread->ethread, 0x124);
  const uint32_t signal = 1;
  prepared.memory->Write(context, thread->ethread + 0x120, &status, 4);
  prepared.memory->Write(context, thread->ethread + 4, &signal, 4);
}

static ImageThread prepareImageThread(PreparedImage& prepared, Runtime& runtime, uint32_t id, uint32_t stackSize) {
  runtime.Checkpoint({});
  if (!prepared.memory || !prepared.importsBound || prepared.boundRuntime != &runtime ||
      prepared.threads.size() >= kImageThreadCapacity || !id || id % 4 || stackSize > kImageThreadStackBudget) {
    throw Fault({}, id, stackSize, "thread preparation requires its bound image and bounded storage");
  }
  for (const auto& thread : prepared.threads) {
    if (thread.id == id) { throw Fault({}, id, 0, "duplicate image thread identifier"); }
  }
  auto& memory = *prepared.memory;
  const bool hasTls = prepared.image.tls_addr != 0;
  if (prepared.tlsTemplate.size() > (1u << 20)) { throw Fault({}, id, 0, "thread TLS template budget exhausted"); }
  const uint32_t tlsSize = hasTls ? ((uint32_t(prepared.tlsTemplate.size()) + 15) & ~15u) + 4 : 0;
  const uint32_t tlsIndex = 0u - tlsSize / 4;
  const uint32_t indexAddress = prepared.image.tls.index_addr;
  if (hasTls) {
    const uint64_t tableEnd = uint64_t(prepared.image.kernel_thunk) + (prepared.imports.size() + 1) * 4;
    if (uint64_t(indexAddress) < tableEnd && uint64_t(indexAddress) + 4 > prepared.image.kernel_thunk) {
      throw Fault({}, indexAddress, 4, "thread TLS index overlaps image imports");
    }
    uint32_t currentIndex = 0;
    memory.Read({}, indexAddress, &currentIndex, 4);
    if (!prepared.threads.empty() && currentIndex != tlsIndex) {
      throw Fault({}, indexAddress, 4, "thread TLS index was modified");
    }
  }
  prepared.threads.reserve(kImageThreadCapacity);
  const uint32_t region = prepared.threads.empty() ? kImageThreadRegionBase : prepared.threads.back().stackTop + 4096;
  const uint32_t stackSpan = (std::max(stackSize, 0x10000u) + tlsSize + 4095) & ~4095u;
  const uint32_t regionSize = kImageThreadSlotFloor + stackSpan;
  if (uint64_t(region) + regionSize > kImageThreadRegionCeiling) {
    throw Fault({}, region, regionSize, "image thread address capacity exhausted");
  }
  ImageThread thread{};
  thread.id = id;
  thread.kpcr = region + 0x1000;
  thread.ethread = thread.kpcr + 0x400;
  thread.stackLimit = region + kImageThreadSlotFloor;
  thread.stackTop = thread.stackLimit + stackSpan;
  thread.tls = thread.stackTop - tlsSize;
  thread.tlsData = hasTls ? thread.tls + 4 : 0;
  thread.context.esp = (thread.tls & ~15u) - 16;
  thread.context.fs_base = thread.kpcr;
  thread.context.fcw = 0x27f;
  thread.context.mxcsr = 0x1f80;
  thread.context.host = &runtime;
  memory.Map(region, regionSize);
  struct PendingMapping {
    Memory& memory;
    uint32_t address;
    uint32_t size;
    bool published = false;
    ~PendingMapping() { if (!published) { memory.Discard(address, size); } }
  } pending{memory, region, regionSize};
  memory.Commit(thread.kpcr, 4096);
  memory.Commit(thread.stackLimit, stackSpan);
  {
    auto lease = memory.Borrow();
    if (hasTls) { memory.CheckRange({}, indexAddress, 4); }
    runtime.Checkpoint(thread.context);
    const auto write = [&](uint32_t address, uint32_t value) { memory.Write({}, address, &value, 4); };
    write(thread.kpcr, UINT32_MAX);
    write(thread.kpcr + 4, thread.stackTop);
    write(thread.kpcr + 8, thread.stackLimit);
    write(thread.kpcr + 0x18, thread.kpcr);
    write(thread.kpcr + 0x1c, thread.kpcr);
    write(thread.kpcr + 0x20, thread.kpcr + 0x28);
    write(thread.kpcr + 0x28, thread.ethread);
    write(thread.ethread, 0x00800006);
    write(thread.ethread + 0x120, 0x103);
    write(thread.ethread + 0x1c, thread.stackTop);
    write(thread.ethread + 0x20, thread.stackLimit);
    write(thread.ethread + 0x28, thread.tls);
    write(thread.ethread + 0x12c, id);
    if (hasTls) {
      write(thread.tls, thread.tlsData);
      memory.Write({}, thread.tlsData, prepared.tlsTemplate.data(), prepared.tlsTemplate.size());
      write(indexAddress, tlsIndex);
    }
    prepared.threads.push_back(thread);
    pending.published = true;
  }
  return thread;
}

ImageThread PrepareImageThread(PreparedImage& prepared, Runtime& runtime, uint32_t id, uint32_t stackSize) {
  std::lock_guard lock(prepared.threadLock);
  if (runtime.ActiveWorkers()) { throw Fault({}, id, stackSize, "cold thread preparation requires a stopped runtime"); }
  return prepareImageThread(prepared, runtime, id, stackSize);
}

void LaunchImageThread(PreparedImage& prepared, Runtime& runtime, const X86Context& parent, uint32_t id,
    uint32_t stackSize, const std::function<void(const ImageThread&)>& launch) {
  if (Memory::HasBorrow()) { throw Fault(parent, id, 0, "child preparation during backing lease"); }
  std::lock_guard lock(prepared.threadLock);
  const bool ownsParent = std::any_of(prepared.threads.begin(), prepared.threads.end(),
    [&](const auto& thread) { return thread.kpcr == parent.fs_base; });
  if (!launch || !runtime.OwnsCurrentWorker() || parent.host != &runtime || !ownsParent) {
    throw Fault(parent, id, 0, "child preparation requires an owned guest parent");
  }
  const auto thread = prepareImageThread(prepared, runtime, id, stackSize);
  try { launch(thread); }
  catch (...) {
    prepared.threads.pop_back();
    prepared.memory->Discard(thread.kpcr - 0x1000, thread.stackTop - thread.kpcr + 0x1000);
    throw;
  }
}

}
