#pragma once
#include "cpu/x86_ops.h"
#include <span>

struct ArithmeticFixture {
  const char* operation;
  unsigned width;
  unsigned shift;
  uint32_t X86Context::* destination;
  int64_t count;
  X86Func* function;
};

std::span<const ArithmeticFixture> ArithmeticFixtures();
