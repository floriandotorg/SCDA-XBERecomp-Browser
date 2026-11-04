#include "browser/dispatcher.h"
#include <algorithm>
#include <utility>

namespace browser {

std::shared_ptr<Dispatcher::Object>& Dispatcher::Find(uint32_t id) {
  for (auto& object : objects_) {
    if (object && object->id == id) { return object; }
  }
  throw DispatcherError(0xc0000008, id, "unknown dispatcher object");
}

uint32_t Dispatcher::Create(Kind kind, int32_t signal, int32_t limit, uint32_t owner) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  for (auto& object : objects_) {
    if (!object) {
      if (nextId_ > UINT32_MAX) { throw Fault({}, 0, 0, "dispatcher identifier budget exhausted"); }
      const auto id = uint32_t(nextId_);
      object = std::make_shared<Object>(Object{id, kind, signal, limit, false, owner});
      nextId_ += 4;
      return id;
    }
  }
  size_t mutants = 0, closedOwned = 0, threads = 0, timers = 0;
  for (const auto& object : objects_) {
    mutants += object->kind == Kind::Mutant;
    closedOwned += object->kind == Kind::Mutant && object->owner && object->unreferenced;
    threads += object->kind == Kind::Thread;
    timers += object->kind == Kind::NotificationTimer || object->kind == Kind::SynchronizationTimer;
  }
  const std::string reason = "dispatcher object capacity exhausted: mutants=" + std::to_string(mutants) +
    " closed-owned=" + std::to_string(closedOwned) + " threads=" + std::to_string(threads) + " timers=" + std::to_string(timers);
  throw Fault({}, 0, 0, reason.c_str());
}

uint32_t Dispatcher::CreateEvent(bool notification, bool signaled) {
  return Create(notification ? Kind::NotificationEvent : Kind::SynchronizationEvent, signaled ? 1 : 0, 1);
}

uint32_t Dispatcher::CreateThread() { return Create(Kind::Thread, 0, 1); }

void Dispatcher::CompleteThread(uint32_t id) {
  {
    std::lock_guard lock(lock_);
    auto& object = Find(id);
    if (object->kind != Kind::Thread || object->signal) { throw Fault({}, id, 0, "invalid or repeated dispatcher thread completion"); }
    object->signal = 1;
    if (object->unreferenced) { object->destroyed = true; object.reset(); }
  }
  if (!Disconnected()) { runtime_.Wake(); }
}

uint32_t Dispatcher::CreateSemaphore(int32_t initial, int32_t limit) {
  if (initial < 0 || limit <= 0 || initial > limit) { throw Fault({}, 0, 0, "invalid semaphore count or limit"); }
  return Create(Kind::Semaphore, initial, limit);
}

uint32_t Dispatcher::CreateMutant(uint32_t owner) {
  return Create(Kind::Mutant, owner ? 0 : 1, 65535, owner);
}

uint32_t Dispatcher::CreateTimer(bool notification) {
  return Create(notification ? Kind::NotificationTimer : Kind::SynchronizationTimer, 0, 1);
}

bool Dispatcher::ExpireTimer(Object& object) {
  if (!object.deadline || *object.deadline > timerTime_) { return false; }
  object.signal = 1;
  if (object.period == 0) {
    object.deadline.reset();
  } else {
    const uint64_t remaining = object.period - (timerTime_ - *object.deadline) % object.period;
    if (remaining > UINT64_MAX - timerTime_) { object.deadline.reset(); }
    else { object.deadline = timerTime_ + remaining; }
  }
  return true;
}

bool Dispatcher::SetTimer(uint32_t id, uint64_t delayTicks, uint64_t periodTicks) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  auto& object = *Find(id);
  if (object.kind != Kind::NotificationTimer && object.kind != Kind::SynchronizationTimer) {
    throw Fault({}, id, 0, "dispatcher object is not a timer");
  }
  if (delayTicks > UINT64_MAX - timerTime_) { throw Fault({}, id, 0, "timer deadline overflow"); }
  return ArmTimer(object, timerTime_ + delayTicks, periodTicks);
}

bool Dispatcher::SetTimerGuest(uint32_t id, int64_t dueTime, int32_t periodMilliseconds) {
  return SetTimerGuestState(id, dueTime, periodMilliseconds).armed;
}

