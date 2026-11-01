#include "browser/shutdown-provider.h"
#include <algorithm>
#include <array>
#include <bit>

namespace browser {

ShutdownNotifications::ShutdownNotifications(Runtime& runtime, PreparedImage& image) : runtime_(runtime), image_(image) {
  notifications_.reserve(64);
}

std::vector<ShutdownNotification> ShutdownNotifications::Snapshot() const {
  std::lock_guard lock(lock_);
  return notifications_;
}

void ShutdownNotifications::Invoke(X86Context& context, uint8_t* base) {
  if (context.host != &runtime_ || base != image_.memory->Base() || !image_.importsBound ||
      image_.boundRuntime != &runtime_ || !runtime_.OwnsCurrentWorker()) {
    throw Fault(context, 47, 0, "shutdown registration requires its owned image worker");
  }
  if (Memory::HasBorrow()) { throw Fault(context, 47, 0, "shutdown registration during backing lease"); }
  const uint64_t stackEnd = uint64_t(context.esp) + 12;
  if (stackEnd > UINT32_MAX) { throw Fault(context, context.esp, 12, "shutdown registration frame overflow"); }
  auto lease = image_.memory->Borrow();
  std::array<uint32_t, 3> frame;
  image_.memory->Read(context, context.esp, frame.data(), sizeof(frame));
  const uint32_t address = frame[1];
  if (!address || address % 4) { throw Fault(context, address, 16, "invalid shutdown registration address"); }
  std::array<uint32_t, 4> record;
  image_.memory->Read(context, address, record.data(), sizeof(record));
  std::lock_guard lock(lock_);
  const auto found = std::find_if(notifications_.begin(), notifications_.end(), [address](const auto& notification) {
    return notification.address == address;
  });
  const bool registering = uint8_t(frame[2]) != 0;
  if (registering) {
    if (found != notifications_.end() || notifications_.size() == 64 || !runtime_.Find(record[0])) {
      throw Fault(context, address, 16, "duplicate, full or unsupported shutdown registration");
    }
    const ShutdownNotification notification{address, record[0], std::bit_cast<int32_t>(record[1])};
    const auto position = std::find_if(notifications_.begin(), notifications_.end(), [&](const auto& existing) {
      return existing.priority < notification.priority;
    });
    runtime_.Checkpoint(context);
    notifications_.insert(position, notification);
  } else {
    if (found == notifications_.end()) { throw Fault(context, address, 16, "shutdown registration is not registered"); }
    runtime_.Checkpoint(context);
    notifications_.erase(found);
  }
  context.esp = uint32_t(stackEnd);
}

std::shared_ptr<ShutdownNotifications> InstallShutdownProvider(Runtime& runtime, PreparedImage& image) {
  auto provider = std::make_shared<ShutdownNotifications>(runtime, image);
  runtime.RegisterKernel(47, [provider](X86Context& context, uint8_t* base) { provider->Invoke(context, base); });
  return provider;
}

}
