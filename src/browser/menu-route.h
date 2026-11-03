#pragma once
#include "browser/sdk-provider.h"
#include <string>
#include <string_view>

namespace browser {

class MenuMissionObservation final {
public:
  static constexpr size_t MaxCommandUnits = 1024;
  static constexpr size_t MaxFieldUnits = 128;
  enum class State : uint32_t { Unavailable, Malformed, ContextMismatch, Request };
  struct Command {
    bool valid = false;
    uint32_t menuPage = 0;
    uint32_t mode = 0;
    uint32_t difficulty = 0;
    std::u16string map;
    std::u16string profile;
  };
  struct Event {
    uint64_t sequence = 0;
    uint64_t workerId = 0;
    uint32_t target = 0;
    uint32_t returnAddress = 0;
    uint32_t thisPointer = 0;
    uint32_t commandAddress = 0;
    uint32_t difficultyOwner = 0;
    uint32_t menuOwner = 0;
    uint32_t fsBase = 0;
    State state = State::Unavailable;
    std::u16string command;
    Command request;
  };
  struct SnapshotData {
    std::array<Event, 8> events{};
    size_t count = 0;
    uint64_t total = 0;
  };
  MenuMissionObservation(Runtime& runtime, PreparedImage& image);
  bool BelongsTo(Runtime& runtime, PreparedImage& image) const;
  static Command Classify(std::u16string_view command);
  void Observe(const X86Context& ctx, uint8_t* base, uint32_t target, uint64_t workerId) noexcept;
  SnapshotData Snapshot() const;
private:
  Runtime& runtime_;
  PreparedImage& image_;
  Memory* memory_;
  mutable std::mutex observationLock_;
  SnapshotData evidence_;
  void Inspect(const X86Context& ctx, Event& event);
};

class MenuRoute final : public SdkGraphics {
public:
  struct SnapshotData {
    uint64_t rejectedAttempts = 0;
    uint32_t lastSelection = 0;
  };
  MenuRoute(Runtime& runtime, PreparedImage& image);
  bool BelongsTo(Runtime& runtime, PreparedImage& image) const override;
  bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) override;
  SnapshotData Snapshot() const;
private:
  Runtime& runtime_;
  PreparedImage& image_;
  Memory* memory_;
  mutable std::mutex lock_;
  SnapshotData snapshot_;
  uint32_t Call(X86Context& ctx, uint8_t* base, uint32_t target, uint32_t self, std::initializer_list<uint32_t> arguments);
};

}
