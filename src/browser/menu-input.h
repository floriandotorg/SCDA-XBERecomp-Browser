#pragma once
#include "browser/sdk-provider.h"

namespace browser {

class MenuInput final : public SdkGraphics {
public:
  enum Button : uint32_t {
    Up = 1, Down = 2, Left = 4, Right = 8,
    Forward = 16, Backward = 32, StrafeLeft = 64, StrafeRight = 128,
    Start = 256, Confirm = 512, Back = 1024,
    X = 2048, Y = 4096, Black = 8192, White = 16384, LeftTrigger = 32768, RightTrigger = 65536
  };
  MenuInput(Runtime& runtime, PreparedImage& image);
  bool BelongsTo(Runtime& runtime, PreparedImage& image) const override;
  bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) override;
  std::atomic<uint32_t>* SharedAddress() { return shared_.data(); }
  uint32_t SharedWords() const { return uint32_t(shared_.size()); }
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
  std::array<std::atomic<uint32_t>, 2> shared_{};
  std::mutex lock_;
  std::array<Handle, 32> handles_{};
  uint32_t nextToken_ = 0xbc000000;
  void Require(const X86Context& ctx, uint8_t* base) const;
  bool Overlaps(const X86Context& ctx, uint32_t left, size_t leftBytes, uint32_t right, size_t rightBytes) const;
  void CheckOutput(const X86Context& ctx, uint32_t address, size_t bytes, size_t frameBytes) const;
};

}
