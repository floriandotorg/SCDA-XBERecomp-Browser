#pragma once
#include "browser/cpu_runtime.h"
#include <array>
#include <atomic>
#include <optional>

namespace browser {

inline constexpr size_t kDispatcherCapacity = 256;

struct DispatcherError : Fault {
  uint32_t status;
  DispatcherError(uint32_t value, uint32_t address, const char* reason)
    : Fault({}, address, 0, reason), status(value) {}
};

class Dispatcher {
  friend class DispatcherHandles;
  enum class Kind { NotificationEvent, SynchronizationEvent, Semaphore, Mutant, NotificationTimer, SynchronizationTimer, Thread };
  struct Object {
    uint32_t id;
    Kind kind;
    int32_t signal;
    int32_t limit;
    bool destroyed = false;
    uint32_t owner = 0;
    bool abandoned = false;
    std::optional<uint64_t> deadline;
    uint64_t period = 0;
    bool unreferenced = false;
  };
  Runtime& runtime_;
  mutable std::mutex lock_;
  std::array<std::shared_ptr<Object>, kDispatcherCapacity> objects_{};
  uint64_t nextId_ = 4;
  size_t waiters_ = 0;
  const std::optional<uint64_t> systemTimeOrigin_;
  uint64_t timerTime_ = 0;
  bool timerPaused_ = false;
  const bool runtimeClock_;
  const bool hostWatchdog_;
  std::atomic<bool> disconnected_ = false;
  void RefreshTime(const X86Context& ctx);
  bool ExpireTimer(Object& object);
  bool ArmTimer(Object& object, uint64_t deadline, uint64_t periodTicks);
  void RetireUnreferenced(uint32_t id);
  std::shared_ptr<Object>& Find(uint32_t id);
  uint32_t Create(Kind kind, int32_t signal, int32_t limit, uint32_t owner = 0);
public:
  enum class WaitResult { Signaled, TimedOut, Destroyed, Abandoned, WatchdogExpired };
  enum class WaitMode { Any, All };
  struct WaitOutcome {
    WaitResult result;
    std::optional<size_t> index;
  };
  struct TimerState {
    bool armed;
    bool signaled;
  };
  struct EventInformation {
    bool notification;
    int32_t signal;
  };
  struct SemaphoreInformation {
    int32_t count;
    int32_t limit;
  };
  struct MutantInformation {
    int32_t count;
    bool ownedByCaller;
    bool abandoned;
  };
private:
  uint64_t GuestDeadline(const X86Context& ctx, int64_t timeout) const;
  WaitOutcome WaitImpl(const X86Context& ctx, std::span<const uint32_t> ids, WaitMode mode, std::chrono::milliseconds hostTimeout, uint32_t owner, bool guestClock, std::optional<int64_t> guestTimeout);
public:
  explicit Dispatcher(Runtime& runtime, std::optional<uint64_t> systemTimeOrigin = std::nullopt, bool runtimeClock = false,
      bool hostWatchdog = true)
    : runtime_(runtime), systemTimeOrigin_(systemTimeOrigin), runtimeClock_(runtimeClock), hostWatchdog_(hostWatchdog) {}
  uint32_t CreateEvent(bool notification, bool signaled);
  uint32_t CreateThread();
  void CompleteThread(uint32_t id);
  uint32_t CreateSemaphore(int32_t initial, int32_t limit);
  uint32_t CreateMutant(uint32_t owner = 0);
  uint32_t CreateTimer(bool notification);
  bool SetTimer(uint32_t id, uint64_t delayTicks, uint64_t periodTicks = 0);
  bool SetTimerGuest(uint32_t id, int64_t dueTime, int32_t periodMilliseconds = 0);
  TimerState SetTimerGuestState(uint32_t id, int64_t dueTime, int32_t periodMilliseconds);
  bool CancelTimer(uint32_t id);
  TimerState CancelTimerState(uint32_t id);
  void AdvanceTime(uint64_t elapsedTicks);
  void SetTimerPaused(bool paused);
  uint64_t TimerTime() const;
  int32_t ReleaseMutant(uint32_t id, uint32_t owner);
  size_t AbandonOwner(uint32_t owner);
  int32_t SetEvent(uint32_t id);
  int32_t ResetEvent(uint32_t id);
  int32_t ReleaseSemaphore(uint32_t id, int32_t count);
  EventInformation QueryEvent(uint32_t id);
  SemaphoreInformation QuerySemaphore(uint32_t id);
  MutantInformation QueryMutant(uint32_t id, uint32_t owner);
  void Destroy(uint32_t id);
  void Disconnect() { disconnected_.store(true, std::memory_order_release); }
  bool Disconnected() const { return disconnected_.load(std::memory_order_acquire); }
  WaitResult Wait(const X86Context& ctx, uint32_t id, std::chrono::milliseconds timeout, uint32_t owner = 0);
  WaitOutcome WaitMultiple(const X86Context& ctx, std::span<const uint32_t> ids, WaitMode mode, std::chrono::milliseconds timeout, uint32_t owner = 0);
  WaitResult WaitGuest(const X86Context& ctx, uint32_t id, std::optional<int64_t> timeout, std::chrono::milliseconds watchdog, uint32_t owner = 0);
  WaitOutcome WaitMultipleGuest(const X86Context& ctx, std::span<const uint32_t> ids, WaitMode mode, std::optional<int64_t> timeout, std::chrono::milliseconds watchdog, uint32_t owner = 0);
  size_t ActiveObjects() const;
  size_t ActiveWaiters() const;
};

}
