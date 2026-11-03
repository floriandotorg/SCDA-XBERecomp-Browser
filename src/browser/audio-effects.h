#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace browser::audio {

inline constexpr uint32_t kEffectsMaxCount = 64;
inline constexpr uint32_t kEffectSegmentsPerEffect = 4;
inline constexpr uint32_t kEffectEntryBytes = 32;
inline constexpr uint32_t kEffectSegmentMinimumBytes = 0x40;
inline constexpr uint64_t kEffectsTableBase = 0x818;

enum class EffectsPlanError {
  kNone,
  kBudgetExceeded,
};

struct EffectSegmentPlan {
  uint32_t declaredSize = 0;
  uint64_t allocationSize = 0;
};

struct EffectPlan {
  EffectSegmentPlan segments[kEffectSegmentsPerEffect];
};

struct EffectsImagePlan {
  uint32_t count = 0;
  uint32_t flags = 0;
  uint64_t descriptorBytes = 0;
  uint64_t segmentBytes = 0;
  uint64_t totalBytes = 0;
  std::vector<EffectPlan> effects;
};

struct EffectsPlanResult {
  EffectsPlanError error = EffectsPlanError::kNone;
  EffectsImagePlan plan;
  bool Ok() const { return error == EffectsPlanError::kNone; }
};

EffectsPlanResult PlanEffectsImage(std::span<const uint8_t> image, uint64_t budgetBytes);

}
