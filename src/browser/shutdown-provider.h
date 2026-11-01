#pragma once
#include "browser/image.h"

namespace browser {

struct ShutdownNotification {
  uint32_t address;
  uint32_t routine;
  int32_t priority;
};

class ShutdownNotifications {
  Runtime& runtime_;
  PreparedImage& image_;
  mutable std::mutex lock_;
  std::vector<ShutdownNotification> notifications_;
  void Invoke(X86Context& context, uint8_t* base);
  friend std::shared_ptr<ShutdownNotifications> InstallShutdownProvider(Runtime&, PreparedImage&);
public:
  ShutdownNotifications(Runtime& runtime, PreparedImage& image);
  std::vector<ShutdownNotification> Snapshot() const;
};

std::shared_ptr<ShutdownNotifications> InstallShutdownProvider(Runtime& runtime, PreparedImage& image);

}
