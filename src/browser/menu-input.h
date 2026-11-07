#pragma once
#include "browser/input-core.h"
#include "browser/sdk-provider.h"
#include <functional>

namespace browser {

class MenuInput final : public SdkGraphics {
public:
  enum Button : uint32_t {
    Up = 1, Down = 2, Left = 4, Right = 8,
    Forward = 16, Backward = 32, StrafeLeft = 64, StrafeRight = 128,
    Start = 256, Confirm = 512, Back = 1024,
    X = 2048, Y = 4096, Black = 8192, White = 16384, LeftTrigger = 32768, RightTrigger = 65536,
    Select = 131072, LeftThumb = 262144, RightThumb = 524288
  };
  using SharedStorage = std::array<std::atomic<uint32_t>, 2>;
  using GamepadStorage = input_core::GamepadWords;
  MenuInput(Runtime& runtime, PreparedImage& image, std::shared_ptr<SharedStorage> storage = {}, std::shared_ptr<GamepadStorage> gamepad = {});
  bool BelongsTo(Runtime& runtime, PreparedImage& image) const override;
  bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) override;
  // a pad for each port in place of the shared mask and gamepad record (the input core); false: nothing plugged in
  using PadSource = std::function<bool(uint32_t port, std::array<uint8_t, 18>& sample)>;
  void SetPadSource(PadSource source) { source_ = std::move(source); }
  // XInputSetState's motors, the low and the high frequency one
  using RumbleSink = std::function<void(uint32_t port, uint16_t left, uint16_t right)>;
  void SetRumbleSink(RumbleSink sink) { rumble_ = std::move(sink); }
  std::atomic<uint32_t>* SharedAddress() { return shared_.data(); }
  uint32_t SharedWords() const { return uint32_t(shared_.size()); }
  std::atomic<uint32_t>* GamepadAddress() { return gamepad_->data(); }
  GamepadStorage& Gamepad() { return *gamepad_; }
  uint32_t GamepadWords() const { return uint32_t(gamepad_->size()); }
private:
  struct Handle {
    uint32_t token = 0;
    uint32_t port = 0;
    uint32_t packet = 0;
    bool sampled = false;
    std::array<uint8_t, 18> last{};
  };
  Runtime& runtime_;
  PreparedImage& image_;
  Memory* memory_;
  std::shared_ptr<SharedStorage> storage_;
  SharedStorage& shared_;
  std::shared_ptr<GamepadStorage> gamepad_;
  PadSource source_;
  RumbleSink rumble_;
  std::array<uint32_t, 7> SampleGamepad(const X86Context& ctx) const;
  std::array<uint8_t, 18> SampleMask(const X86Context& ctx, uint32_t output) const;
  std::mutex lock_;
  std::array<Handle, 32> handles_{};
  uint32_t nextToken_ = 0xbc000000;
  void Require(const X86Context& ctx, uint8_t* base) const;
  bool Overlaps(const X86Context& ctx, uint32_t left, size_t leftBytes, uint32_t right, size_t rightBytes) const;
  void CheckOutput(const X86Context& ctx, uint32_t address, size_t bytes, size_t frameBytes) const;
};

}
