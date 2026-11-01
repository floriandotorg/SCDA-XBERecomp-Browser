#include "browser/dispatcher-handles.h"
#include <algorithm>
#include <bit>
#include <utility>

namespace browser {

DispatcherHandles::Reference::~Reference() {
  if (id) { dispatcher.RetireUnreferenced(id); }
}

DispatcherHandles::Slot& DispatcherHandles::Available() {
  if (nextHandle_ > UINT32_MAX) { throw Fault({}, 0, 0, "dispatcher handle identifier budget exhausted"); }
  for (auto& slot : slots_) {
    if (!slot.object) { return slot; }
  }
  throw Fault({}, 0, 0, "dispatcher handle capacity exhausted");
}

DispatcherHandles::Slot& DispatcherHandles::Find(uint32_t handle) {
  for (auto& slot : slots_) {
    if (slot.object && slot.handle == handle) { return slot; }
  }
  throw Fault({}, handle, 0, "unknown dispatcher handle");
}

std::shared_ptr<DispatcherHandles::Reference> DispatcherHandles::Pin(uint32_t handle) {
  std::lock_guard lock(lock_);
  dispatcher_.runtime_.Checkpoint({});
  return Find(handle).object;
}

uint32_t DispatcherHandles::Publish(Slot& slot, std::shared_ptr<Reference> object) {
  slot.handle = uint32_t(nextHandle_);
  slot.object = std::move(object);
  nextHandle_ += 4;
  return slot.handle;
}

uint32_t DispatcherHandles::Create(Kind kind, bool notification, int32_t initial, int32_t limit, uint32_t owner) {
  std::lock_guard lock(lock_);
  dispatcher_.runtime_.Checkpoint({});
  auto& slot = Available();
  auto object = std::make_shared<Reference>(dispatcher_);
  switch (kind) {
    case Kind::Event: object->id = dispatcher_.CreateEvent(notification, initial != 0); break;
    case Kind::Semaphore: object->id = dispatcher_.CreateSemaphore(initial, limit); break;
    case Kind::Timer: object->id = dispatcher_.CreateTimer(notification); break;
    case Kind::Mutant: object->id = dispatcher_.CreateMutant(owner); break;
  }
  return Publish(slot, std::move(object));
}

uint32_t DispatcherHandles::CreateEvent(bool notification, bool signaled) {
  return Create(Kind::Event, notification, signaled ? 1 : 0, 1);
}

uint32_t DispatcherHandles::CreateSemaphore(int32_t initial, int32_t limit) {
  return Create(Kind::Semaphore, false, initial, limit);
}

uint32_t DispatcherHandles::CreateTimer(bool notification) {
  return Create(Kind::Timer, notification, 0, 1);
}

uint32_t DispatcherHandles::CreateMutant(uint32_t owner) {
  return Create(Kind::Mutant, false, 0, 1, owner);
}

int32_t DispatcherHandles::ReleaseMutant(uint32_t handle, uint32_t owner) {
  const auto object = Pin(handle);
  return dispatcher_.ReleaseMutant(object->id, owner);
}

size_t DispatcherHandles::AbandonOwner(uint32_t owner) {
  return dispatcher_.AbandonOwner(owner);
}

bool DispatcherHandles::SetTimer(uint32_t handle, uint64_t delayTicks, uint64_t periodTicks) {
  const auto object = Pin(handle);
  return dispatcher_.SetTimer(object->id, delayTicks, periodTicks);
}

bool DispatcherHandles::SetTimerGuest(Memory& memory, const X86Context& ctx, uint32_t handle,
  uint32_t dueTimeAddress, int32_t periodMilliseconds) {
  if (Memory::HasBorrow() || dueTimeAddress == 0) {
    throw Fault(ctx, dueTimeAddress, 8, "guest timer requires no backing lease and a nonnull due time pointer");
  }
  const auto object = Pin(handle);
  std::array<uint8_t, 8> bytes{};
  memory.Read(ctx, dueTimeAddress, bytes.data(), bytes.size());
  uint64_t bits = 0;
  for (size_t n = 0; n < bytes.size(); ++n) { bits |= uint64_t(bytes[n]) << (n * 8); }
  return dispatcher_.SetTimerGuest(object->id, std::bit_cast<int64_t>(bits), periodMilliseconds);
}

Dispatcher::TimerState DispatcherHandles::SetTimerGuestState(uint32_t handle, int64_t dueTime, int32_t periodMilliseconds) {
  const auto object = Pin(handle);
  return dispatcher_.SetTimerGuestState(object->id, dueTime, periodMilliseconds);
}

bool DispatcherHandles::CancelTimer(uint32_t handle) {
  return CancelTimerState(handle).armed;
}

Dispatcher::TimerState DispatcherHandles::CancelTimerState(uint32_t handle) {
  const auto object = Pin(handle);
  return dispatcher_.CancelTimerState(object->id);
}

uint32_t DispatcherHandles::Duplicate(uint32_t handle, bool closeSource) {
  std::lock_guard lock(lock_);
  dispatcher_.runtime_.Checkpoint({});
  auto& source = Find(handle);
  const auto target = Publish(Available(), source.object);
  if (closeSource) { source.object.reset(); }
  return target;
}

void DispatcherHandles::Close(uint32_t handle) {
  std::shared_ptr<Reference> released;
  {
    std::lock_guard lock(lock_);
    released = std::move(Find(handle).object);
  }
}

int32_t DispatcherHandles::SetEvent(uint32_t handle) {
  const auto object = Pin(handle);
  return dispatcher_.SetEvent(object->id);
}

int32_t DispatcherHandles::ResetEvent(uint32_t handle) {
  const auto object = Pin(handle);
  return dispatcher_.ResetEvent(object->id);
}

int32_t DispatcherHandles::ReleaseSemaphore(uint32_t handle, int32_t count) {
  const auto object = Pin(handle);
  return dispatcher_.ReleaseSemaphore(object->id, count);
}

Dispatcher::EventInformation DispatcherHandles::QueryEvent(uint32_t handle) {
  const auto object = Pin(handle);
  return dispatcher_.QueryEvent(object->id);
}

Dispatcher::SemaphoreInformation DispatcherHandles::QuerySemaphore(uint32_t handle) {
  const auto object = Pin(handle);
  return dispatcher_.QuerySemaphore(object->id);
}

Dispatcher::MutantInformation DispatcherHandles::QueryMutant(uint32_t handle, uint32_t owner) {
  const auto object = Pin(handle);
  return dispatcher_.QueryMutant(object->id, owner);
}

uint32_t DispatcherHandles::WaitGuestStatus(Memory& memory, const X86Context& ctx,
  std::span<const uint32_t> handles, Dispatcher::WaitMode mode, uint32_t timeoutAddress,
  bool alertable, std::chrono::milliseconds watchdog) {
  if (Memory::HasBorrow() || handles.empty() || handles.size() > 64) {
    throw Fault(ctx, 0, handles.size(), "handle wait requires no backing lease and one through 64 handles");
  }
  std::array<std::shared_ptr<Reference>, 64> objects{};
  std::array<uint32_t, 64> ids{};
  const auto count = handles.size();
  {
    std::lock_guard lock(lock_);
    dispatcher_.runtime_.Checkpoint(ctx);
    for (size_t n = 0; n < count; ++n) {
      objects[n] = Find(handles[n]).object;
      ids[n] = objects[n]->id;
    }
  }
  return browser::WaitGuestStatus(dispatcher_, memory, ctx, std::span(ids.data(), count), mode, timeoutAddress, alertable, watchdog, ctx.thread_handle);
}

size_t DispatcherHandles::ActiveHandles() const {
  std::lock_guard lock(lock_);
  return std::count_if(slots_.begin(), slots_.end(), [](const auto& slot) { return bool(slot.object); });
}

}
