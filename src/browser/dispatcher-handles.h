#pragma once
#include "browser/guest-wait.h"

namespace browser {

class DispatcherHandles {
  enum class Kind { Event, Semaphore, Timer, Mutant };
  struct Reference {
    Dispatcher& dispatcher;
    uint32_t id = 0;
    explicit Reference(Dispatcher& owner) : dispatcher(owner) {}
    ~Reference();
  };
  struct Slot {
    uint32_t handle = 0;
    std::shared_ptr<Reference> object;
  };
  Dispatcher& dispatcher_;
  mutable std::mutex lock_;
  std::array<Slot, kDispatcherCapacity> slots_{};
  uint64_t nextHandle_ = 0x1000;
  Slot& Available();
  Slot& Find(uint32_t handle);
  std::shared_ptr<Reference> Pin(uint32_t handle);
  uint32_t Publish(Slot& slot, std::shared_ptr<Reference> object);
  uint32_t Create(Kind kind, bool notification, int32_t initial, int32_t limit, uint32_t owner = 0);
public:
  explicit DispatcherHandles(Dispatcher& dispatcher) : dispatcher_(dispatcher) {}
  uint32_t CreateEvent(bool notification, bool signaled);
  uint32_t PublishThread(uint32_t handle);
  void RollbackThread(uint32_t handle, uint32_t token);
  uint32_t CreateSemaphore(int32_t initial, int32_t limit);
  uint32_t CreateTimer(bool notification);
  uint32_t CreateMutant(uint32_t owner = 0);
  int32_t ReleaseMutant(uint32_t handle, uint32_t owner);
  size_t AbandonOwner(uint32_t owner);
  bool SetTimer(uint32_t handle, uint64_t delayTicks, uint64_t periodTicks = 0);
  bool SetTimerGuest(Memory& memory, const X86Context& ctx, uint32_t handle,
    uint32_t dueTimeAddress, int32_t periodMilliseconds = 0);
  Dispatcher::TimerState SetTimerGuestState(uint32_t handle, int64_t dueTime, int32_t periodMilliseconds);
  bool CancelTimer(uint32_t handle);
  Dispatcher::TimerState CancelTimerState(uint32_t handle);
  uint32_t Duplicate(uint32_t handle, bool closeSource = false);
  void Close(uint32_t handle);
  bool Contains(uint32_t handle);
  void RollbackCreate(uint32_t handle);
  int32_t SetEvent(uint32_t handle);
  int32_t ResetEvent(uint32_t handle);
  int32_t ReleaseSemaphore(uint32_t handle, int32_t count);
  Dispatcher::EventInformation QueryEvent(uint32_t handle);
  Dispatcher::SemaphoreInformation QuerySemaphore(uint32_t handle);
  Dispatcher::MutantInformation QueryMutant(uint32_t handle, uint32_t owner);
  uint32_t WaitGuestStatus(Memory& memory, const X86Context& ctx,
    std::span<const uint32_t> handles, Dispatcher::WaitMode mode, uint32_t timeoutAddress,
    bool alertable, std::chrono::milliseconds watchdog);
  size_t ActiveHandles() const;
};

}
