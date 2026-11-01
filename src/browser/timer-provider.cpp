#include "browser/timer-provider.h"
#include "browser/dpc-provider.h"
#include <array>
#include <bit>

namespace browser {

void TimerQueue::RequireWorker(const X86Context& context) const {
  if (context.host != &runtime_ || !image_.importsBound || image_.boundRuntime != &runtime_ ||
      !runtime_.OwnsCurrentContext(context) || ((tickInstalled_ || clockObserver_) && image_.memory.get() != memory_)) {
    throw Fault(context, 149, 0, "timer queue requires its owned image worker");
  }
  if (Memory::HasBorrow()) { throw Fault(context, 149, 0, "timer queue operation during backing lease"); }
  runtime_.Checkpoint(context);
}

bool TimerQueue::Arm(const X86Context& context, uint32_t timer, int64_t dueTime, uint32_t dpc) {
  RequireWorker(context);
  std::lock_guard lock(lock_);
  auto lease = image_.memory->Borrow();
  return ArmLocked(context, timer, dueTime, dpc, false);
}

bool TimerQueue::ArmLocked(const X86Context& context, uint32_t timer, int64_t dueTime, uint32_t dpc, bool publish) {
  auto* destination = static_cast<std::optional<Schedule>*>(nullptr);
  bool inserted = false;
  bool published = false;
  for (auto& schedule : schedules_) {
    if (schedule && schedule->expiration.timer == timer) {
      destination = &schedule;
      inserted = true;
      published = schedule->published;
      break;
    }
    if (!schedule && !destination) { destination = &schedule; }
  }
  if (!publish && published) { throw Fault(context, timer, 40, "metadata arm cannot replace a published timer"); }
  if (!timer || timer % 4 || (dpc && dpc % 4)) { throw Fault(context, timer, 40, "invalid timer or DPC alignment"); }
  std::array<uint8_t, 40> bytes;
  image_.memory->Read(context, timer, bytes.data(), bytes.size());
  if ((bytes[0] != 8 && bytes[0] != 9) || bytes[2] != 10 || bytes[3] != uint8_t(published)) {
    throw Fault(context, timer, 40, "relative timer queue requires coherent initialized storage");
  }
  std::array<uint32_t, 2> waitList;
  image_.memory->Read(context, timer + 8, waitList.data(), sizeof(waitList));
  if (publish && (waitList[0] != timer + 8 || waitList[1] != timer + 8)) {
    throw Fault(context, timer, 40, "timer scheduling with guest waiters is unsupported");
  }
  if (dpc) {
    if (uint64_t(dpc) < uint64_t(timer) + 40 && uint64_t(timer) < uint64_t(dpc) + 28) {
      throw Fault(context, dpc, 28, "timer and deferred storage overlap");
    }
    std::array<uint32_t, 7> deferred;
    image_.memory->Read(context, dpc, deferred.data(), sizeof(deferred));
    if ((deferred[0] & 0xffffff) != 19 || !runtime_.Find(deferred[3])) {
      throw Fault(context, dpc, 28, "relative timer queue requires initialized unqueued DPC storage");
    }
  }
  const uint64_t deadline = runtime_.RelativeTimerDeadline(context, dueTime);
  if (!destination || order_ == UINT64_MAX) { throw Fault(context, timer, 40, "relative timer queue capacity exhausted"); }
  runtime_.Checkpoint(context);
  if (publish) {
    const std::array<uint8_t, 3> header = {0, 10, 1};
    const uint32_t zero = 0;
    image_.memory->Write(context, timer + 1, header.data(), header.size());
    image_.memory->Write(context, timer + 4, &zero, sizeof(zero));
    image_.memory->Write(context, timer + 16, &deadline, sizeof(deadline));
    image_.memory->Write(context, timer + 32, &dpc, sizeof(dpc));
    image_.memory->Write(context, timer + 36, &zero, sizeof(zero));
  }
  *destination = Schedule{{timer, dpc, deadline}, ++order_, publish};
  return inserted;
}

void TimerQueue::SetTimer(X86Context& context, uint8_t* base) {
  RequireWorker(context);
  if (base != image_.memory->Base()) { throw Fault(context, 149, 0, "timer scheduling requires its image backing"); }
  const uint64_t stackEnd = uint64_t(context.esp) + 20;
  if (stackEnd > UINT32_MAX) { throw Fault(context, context.esp, 20, "timer scheduling frame overflow"); }
  std::lock_guard lock(lock_);
  auto lease = image_.memory->Borrow();
  std::array<uint32_t, 5> frame;
  image_.memory->Read(context, context.esp, frame.data(), sizeof(frame));
  const int64_t dueTime = std::bit_cast<int64_t>((uint64_t(frame[3]) << 32) | frame[2]);
  const bool inserted = ArmLocked(context, frame[1], dueTime, frame[4], true);
  context.eax = uint32_t(inserted);
  context.esp = uint32_t(stackEnd);
}

std::optional<TimerExpiration> TimerQueue::TakeExpired(const X86Context& context) {
  RequireWorker(context);
  std::lock_guard lock(lock_);
  const uint64_t now = runtime_.TimerTime(context);
  auto* selected = static_cast<std::optional<Schedule>*>(nullptr);
  for (auto& schedule : schedules_) {
    if (schedule && schedule->expiration.deadline <= now && (!selected ||
        schedule->expiration.deadline < (*selected)->expiration.deadline ||
        (schedule->expiration.deadline == (*selected)->expiration.deadline && schedule->order < (*selected)->order))) {
      selected = &schedule;
    }
  }
  runtime_.Checkpoint(context);
  if (!selected) { return {}; }
  const auto expiration = (*selected)->expiration;
  if ((*selected)->published) {
    auto lease = image_.memory->Borrow();
    std::array<uint32_t, 10> fields;
    image_.memory->Read(context, expiration.timer, fields.data(), sizeof(fields));
    const uint64_t deadline = (uint64_t(fields[5]) << 32) | fields[4];
    if ((fields[0] != 0x010a0008 && fields[0] != 0x010a0009) || fields[1] ||
        fields[2] != expiration.timer + 8 || fields[3] != expiration.timer + 8 ||
        deadline != expiration.deadline || fields[8] != expiration.dpc || fields[9]) {
      throw Fault(context, expiration.timer, 40, "timer expiry requires coherent storage without guest waiters");
    }
    const uint8_t inserted = 0;
    const uint32_t signal = 1;
    image_.memory->Write(context, expiration.timer + 3, &inserted, sizeof(inserted));
    image_.memory->Write(context, expiration.timer + 4, &signal, sizeof(signal));
  }
  selected->reset();
  return expiration;
}

void TimerQueue::Drive(X86Context& context, uint8_t* base) {
  RequireWorker(context);
  if (base != image_.memory->Base()) { throw Fault(context, 149, 0, "expiry driver requires its image backing"); }
  if (tickInstalled_ || clockObserver_) {
    std::lock_guard lock(lock_);
    const uint64_t now = runtime_.TimerTime(context);
    runtime_.Checkpoint(context);
    if (tickInstalled_) {
      const uint32_t milliseconds = uint32_t(now / 10000);
      memory_->Write(context, 0xc0002000, &milliseconds, sizeof(milliseconds));
    }
    if (clockObserver_) { clockObserver_(context, now); }
  }
  if (!Pending()) { return; }
  uint8_t irql = 0;
  image_.memory->Read(context, context.fs_base + 0x24, &irql, 1);
  if (irql >= 2) { return; }
  std::unique_lock delivery(image_.dpcDeliveryLock, std::try_to_lock);
  if (!delivery.owns_lock()) { return; }
  const auto expiration = TakeExpired(context);
  if (expiration && expiration->dpc) {
    DeliverLockedTimerDpc(image_, runtime_, context, expiration->dpc, Runtime::SystemTime(), delivery);
  }
}

size_t TimerQueue::Pending() const {
  std::lock_guard lock(lock_);
  size_t count = 0;
  for (const auto& schedule : schedules_) { count += schedule.has_value(); }
  return count;
}

void TimerQueue::Initialize(X86Context& context, uint8_t* base) {
  RequireWorker(context);
  if (base != image_.memory->Base()) { throw Fault(context, 113, 0, "timer initialization requires its image backing"); }
  std::lock_guard lock(lock_);
  const uint64_t stackEnd = uint64_t(context.esp) + 12;
  if (stackEnd > UINT32_MAX) { throw Fault(context, context.esp, 12, "timer initialization frame overflow"); }
  auto lease = image_.memory->Borrow();
  std::array<uint32_t, 3> frame;
  image_.memory->Read(context, context.esp, frame.data(), sizeof(frame));
  const uint32_t address = frame[1];
  if (!address || address % 4 || frame[2] > 1) {
    throw Fault(context, address, 40, "invalid timer storage or unsupported timer type");
  }
  image_.memory->CheckRange(context, address, 40);
  for (const auto& schedule : schedules_) {
    if (schedule && schedule->expiration.timer == address) { throw Fault(context, address, 40, "cannot initialize an armed timer"); }
  }
  runtime_.Checkpoint(context);
  const uint8_t type = uint8_t(8 + frame[2]);
  const std::array<uint8_t, 2> header = {10, 0};
  const std::array<uint32_t, 7> fields = {0, address + 8, address + 8, 0, 0, 0, 0};
  const uint32_t period = 0;
  image_.memory->Write(context, address, &type, sizeof(type));
  image_.memory->Write(context, address + 2, header.data(), header.size());
  image_.memory->Write(context, address + 4, fields.data(), sizeof(fields));
  image_.memory->Write(context, address + 36, &period, sizeof(period));
  context.esp = uint32_t(stackEnd);
}

void InstallTimerExpiryDriver(Runtime& runtime, PreparedImage& image, const std::shared_ptr<TimerQueue>& queue) {
  if (!queue || &queue->runtime_ != &runtime || &queue->image_ != &image) {
    throw Fault({}, 149, 0, "expiry driver requires its session owned timer queue");
  }
  runtime.InstallExpiryDriver([queue](X86Context& context, uint8_t* base) { queue->Drive(context, base); });
  runtime.RegisterKernel(149, [queue](X86Context& context, uint8_t* base) { queue->SetTimer(context, base); });
  queue->expiryInstalled_ = true;
}

KernelDataExport InstallTimerTickCount(Runtime& runtime, PreparedImage& image, const std::shared_ptr<TimerQueue>& queue) {
  runtime.Checkpoint({});
  if (!queue || &queue->runtime_ != &runtime || &queue->image_ != &image || !image.memory ||
      image.memory.get() != queue->memory_ || image.importsBound || image.boundRuntime || runtime.ActiveWorkers() ||
      (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) || runtime.KernelTarget(149) ||
      runtime.KernelTarget(156) || Memory::HasBorrow()) {
    throw Fault({}, 156, 4, "tick count requires its unpublished stopped image and owned timer queue");
  }
  std::lock_guard lock(queue->lock_);
  if (queue->expiryInstalled_ || queue->tickInstalled_) {
    throw Fault({}, 156, 4, "tick count requires an unconfigured expiry driver");
  }
  for (const auto& diagnostic : image.diagnosticImports) {
    if (diagnostic.ordinal == 156) { throw Fault({}, 156, 4, "tick count conflicts with a diagnostic import"); }
  }
  constexpr uint32_t address = 0xc0002000;
  auto mutation = image.memory->Mutate();
  if (image.memory->QueryPages(address).mapped) { throw Fault({}, address, 4, "tick count requires fresh owned storage"); }
  image.memory->Map(address, 4096);
  try {
    image.memory->Commit(address, 4096);
    const uint32_t zero = 0;
    image.memory->Write({}, address, &zero, sizeof(zero));
    InstallTimerExpiryDriver(runtime, image, queue);
  } catch (...) {
    image.memory->Discard(address, 4096);
    throw;
  }
  queue->tickInstalled_ = true;
  return {156, address, 4};
}

void InstallTimerClockObserver(Runtime& runtime, PreparedImage& image, const std::shared_ptr<TimerQueue>& queue,
  std::function<void(const X86Context&, uint64_t)> observer) {
  runtime.Checkpoint({});
  if (!queue || !observer || &queue->runtime_ != &runtime || &queue->image_ != &image || !image.memory ||
      image.memory.get() != queue->memory_ || image.importsBound || image.boundRuntime || runtime.HasPublishedImage() ||
      runtime.ActiveWorkers() || (image.diagnosticRuntime && image.diagnosticRuntime != &runtime) || Memory::HasBorrow()) {
    throw Fault({}, 149, 0, "timer clock observer requires its unpublished stopped image and owned timer queue");
  }
  std::lock_guard lock(queue->lock_);
  if (!queue->expiryInstalled_ || queue->clockObserver_) {
    throw Fault({}, 149, 0, "timer clock observer requires one installed expiry driver and no prior observer");
  }
  queue->clockObserver_ = std::move(observer);
}

std::shared_ptr<TimerQueue> InstallTimerProvider(Runtime& runtime, PreparedImage& image) {
  auto queue = std::make_shared<TimerQueue>(runtime, image);
  runtime.RegisterKernel(113, [queue](X86Context& context, uint8_t* base) { queue->Initialize(context, base); });
  return queue;
}

}