Dispatcher::TimerState Dispatcher::SetTimerGuestState(uint32_t id, int64_t dueTime, int32_t periodMilliseconds) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  auto& object = *Find(id);
  if (object.kind != Kind::NotificationTimer && object.kind != Kind::SynchronizationTimer) {
    throw Fault({}, id, 0, "dispatcher object is not a timer");
  }
  if (periodMilliseconds < 0) { throw Fault({}, id, 0, "negative guest timer period"); }
  const auto deadline = GuestDeadline({}, dueTime);
  const TimerState previous{object.deadline.has_value(), object.signal != 0};
  ArmTimer(object, deadline, uint64_t(periodMilliseconds) * 10000);
  return previous;
}

bool Dispatcher::ArmTimer(Object& object, uint64_t deadline, uint64_t periodTicks) {
  const bool previous = object.deadline.has_value();
  object.signal = 0;
  object.deadline = deadline;
  object.period = periodTicks;
  ExpireTimer(object);
  runtime_.Wake();
  return previous;
}

bool Dispatcher::CancelTimer(uint32_t id) {
  return CancelTimerState(id).armed;
}

Dispatcher::TimerState Dispatcher::CancelTimerState(uint32_t id) {
  std::lock_guard lock(lock_);
  auto& object = *Find(id);
  if (object.kind != Kind::NotificationTimer && object.kind != Kind::SynchronizationTimer) {
    throw Fault({}, id, 0, "dispatcher object is not a timer");
  }
  const TimerState previous{object.deadline.has_value(), object.signal != 0};
  object.deadline.reset();
  object.period = 0;
  return previous;
}

void Dispatcher::RefreshTime(const X86Context& ctx) {
  if (!runtimeClock_) { return; }
  const auto sample = runtime_.TimerTime(ctx);
  if (sample < timerTime_) { throw Fault(ctx, 0, 0, "dispatcher clock regressed"); }
  timerTime_ = sample;
  bool expired = false;
  for (auto& object : objects_) {
    if (object && ExpireTimer(*object)) { expired = true; }
  }
  if (expired) { runtime_.Wake(); }
}

void Dispatcher::AdvanceTime(uint64_t elapsedTicks) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  if (timerPaused_) { return; }
  if (elapsedTicks > UINT64_MAX - timerTime_) { throw Fault({}, 0, 0, "timer clock overflow"); }
  timerTime_ += elapsedTicks;
  bool expired = false;
  for (auto& object : objects_) {
    if (object && ExpireTimer(*object)) { expired = true; }
  }
  if (expired || elapsedTicks > 0) { runtime_.Wake(); }
}

void Dispatcher::SetTimerPaused(bool paused) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  timerPaused_ = paused;
}

uint64_t Dispatcher::TimerTime() const {
  std::lock_guard lock(lock_);
  return timerTime_;
}

int32_t Dispatcher::ReleaseMutant(uint32_t id, uint32_t owner) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  auto& reference = Find(id);
  auto& object = *reference;
  if (object.kind != Kind::Mutant || owner == 0 || object.owner != owner) {
    throw DispatcherError(0xc0000046, id, "mutant release requires its nonzero owner");
  }
  const int32_t previous = object.signal;
  if (++object.signal == 1) {
    object.owner = 0;
    if (object.unreferenced) {
      object.destroyed = true;
      reference.reset();
    }
    runtime_.Wake();
  }
  return previous;
}

size_t Dispatcher::AbandonOwner(uint32_t owner) {
  if (owner == 0) { throw Fault({}, 0, 0, "mutant abandonment requires a nonzero owner"); }
  std::lock_guard lock(lock_);
  size_t count = 0;
  for (auto& object : objects_) {
    if (object && object->kind == Kind::Mutant && object->owner == owner) {
      object->owner = 0;
      object->signal = 1;
      object->abandoned = true;
      if (object->unreferenced) {
        object->destroyed = true;
        object.reset();
      }
      ++count;
    }
  }
  if (count > 0) { runtime_.Wake(); }
  return count;
}

int32_t Dispatcher::SetEvent(uint32_t id) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  auto& object = *Find(id);
  if (object.kind != Kind::NotificationEvent && object.kind != Kind::SynchronizationEvent) { throw Fault({}, id, 0, "dispatcher object is not an event"); }
  const int32_t previous = std::exchange(object.signal, 1);
  runtime_.Wake();
  return previous;
}

int32_t Dispatcher::ResetEvent(uint32_t id) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  auto& object = *Find(id);
  if (object.kind != Kind::NotificationEvent && object.kind != Kind::SynchronizationEvent) { throw Fault({}, id, 0, "dispatcher object is not an event"); }
  return std::exchange(object.signal, 0);
}

