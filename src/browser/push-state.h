#pragma once
#include "gpu/state.h"
#include <cstddef>
#include <cstdint>
#include <span>

namespace browser {

enum class PushStateStatus : uint32_t {
  StateOnly,
  OutputWrite,
  UnsupportedControl,
  UnsupportedMethod,
  Malformed,
  LimitExceeded
};

struct PushStateLimits {
  size_t maxWords = 16384;
  size_t maxPackets = 4096;
};

struct PushStateResult {
  PushStateStatus status = PushStateStatus::StateOnly;
  size_t wordOffset = 0;
  uint32_t method = 0;
  size_t packets = 0;
  size_t values = 0;
};

PushStateResult ApplyPushState(gpu::State& state, std::span<const uint32_t> words, PushStateLimits limits = {});

}
