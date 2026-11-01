#pragma once
#include "browser/sdk-provider.h"

namespace browser {

class MenuLaunchRequested final : public std::exception {
public:
  std::string path;
  bool hasData = false;
  std::array<uint8_t, 3072> data{};
  const char* what() const noexcept override { return "menu launch requested"; }
};

class MenuLaunch final : public SdkGraphics {
public:
  MenuLaunch(Runtime& runtime, PreparedImage& image);
  bool BelongsTo(Runtime& runtime, PreparedImage& image) const override;
  bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) override;
private:
  Runtime& runtime_;
  PreparedImage& image_;
  Memory* memory_;
};

}
