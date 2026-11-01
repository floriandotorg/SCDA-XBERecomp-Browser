#pragma once
#include "browser/image.h"
#include <array>

namespace browser {

struct TimerExpiration {
  uint32_t timer;
  uint32_t dpc;
  uint64_t deadline;
};

class TimerQueue {
  friend void InstallTimerExpiryDriver(Runtime&, PreparedImage&, const std::shared_ptr<TimerQueue>&);
  friend KernelDataExport InstallTimerTickCount(Runtime&, PreparedImage&, const std::shared_ptr<TimerQueue>&);
  friend void InstallTimerClockObserver(Runtime&, PreparedImage&, const std::shared_ptr<TimerQueue>&,
    std::function<void(const X86Context&, uint64_t)>);
  struct Schedule {
    TimerExpiration expiration;
    uint64_t order;
    bool published;
  };
  Runtime& runtime_;
  PreparedImage& image_;
  Memory* const memory_;
  mutable std::mutex lock_;
  std::array<std::optional<Schedule>, 64> schedules_;
  uint64_t order_ = 0;
  bool expiryInstalled_ = false;
  bool tickInstalled_ = false;
  std::function<void(const X86Context&, uint64_t)> clockObserver_;
  void RequireWorker(const X86Context& context) const;
  bool ArmLocked(const X86Context& context, uint32_t timer, int64_t dueTime, uint32_t dpc, bool publish);
public:
  TimerQueue(Runtime& runtime, PreparedImage& image) : runtime_(runtime), image_(image), memory_(image.memory.get()) {}
  void Initialize(X86Context& context, uint8_t* base);
  void SetTimer(X86Context& context, uint8_t* base);
  bool Arm(const X86Context& context, uint32_t timer, int64_t dueTime, uint32_t dpc);
  std::optional<TimerExpiration> TakeExpired(const X86Context& context);
  size_t Pending() const;
  void Drive(X86Context& context, uint8_t* base);
};

std::shared_ptr<TimerQueue> InstallTimerProvider(Runtime& runtime, PreparedImage& image);
void InstallTimerExpiryDriver(Runtime& runtime, PreparedImage& image, const std::shared_ptr<TimerQueue>& queue);
KernelDataExport InstallTimerTickCount(Runtime& runtime, PreparedImage& image, const std::shared_ptr<TimerQueue>& queue);
void InstallTimerClockObserver(Runtime& runtime, PreparedImage& image, const std::shared_ptr<TimerQueue>& queue,
  std::function<void(const X86Context&, uint64_t)> observer);

}