int32_t Dispatcher::ReleaseSemaphore(uint32_t id, int32_t count) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  auto& object = *Find(id);
  if (object.kind != Kind::Semaphore || count <= 0 || count > object.limit - object.signal) {
    throw Fault({}, id, 0, "invalid semaphore release");
  }
  const int32_t previous = object.signal;
  object.signal += count;
  runtime_.Wake();
  return previous;
}

Dispatcher::EventInformation Dispatcher::QueryEvent(uint32_t id) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  const auto& object = *Find(id);
  if (object.kind != Kind::NotificationEvent && object.kind != Kind::SynchronizationEvent) {
    throw Fault({}, id, 0, "dispatcher object is not an event");
  }
  return {object.kind == Kind::NotificationEvent, object.signal};
}

Dispatcher::SemaphoreInformation Dispatcher::QuerySemaphore(uint32_t id) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  const auto& object = *Find(id);
  if (object.kind != Kind::Semaphore) { throw Fault({}, id, 0, "dispatcher object is not a semaphore"); }
  return {object.signal, object.limit};
}

Dispatcher::MutantInformation Dispatcher::QueryMutant(uint32_t id, uint32_t owner) {
  std::lock_guard lock(lock_);
  runtime_.Checkpoint({});
  const auto& object = *Find(id);
  if (object.kind != Kind::Mutant) { throw Fault({}, id, 0, "dispatcher object is not a mutant"); }
  return {object.signal, owner != 0 && object.owner == owner, object.abandoned};
}

void Dispatcher::RetireUnreferenced(uint32_t id) {
  std::lock_guard lock(lock_);
  auto& object = Find(id);
  if (object->kind == Kind::Thread && !object->signal) {
    object->unreferenced = true;
    return;
  }
  object->destroyed = true;
  object.reset();
  if (!Disconnected()) { runtime_.Wake(); }
}

void Dispatcher::Destroy(uint32_t id) {
  std::lock_guard lock(lock_);
  auto& object = Find(id);
  object->destroyed = true;
  object.reset();
  if (!Disconnected()) { runtime_.Wake(); }
}

Dispatcher::WaitResult Dispatcher::Wait(const X86Context& ctx, uint32_t id, std::chrono::milliseconds timeout, uint32_t owner) {
  return WaitMultiple(ctx, std::span(&id, 1), WaitMode::Any, timeout, owner).result;
}

Dispatcher::WaitOutcome Dispatcher::WaitMultiple(const X86Context& ctx, std::span<const uint32_t> ids, WaitMode mode, std::chrono::milliseconds timeout, uint32_t owner) {
  return WaitImpl(ctx, ids, mode, timeout, owner, false, std::nullopt);
}

Dispatcher::WaitResult Dispatcher::WaitGuest(const X86Context& ctx, uint32_t id, std::optional<int64_t> timeout, std::chrono::milliseconds watchdog, uint32_t owner) {
  return WaitMultipleGuest(ctx, std::span(&id, 1), WaitMode::Any, timeout, watchdog, owner).result;
}

Dispatcher::WaitOutcome Dispatcher::WaitMultipleGuest(const X86Context& ctx, std::span<const uint32_t> ids, WaitMode mode, std::optional<int64_t> timeout, std::chrono::milliseconds watchdog, uint32_t owner) {
  return WaitImpl(ctx, ids, mode, watchdog, owner, true, timeout);
}

uint64_t Dispatcher::GuestDeadline(const X86Context& ctx, int64_t timeout) const {
  if (timeout == 0) { return timerTime_; }
  if (timeout > 0) {
    if (!systemTimeOrigin_) { throw Fault(ctx, 0, 0, "absolute guest timeout requires a system time origin"); }
    const uint64_t absolute = uint64_t(timeout);
    return absolute <= *systemTimeOrigin_ ? 0 : absolute - *systemTimeOrigin_;
  }
  const uint64_t delay = uint64_t(-(timeout + 1)) + 1;
  if (delay > UINT64_MAX - timerTime_) { throw Fault(ctx, 0, 0, "guest wait deadline overflow"); }
  return timerTime_ + delay;
}

