#pragma once
#include <cstdint>

struct DiagnosticService {
  const char* name;
  uint32_t address;
  const char* detail;
};
