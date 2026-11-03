#include "browser/audio-effects.h"

#include <cstdio>
#include <vector>

using namespace browser::audio;

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    ++checks;                                                         \
    if (!(cond)) {                                                    \
      ++failures;                                                     \
      std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);      \
    }                                                                 \
  } while (0)

static void Put32(std::vector<uint8_t>& image, uint64_t offset, uint32_t value) {
  for (uint64_t n = 0; n < 4; ++n) {
    image[offset + n] = uint8_t(value >> (n * 8));
  }
}

static std::vector<uint8_t> KnownImage() {
  std::vector<uint8_t> image(0x1000, 0);
  Put32(image, 0x804, 2);
  Put32(image, 0x80C, 3);
  const uint64_t table = 0x818 + uint64_t(2 + 3) * 4;
  Put32(image, table, 2);
  Put32(image, table + 4, 0xDEADBEEF);
  const uint32_t sizes[2][4] = {{0x10, 0x40, 0x100, 0}, {0x1000, 1, 0x3F, 0x41}};
  for (uint32_t i = 0; i < 2; ++i) {
    for (uint32_t k = 0; k < 4; ++k) {
      Put32(image, table + 8 + i * 32 + k * 8 + 4, sizes[i][k]);
    }
  }
  return image;
}

static void EmptyAndTruncated() {
  const EffectsPlanResult empty = PlanEffectsImage({}, UINT64_MAX);
  CHECK(empty.Ok());
  CHECK(empty.plan.count == 0);
  CHECK(empty.plan.flags == 0);
  CHECK(empty.plan.effects.empty());
  CHECK(empty.plan.descriptorBytes == 40);
  CHECK(empty.plan.segmentBytes == 0);
  CHECK(empty.plan.totalBytes == 40);

  const EffectsPlanResult noBudget = PlanEffectsImage({}, 39);
  CHECK(noBudget.error == EffectsPlanError::kBudgetExceeded);
  CHECK(!noBudget.Ok());
  CHECK(noBudget.plan.totalBytes == 40);

  std::vector<uint8_t> truncated(0x808, 0);
  Put32(truncated, 0x804, 7);
  const EffectsPlanResult shortTable = PlanEffectsImage(truncated, UINT64_MAX);
  CHECK(shortTable.Ok());
  CHECK(shortTable.plan.count == 0);
  CHECK(shortTable.plan.flags == 0);
  CHECK(shortTable.plan.totalBytes == 40);

  std::vector<uint8_t> partial(0x82E, 0);
  Put32(partial, 0x804, 2);
  Put32(partial, 0x80C, 3);
  partial[0x82C] = 5;
  partial[0x82D] = 0;
  const EffectsPlanResult partialCount = PlanEffectsImage(partial, UINT64_MAX);
  CHECK(partialCount.Ok());
  CHECK(partialCount.plan.count == 0);
  CHECK(partialCount.plan.flags == 0);
}

static void KnownBytes() {
  const std::vector<uint8_t> image = KnownImage();
  const EffectsPlanResult result = PlanEffectsImage(image, 4809);
  CHECK(result.Ok());
  CHECK(result.plan.count == 2);
  CHECK(result.plan.flags == 0xDEADBEEF);
  CHECK(result.plan.effects.size() == 2);
  CHECK(result.plan.descriptorBytes == 72);
  const uint32_t declared[2][4] = {{0x10, 0x40, 0x100, 0}, {0x1000, 1, 0x3F, 0x41}};
  const uint64_t allocation[2][4] = {{0x40, 0x40, 0x100, 0x40}, {0x1000, 0x40, 0x40, 0x41}};
  uint64_t segments = 0;
  for (uint32_t i = 0; i < 2; ++i) {
    for (uint32_t k = 0; k < 4; ++k) {
      CHECK(result.plan.effects[i].segments[k].declaredSize == declared[i][k]);
      CHECK(result.plan.effects[i].segments[k].allocationSize == allocation[i][k]);
      segments += allocation[i][k];
    }
  }
  CHECK(result.plan.segmentBytes == segments);
  CHECK(result.plan.totalBytes == 72 + segments);
  CHECK(result.plan.totalBytes == 4809);

  const EffectsPlanResult tight = PlanEffectsImage(image, 4808);
  CHECK(tight.error == EffectsPlanError::kBudgetExceeded);
  CHECK(tight.plan.totalBytes == 4809);
}

static void CountLimit() {
  std::vector<uint8_t> image = KnownImage();
  Put32(image, 0x818 + uint64_t(2 + 3) * 4, 100);
  const EffectsPlanResult result = PlanEffectsImage(image, UINT64_MAX);
  CHECK(result.Ok());
  CHECK(result.plan.count == 64);
  CHECK(result.plan.effects.size() == 64);
  CHECK(result.plan.descriptorBytes == 8 + uint64_t(64) * 32);
  for (uint32_t n = 0; n < 64; ++n) {
    for (uint32_t k = 0; k < 4; ++k) {
      CHECK(result.plan.effects[n].segments[k].allocationSize >= 0x40);
    }
  }
  CHECK(result.plan.totalBytes == result.plan.descriptorBytes + result.plan.segmentBytes);
}

