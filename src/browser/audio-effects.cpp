#include "browser/audio-effects.h"

#include <algorithm>
#include <cstring>

namespace browser::audio {

namespace {

uint32_t ReadWord(std::span<const uint8_t> image, uint64_t offset) {
  if (offset + 4 > image.size()) {
    return 0;
  }
  uint32_t value = 0;
  std::memcpy(&value, image.data() + offset, 4);
  return value;
}

}

EffectsPlanResult PlanEffectsImage(std::span<const uint8_t> image, uint64_t budgetBytes) {
  const uint64_t table = kEffectsTableBase +
      (uint64_t(ReadWord(image, 0x804)) + uint64_t(ReadWord(image, 0x80C))) * 4;
  EffectsPlanResult result;
  EffectsImagePlan& plan = result.plan;
  plan.count = std::min(ReadWord(image, table), kEffectsMaxCount);
  plan.flags = ReadWord(image, table + 4);
  plan.descriptorBytes = 8 + uint64_t(std::max(plan.count, 1u)) * kEffectEntryBytes;
  plan.effects.resize(plan.count);
  for (uint32_t n = 0; n < plan.count; ++n) {
    const uint64_t entry = table + 8 + uint64_t(n) * kEffectEntryBytes;
    for (uint32_t k = 0; k < kEffectSegmentsPerEffect; ++k) {
      const uint32_t size = ReadWord(image, entry + uint64_t(k) * 8 + 4);
      EffectSegmentPlan& segment = plan.effects[n].segments[k];
      segment.declaredSize = size;
      segment.allocationSize = std::max<uint64_t>(size, kEffectSegmentMinimumBytes);
      plan.segmentBytes += segment.allocationSize;
    }
  }
  plan.totalBytes = plan.descriptorBytes + plan.segmentBytes;
  if (plan.totalBytes > budgetBytes) {
    result.error = EffectsPlanError::kBudgetExceeded;
  }
  return result;
}

}
