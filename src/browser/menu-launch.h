#pragma once
#include "browser/sdk-provider.h"
#include <optional>

namespace browser {

class MenuLaunchRequested final : public std::exception {
public:
  std::string path;
  bool hasData = false;
  std::array<uint8_t, 3072> data{};
  std::optional<std::array<uint8_t, 4096>> page;
  const char* what() const noexcept override { return "menu launch requested"; }
};

class MenuLaunch final : public SdkGraphics {
public:
  MenuLaunch(Runtime& runtime, PreparedImage& image, bool originalLaunch = false);
  bool BelongsTo(Runtime& runtime, PreparedImage& image) const override;
  bool Dispatch(X86Context& ctx, uint8_t* base, uint32_t id) override;
  void InstallFirmware(uint32_t launchCell);
private:
  Runtime& runtime_;
  PreparedImage& image_;
  Memory* memory_;
  bool originalLaunch_;
};

}
