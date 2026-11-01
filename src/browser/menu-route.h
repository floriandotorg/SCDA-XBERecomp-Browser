#pragma once
#include "browser/sdk-provider.h"

namespace browser {

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