static void OffsetOverflow() {
  std::vector<uint8_t> image(0x1000, 0);
  Put32(image, 0x804, 0xFFFFFFFF);
  Put32(image, 0x80C, 0xFFFFFFFF);
  const EffectsPlanResult wrapped = PlanEffectsImage(image, UINT64_MAX);
  CHECK(wrapped.Ok());
  CHECK(wrapped.plan.count == 0);
  CHECK(wrapped.plan.flags == 0);
  CHECK(wrapped.plan.totalBytes == 40);

  Put32(image, 0x804, 0x40000000);
  Put32(image, 0x80C, 0);
  const EffectsPlanResult far = PlanEffectsImage(image, UINT64_MAX);
  CHECK(far.Ok());
  CHECK(far.plan.count == 0);
  CHECK(far.plan.flags == 0);
}

static void SizeOverflow() {
  std::vector<uint8_t> image(0x1000, 0);
  Put32(image, 0x804, 0);
  Put32(image, 0x80C, 0);
  const uint64_t table = 0x818;
  Put32(image, table, 1);
  Put32(image, table + 4, 0x12345678);
  for (uint32_t k = 0; k < 4; ++k) {
    Put32(image, table + 8 + k * 8 + 4, 0xFFFFFFFF);
  }
  const uint64_t expectedTotal = 40 + uint64_t(4) * 0xFFFFFFFFu;
  CHECK(expectedTotal == 0x400000024ull);
  const EffectsPlanResult result = PlanEffectsImage(image, UINT64_MAX);
  CHECK(result.Ok());
  CHECK(result.plan.count == 1);
  CHECK(result.plan.flags == 0x12345678);
  for (uint32_t k = 0; k < 4; ++k) {
    CHECK(result.plan.effects[0].segments[k].declaredSize == 0xFFFFFFFF);
    CHECK(result.plan.effects[0].segments[k].allocationSize == 0xFFFFFFFFull);
  }
  CHECK(result.plan.totalBytes == expectedTotal);
  CHECK(result.plan.totalBytes > 0xFFFFFFFFull);

  const EffectsPlanResult unaffordable = PlanEffectsImage(image, expectedTotal - 1);
  CHECK(unaffordable.error == EffectsPlanError::kBudgetExceeded);
  const EffectsPlanResult exact = PlanEffectsImage(image, expectedTotal);
  CHECK(exact.Ok());
}

static void NoWrapAlias() {
  std::vector<uint8_t> image(0x1000, 0);
  Put32(image, 0x804, 0xFFFFFFFF);
  Put32(image, 0x80C, 2);
  Put32(image, 0x81C, 7);
  Put32(image, 0x820, 0xCAFEBABE);
  const EffectsPlanResult aliased = PlanEffectsImage(image, UINT64_MAX);
  CHECK(aliased.Ok());
  CHECK(aliased.plan.count == 0);
  CHECK(aliased.plan.flags == 0);
  CHECK(aliased.plan.effects.empty());
  CHECK(aliased.plan.totalBytes == 40);

  std::vector<uint8_t> exact(0x1000, 0);
  Put32(exact, 0x804, 0x80000000);
  Put32(exact, 0x80C, 0x80000000);
  Put32(exact, 0x818, 9);
  Put32(exact, 0x81C, 0x11111111);
  const EffectsPlanResult wrappedZero = PlanEffectsImage(exact, UINT64_MAX);
  CHECK(wrappedZero.Ok());
  CHECK(wrappedZero.plan.count == 0);
  CHECK(wrappedZero.plan.flags == 0);
  CHECK(wrappedZero.plan.totalBytes == 40);
}

static void HighBitCount() {
  std::vector<uint8_t> image(0x1000, 0);
  Put32(image, 0x804, 0);
  Put32(image, 0x80C, 0);
  Put32(image, 0x818, 0x80000005);
  Put32(image, 0x81C, 0xABCDEF01);
  const EffectsPlanResult high = PlanEffectsImage(image, UINT64_MAX);
  CHECK(high.Ok());
  CHECK(high.plan.count == 64);
  CHECK(high.plan.flags == 0xABCDEF01);
  CHECK(high.plan.effects.size() == 64);
  CHECK(high.plan.descriptorBytes == 8 + uint64_t(64) * 32);
  CHECK(high.plan.totalBytes == high.plan.descriptorBytes + high.plan.segmentBytes);

  Put32(image, 0x818, 0xFFFFFFFF);
  const EffectsPlanResult maximum = PlanEffectsImage(image, UINT64_MAX);
  CHECK(maximum.Ok());
  CHECK(maximum.plan.count == 64);
  CHECK(maximum.plan.effects.size() == 64);
  CHECK(maximum.plan.descriptorBytes == 2056);
}

int main() {
  EmptyAndTruncated();
  KnownBytes();
  CountLimit();
  OffsetOverflow();
  SizeOverflow();
  NoWrapAlias();
  HighBitCount();
  if (failures != 0) {
    std::printf("audio-effects-contracts-failed checks=%d failures=%d\n", checks, failures);
    return 1;
  }
  std::printf("audio-effects-contracts-ok checks=%d maxcount=%u segments=%u\n", checks, kEffectsMaxCount, kEffectSegmentsPerEffect);
  return 0;
}
