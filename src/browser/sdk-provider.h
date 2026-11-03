#pragma once
#include "browser/image.h"

namespace browser {

enum class SdkPolicy { Observer, UnavailableXNet };
enum class SdkBehavior : uint32_t { Original, Unavailable, Diagnostic };

class SdkGraphics {
public:
  virtual ~SdkGraphics() = default;
  virtual bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) = 0;
  virtual bool BelongsTo(Runtime& runtime, PreparedImage& image) const = 0;
};

class SdkTrace {
public:
  struct Event {
    uint64_t sequence = 0;
    uint64_t workerId = 0;
    uint32_t id = 0;
    uint32_t fsBase = 0;
    uint32_t threadHandle = 0;
    uint32_t esp = 0;
    uint32_t returnAddress = 0;
    uint32_t argumentCount = 0;
    std::array<uint32_t, 6> arguments{};
    std::array<uint32_t, 13> presentation{};
    SdkBehavior behavior = SdkBehavior::Original;
  };
  struct SnapshotData {
    std::array<Event, 64> events{};
    size_t count = 0;
    uint64_t total = 0;
  };
  SnapshotData Snapshot() const;
private:
  mutable std::mutex lock_;
  std::array<Event, 64> events_{};
  uint64_t total_ = 0;
  void Record(Event event);
  friend std::shared_ptr<const SdkTrace> InstallSdkProvider(Runtime&, PreparedImage&, SdkPolicy, std::shared_ptr<SdkGraphics>, std::shared_ptr<SdkGraphics>, std::shared_ptr<SdkGraphics>, std::shared_ptr<SdkGraphics>, std::shared_ptr<SdkGraphics>);
};

std::shared_ptr<const SdkTrace> InstallSdkProvider(Runtime& runtime, PreparedImage& image, SdkPolicy policy = SdkPolicy::Observer,
  std::shared_ptr<SdkGraphics> graphics = {}, std::shared_ptr<SdkGraphics> input = {}, std::shared_ptr<SdkGraphics> launch = {}, std::shared_ptr<SdkGraphics> route = {},
  std::shared_ptr<SdkGraphics> audio = {});

}
