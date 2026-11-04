#pragma once
#include "browser/cpu_runtime.h"
#include "xbe/xbe.h"

namespace browser {

struct ImageLayout {
  uint32_t base;
  uint32_t end;
  uint32_t entry;
};

struct ImageImport {
  uint32_t slot;
  uint32_t ordinal;
};

struct KernelDataExport {
  uint32_t ordinal;
  uint32_t address;
  uint32_t size;
};

enum class ImportKind { Missing, Function, Data, FunctionTrap, DataGuard };

struct UnsupportedImport : Fault {
  uint32_t ordinal;
  UnsupportedImport(const X86Context& context, uint32_t ordinal)
    : Fault(context, ordinal, 0, "unsupported kernel function called"), ordinal(ordinal) {}
};

struct BoundImport {
  uint32_t slot;
  uint32_t ordinal;
  ImportKind kind;
  uint32_t target;
};

inline constexpr uint32_t kImageThreadRegionBase = 0xd0000000;
inline constexpr uint32_t kImageThreadRegionCeiling = 0xf0000000;
inline constexpr uint32_t kImageThreadSlotFloor = 0x3000;
inline constexpr uint32_t kImageThreadStackBudget = 16u << 20;
inline constexpr uint32_t kImageThreadTlsBudget = (1u << 20) + 4;
inline constexpr uint32_t kImageThreadSlotSpan =
  kImageThreadSlotFloor + ((kImageThreadStackBudget + kImageThreadTlsBudget + 4095) & ~4095u);
inline constexpr size_t kImageThreadCapacity = 128;
static_assert(kImageThreadSlotSpan == 0x1104000);
static_assert(kImageThreadSlotSpan < kImageThreadRegionCeiling - kImageThreadRegionBase);

struct ThreadSessionCounters {
  std::atomic<uint32_t> childrenCreated = 0;
  std::atomic<uint32_t> childrenCompleted = 0;
  std::atomic<uint32_t> handlesClosed = 0;
  std::atomic<uint32_t> childrenSuspended = 0;
  std::atomic<uint32_t> objectReferences = 0;
  std::atomic<uint64_t> requestedStackBytes = 0;
  std::atomic<uint64_t> committedBytes = 0;
};

struct ThreadWaitBridge {
  std::function<uint32_t(uint32_t)> publish;
  std::function<void(uint32_t, uint32_t)> rollback;
  std::function<void(uint32_t)> complete;
  std::function<void(uint32_t)> close;
};

struct ImageThread {
  uint32_t id;
  uint32_t kpcr;
  uint32_t ethread;
  uint32_t stackLimit;
  uint32_t stackTop;
  uint32_t tls;
  uint32_t tlsData;
  X86Context context;
};

struct PreparedImage {
private:
  std::shared_ptr<const xbe::Image> authenticatedSource_;
  friend std::unique_ptr<PreparedImage> PrepareImage(std::vector<uint8_t> authenticatedBytes, ImageLayout layout);
public:
  std::shared_ptr<const xbe::Image> AuthenticatedSource() const { return authenticatedSource_; }
  xbe::Image image;
  std::unique_ptr<Memory> memory;
  std::vector<ImageImport> imports;
  std::vector<BoundImport> boundImports;
  std::vector<BoundImport> diagnosticImports;
  bool importsBound = false;
  Runtime* boundRuntime = nullptr;
  Runtime* diagnosticRuntime = nullptr;
  Runtime* threadProviderRuntime = nullptr;
  Runtime* synchronizationProviderRuntime = nullptr;
  std::shared_ptr<ThreadWaitBridge> threadWaitBridge;
  std::shared_ptr<ThreadSessionCounters> threadSessionCounters;
  std::function<void(uint32_t)> threadRetirement;
  mutable std::mutex threadLock;
  std::mutex dpcDeliveryLock;
  std::vector<ImageThread> threads;
  std::vector<uint8_t> tlsTemplate;
  std::vector<uint32_t> tlsCallbacks;
};

std::unique_ptr<PreparedImage> PrepareImage(std::vector<uint8_t> authenticatedBytes, ImageLayout layout);
std::vector<BoundImport> InspectImageImports(PreparedImage& prepared, Runtime& runtime, std::span<const KernelDataExport> dataExports);
void BindImageImports(PreparedImage& prepared, Runtime& runtime, std::span<const KernelDataExport> dataExports);
struct ImageThreadRetirement {
  PreparedImage& image;
  const X86Context& context;
  ~ImageThreadRetirement();
};

struct ImageExecution {
  bool entered = false;
  bool returned = false;
  X86Context context{};
};

void ExecuteImageEntry(PreparedImage& prepared, Runtime& runtime, ImageExecution& execution, std::function<void(X86Context&)> initialize = {});
uint32_t ImageExecutionThreadId(const PreparedImage& prepared, const ImageExecution& execution);
void CompleteImageThread(PreparedImage& prepared, Runtime& runtime, const X86Context& context, uint32_t status);
void DeclareDiagnosticImport(PreparedImage& prepared, Runtime& runtime, uint32_t ordinal, bool data);
uint32_t DiagnosticDataOrdinal(const PreparedImage& prepared, uint32_t address);
ImageThread PrepareImageThread(PreparedImage& prepared, Runtime& runtime, uint32_t id, uint32_t stackSize);
void LaunchImageThread(PreparedImage& prepared, Runtime& runtime, const X86Context& parent, uint32_t id,
  uint32_t stackSize, const std::function<void(const ImageThread&)>& launch);

}
