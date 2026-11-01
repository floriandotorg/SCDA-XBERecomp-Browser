#include "browser/dpc-provider.h"
#include <algorithm>
#include <array>

namespace browser {

static void DeliverDpc(PreparedImage& image, Runtime& runtime, const X86Context& interrupted, uint32_t address,
    std::optional<uint64_t> systemTime, const std::unique_lock<std::mutex>& delivery) {
  if (!image.importsBound || image.boundRuntime != &runtime || !runtime.OwnsCurrentContext(interrupted)) {
    throw Fault(interrupted, address, 28, "DPC delivery requires its current owned image context");
  }
  if (Memory::HasBorrow()) { throw Fault(interrupted, address, 28, "DPC delivery during backing lease"); }
  runtime.Checkpoint(interrupted);
  if (!delivery.owns_lock() || delivery.mutex() != &image.dpcDeliveryLock) {
    throw Fault(interrupted, address, 28, "DPC delivery requires its session delivery lock");
  }
  ImageThread thread;
  {
    std::lock_guard lock(image.threadLock);
    const auto found = std::find_if(image.threads.begin(), image.threads.end(),
      [&](const auto& candidate) { return candidate.kpcr == interrupted.fs_base; });
    if (found == image.threads.end()) { throw Fault(interrupted, address, 28, "DPC delivery has no prepared image thread"); }
    thread = *found;
  }
  const uint32_t stack = interrupted.esp;
  if (!address || address % 4 || uint64_t(stack) < uint64_t(thread.stackLimit) + 20 || stack > thread.tls ||
      (uint64_t(address) < stack && uint64_t(address) + 28 > stack - 20)) {
    throw Fault(interrupted, address, 28, "invalid DPC storage or callback stack");
  }
  auto& memory = *image.memory;
  std::array<uint32_t, 7> deferred;
  uint8_t previousIrql = 0;
  const uint8_t dispatchIrql = 2;
  {
    auto lease = memory.Borrow();
    memory.Read(interrupted, address, deferred.data(), sizeof(deferred));
    if ((deferred[0] & 0xffffff) != 19 || !runtime.Find(deferred[3])) {
      throw Fault(interrupted, address, 28, "DPC delivery requires an unqueued initialized generated routine");
    }
    memory.CheckRange(interrupted, stack - 20, 20);
    memory.Read(interrupted, thread.kpcr + 0x24, &previousIrql, 1);
    if (previousIrql > dispatchIrql) { throw Fault(interrupted, thread.kpcr + 0x24, 1, "DPC delivery above dispatch level"); }
    runtime.Checkpoint(interrupted);
    if (systemTime) {
      deferred[5] = uint32_t(*systemTime);
      deferred[6] = uint32_t(*systemTime >> 32);
      memory.Write(interrupted, address + 20, &deferred[5], 8);
    }
    memory.Write(interrupted, thread.kpcr + 0x24, &dispatchIrql, 1);
  }
  auto callback = interrupted;
  std::exception_ptr failure;
  try {
    runtime.Call(callback, memory.Base(), deferred[3], {address, deferred[4], deferred[5], deferred[6]});
    runtime.Checkpoint(callback);
  } catch (const std::exception&) { failure = std::current_exception(); }
  catch (...) { failure = std::make_exception_ptr(Fault(callback, deferred[3], 0, "DPC callback escaped its invocation")); }
  try { memory.Write(interrupted, thread.kpcr + 0x24, &previousIrql, 1); }
  catch (...) {
    runtime.Cancel();
    if (!failure) { throw; }
  }
  if (failure) { std::rethrow_exception(failure); }
}

bool DeliverImageDpc(PreparedImage& image, Runtime& runtime, const X86Context& interrupted, uint32_t address) {
  std::unique_lock delivery(image.dpcDeliveryLock, std::try_to_lock);
  if (!delivery.owns_lock()) { return false; }
  DeliverDpc(image, runtime, interrupted, address, {}, delivery);
  return true;
}

void DeliverLockedTimerDpc(PreparedImage& image, Runtime& runtime, const X86Context& interrupted,
    uint32_t address, uint64_t systemTime, const std::unique_lock<std::mutex>& delivery) {
  DeliverDpc(image, runtime, interrupted, address, systemTime, delivery);
}

void InstallDpcProvider(Runtime& runtime, PreparedImage& image) {
  runtime.RegisterKernel(107, [&runtime, &image](X86Context& context, uint8_t* base) {
    if (context.host != &runtime || base != image.memory->Base() || !image.importsBound ||
        image.boundRuntime != &runtime || !runtime.OwnsCurrentWorker()) {
      throw Fault(context, 107, 0, "DPC initialization requires its owned image worker");
    }
    if (Memory::HasBorrow()) { throw Fault(context, 107, 0, "DPC initialization during backing lease"); }
    const uint64_t stackEnd = uint64_t(context.esp) + 16;
    if (stackEnd > UINT32_MAX) { throw Fault(context, context.esp, 16, "DPC initialization frame overflow"); }
    auto lease = image.memory->Borrow();
    std::array<uint32_t, 4> frame;
    image.memory->Read(context, context.esp, frame.data(), sizeof(frame));
    const uint32_t address = frame[1];
    if (!address || address % 4 || !runtime.Find(frame[2])) {
      throw Fault(context, address, 28, "invalid DPC storage or unsupported deferred routine");
    }
    image.memory->CheckRange(context, address, 28);
    runtime.Checkpoint(context);
    const std::array<uint8_t, 3> header = {19, 0, 0};
    image.memory->Write(context, address, header.data(), header.size());
    image.memory->Write(context, address + 12, &frame[2], 8);
    context.esp = uint32_t(stackEnd);
  });
}

}