Dispatcher::WaitOutcome Dispatcher::WaitImpl(const X86Context& ctx, std::span<const uint32_t> ids, WaitMode mode, std::chrono::milliseconds timeout, uint32_t owner, bool guestClock, std::optional<int64_t> guestTimeout) {
  if (Memory::HasBorrow() || timeout < std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(30) || (guestClock && timeout == std::chrono::milliseconds::zero())) {
    throw Fault(ctx, 0, 0, "dispatcher wait requires no backing lease and a bounded host timeout");
  }
  if (ids.empty() || ids.size() > 64 || (mode != WaitMode::Any && mode != WaitMode::All)) {
    throw Fault(ctx, 0, ids.size(), "invalid dispatcher wait set or mode");
  }
  auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<std::shared_ptr<Object>, 64> objects{};
  const size_t count = ids.size();
  std::optional<uint64_t> guestDeadline;
  bool pollClock = false;
  {
    std::lock_guard lock(lock_);
    runtime_.Checkpoint(ctx);
    RefreshTime(ctx);
    if (guestClock && guestTimeout) { guestDeadline = GuestDeadline(ctx, *guestTimeout); }
    pollClock = runtimeClock_ && guestDeadline.has_value();
    if (waiters_ >= runtime_.WorkerCapacity()) { throw Fault(ctx, 0, 0, "dispatcher waiter capacity exhausted"); }
    for (size_t n = 0; n < count; ++n) {
      objects[n] = Find(ids[n]);
      if (runtimeClock_ && (objects[n]->kind == Kind::NotificationTimer || objects[n]->kind == Kind::SynchronizationTimer)) { pollClock = true; }
      if (objects[n]->kind == Kind::Mutant && owner == 0) { throw Fault(ctx, ids[n], 0, "mutant wait requires a nonzero owner"); }
      for (size_t previous = 0; previous < n; ++previous) {
        if (objects[n] == objects[previous]) { throw Fault(ctx, ids[n], 0, "duplicate dispatcher wait object"); }
      }
    }
    ++waiters_;
  }
  struct PendingWait {
    Dispatcher& dispatcher;
    ~PendingWait() {
      std::lock_guard lock(dispatcher.lock_);
      --dispatcher.waiters_;
    }
  } pending{*this};
  for (;;) {
    uint64_t sequence;
    auto clockWait = std::chrono::milliseconds(16);
    {
      std::lock_guard lock(lock_);
      runtime_.Checkpoint(ctx);
      RefreshTime(ctx);
      size_t ready = 0;
      size_t selected = 0;
      for (size_t n = 0; n < count; ++n) {
        if (objects[n]->destroyed) { return {WaitResult::Destroyed, n}; }
        if (objects[n]->signal > 0 || (objects[n]->kind == Kind::Mutant && objects[n]->owner == owner)) {
          if (ready == 0) { selected = n; }
          ++ready;
        }
      }
      if (mode == WaitMode::All ? ready == count : ready > 0) {
        std::optional<size_t> abandoned;
        for (size_t n = 0; n < count; ++n) {
          if (mode == WaitMode::All || n == selected) {
            const auto& object = *objects[n];
            if (object.kind == Kind::Mutant && object.signal <= 1 - object.limit) {
              throw Fault(ctx, object.id, 0, "mutant recursion capacity exhausted");
            }
            if (object.abandoned && !abandoned) { abandoned = n; }
          }
        }
        for (size_t n = 0; n < count; ++n) {
          if (mode == WaitMode::All || n == selected) {
            auto& object = *objects[n];
            if (object.kind != Kind::NotificationEvent && object.kind != Kind::NotificationTimer && object.kind != Kind::Thread) { --object.signal; }
            if (object.kind == Kind::Mutant) {
              object.owner = owner;
              object.abandoned = false;
            }
          }
        }
        if (abandoned) { return {WaitResult::Abandoned, abandoned}; }
        return {WaitResult::Signaled, mode == WaitMode::Any ? std::optional<size_t>(selected) : std::nullopt};
      }
      if (guestDeadline && timerTime_ >= *guestDeadline) { return {WaitResult::TimedOut, std::nullopt}; }
      auto wake = guestDeadline;
      for (size_t n = 0; n < count; ++n) {
        if (objects[n]->deadline && (!wake || *objects[n]->deadline < *wake)) { wake = objects[n]->deadline; }
      }
      if (wake && *wake > timerTime_) {
        clockWait = std::chrono::milliseconds(std::clamp<uint64_t>((*wake - timerTime_ + 9999) / 10000, 1, uint64_t(clockWait.count())));
      }
      sequence = runtime_.Sequence();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      if (!guestClock || hostWatchdog_) { return {guestClock ? WaitResult::WatchdogExpired : WaitResult::TimedOut, std::nullopt}; }
      deadline = now + timeout;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    runtime_.Wait(ctx, sequence, pollClock ? std::min(remaining, clockWait) : remaining);
  }
}

size_t Dispatcher::ActiveObjects() const {
  std::lock_guard lock(lock_);
  return std::count_if(objects_.begin(), objects_.end(), [](const auto& object) { return bool(object); });
}

size_t Dispatcher::ActiveWaiters() const {
  std::lock_guard lock(lock_);
  return waiters_;
}

}
