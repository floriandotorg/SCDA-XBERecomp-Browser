#include "browser/push-state.h"
#include <bit>
#include <memory>
#include <vector>

namespace browser {
namespace {

const auto inRange = [](uint32_t method, uint32_t first, uint32_t last) {
  return method >= first && method <= last;
};

const auto writesOutput = [](uint32_t method) {
  return inRange(method, 0x1500, 0x1510) || inRange(method, 0x1518, 0x1524) ||
    method == 0x17fc || method == 0x1800 || method == 0x1808 || method == 0x1810 || method == 0x1818 ||
    method == 0x17c8 || method == 0x17d0 || method == 0x1d70 || method == 0x1d94 || method == 0x1e90;
};

const auto rawStateMethod = [](uint32_t method) {
  return inRange(method, 0x200, 0x214) || inRange(method, 0x260, 0x27c) ||
    inRange(method, 0x288, 0x2a8) || method == 0x2b4 || inRange(method, 0x2c0, 0x2fc) ||
    inRange(method, 0x300, 0x37c) || inRange(method, 0x384, 0x3a4) || inRange(method, 0x3c0, 0x3fc) ||
    inRange(method, 0xa20, 0xa2c) || inRange(method, 0xa60, 0xadc) || inRange(method, 0xae0, 0xafc) ||
    method == 0x17f8 || method == 0x194c || method == 0x1950 || method == 0x195c || method == 0x1960 ||
    method == 0x1d78 || method == 0x1d7c || method == 0x1d8c || method == 0x1d90 ||
    method == 0x1d98 || method == 0x1d9c || inRange(method, 0x1e20, 0x1e24) ||
    inRange(method, 0x1e40, 0x1e60) || method == 0x1e6c || inRange(method, 0x1e70, 0x1e78);
};

struct Journal {
  std::vector<std::pair<uint32_t*, uint32_t>> writes;
  void Set(uint32_t& target, uint32_t value) {
    writes.emplace_back(&target, target);
    target = value;
  }
  void Set(float& target, uint32_t value) { Set(*reinterpret_cast<uint32_t*>(&target), value); }
  void Undo() {
    for (auto write = writes.rbegin(); write != writes.rend(); ++write) { *write->first = write->second; }
    writes.clear();
  }
};

PushStateStatus applyMethod(gpu::State& state, Journal& journal, uint32_t method, uint32_t value) {
  if (inRange(method, 0xb00, 0xb7c) || inRange(method, 0xb80, 0xbfc)) {
    const bool constant = method >= 0xb80;
    auto& cursor = state.simple[(constant ? 0x1ea4 : 0x1e9c) / 4];
    const uint32_t lane = (method / 4) % 4;
    if (cursor >= (constant ? 192u : 136u)) { return PushStateStatus::Malformed; }
    if (constant) { journal.Set(state.vsc[cursor][lane], value); }
    journal.Set(state.simple[method / 4], value);
    if (lane == 3) { journal.Set(cursor, cursor + 1); }
    return PushStateStatus::StateOnly;
  }
  if (writesOutput(method)) { return PushStateStatus::OutputWrite; }
  if (method == 0x100 || method == 0x110 || method == 0x1710) {
    return value == 0 ? PushStateStatus::StateOnly : PushStateStatus::UnsupportedControl;
  }
  if (method == 0x1d84) {
    if (value > 3) { return PushStateStatus::UnsupportedMethod; }
    journal.Set(state.simple[method / 4], value);
    return PushStateStatus::StateOnly;
  }
  if (method == 0x1ea4 || method == 0x1e9c || method == 0x1ea0) {
    const uint32_t bound = method == 0x1ea4 ? 192 : 136;
    if (value >= bound) { return PushStateStatus::Malformed; }
    journal.Set(state.simple[method / 4], value);
    return PushStateStatus::StateOnly;
  }
  if (method == 0x1e94 || method == 0x1e98) {
    if ((method == 0x1e94 && value != 4 && value != 6) || (method == 0x1e98 && value != 0)) {
      return PushStateStatus::UnsupportedMethod;
    }
    journal.Set(state.simple[method / 4], value);
    return PushStateStatus::StateOnly;
  }
  if (inRange(method, 0x1b00, 0x1bfc)) {
    const uint32_t stage = (method - 0x1b00) / 64;
    const uint32_t offset = (method - 0x1b00) % 64;
    if (offset == 0x0c) {
      if (value != 0) { return PushStateStatus::UnsupportedMethod; }
    } else if (offset == 0x24) {
      journal.Set(state.tss[stage][gpu::kTssBorderColor], value);
    } else if (inRange(offset, 0x28, 0x3c)) {
      journal.Set(state.tss[stage][gpu::kTssBumpEnvMat00 + (offset - 0x28) / 4], value);
    } else {
      return PushStateStatus::UnsupportedMethod;
    }
    journal.Set(state.simple[method / 4], value);
    return PushStateStatus::StateOnly;
  }
  if (rawStateMethod(method)) {
    if (inRange(method, 0xae0, 0xaec)) {
      journal.Set(state.tss[(method - 0xae0) / 4][gpu::kTssColorKeyColor], value);
    }
    journal.Set(state.simple[method / 4], value);
    return PushStateStatus::StateOnly;
  }
  return PushStateStatus::UnsupportedMethod;
}

}

PushStateResult ApplyPushState(gpu::State& state, std::span<const uint32_t> words, PushStateLimits limits) {
  PushStateResult result;
  if (words.size() > limits.maxWords) {
    result.status = PushStateStatus::LimitExceeded;
    return result;
  }
  static thread_local Journal journal;
  journal.writes.clear();
  struct Rollback {
    bool committed = false;
    ~Rollback() { if (!committed) { journal.Undo(); } }
  } rollback;
  auto* staged = &state;
  bool changedExecutionMode = false;
  size_t executionOffset = 0;
  while (result.wordOffset < words.size()) {
    const size_t start = result.wordOffset;
    const uint32_t header = words[start];
    const uint32_t type = header & 0xe0030003;
    if ((header & 0xe0000003) == 0x20000000 || (header & 3) == 1 || (header & 3) == 2 || header == 0x20000) {
      result.status = PushStateStatus::UnsupportedControl;
      return result;
    }
    if (type != 0 && type != 0x40000000) {
      result.status = PushStateStatus::Malformed;
      return result;
    }
    const uint32_t count = (header >> 18) & 0x7ff;
    const uint32_t method = header & 0x1ffc;
    result.method = method;
    if (((header >> 13) & 7) != 0 || method == 0) {
      result.status = PushStateStatus::UnsupportedControl;
      return result;
    }
    if (!count || count > words.size() - start - 1 || (type == 0 && count - 1 > (0x1ffc - method) / 4)) {
      result.status = PushStateStatus::Malformed;
      return result;
    }
    if (result.packets >= limits.maxPackets) {
      result.status = PushStateStatus::LimitExceeded;
      return result;
    }
    for (uint32_t n = 0; n < count; ++n) {
      const uint32_t current = method + (type == 0 ? n * 4 : 0);
      result.method = current;
      const auto status = applyMethod(*staged, journal, current, words[start + 1 + n]);
      if (status != PushStateStatus::StateOnly) {
        result.status = status;
        return result;
      }
      if (current == 0x1e94) {
        changedExecutionMode = true;
        executionOffset = start;
      }
      ++result.values;
    }
    ++result.packets;
    result.wordOffset += count + 1;
  }
  if (changedExecutionMode && staged->simple[0x1e94 / 4] != 4) {
    result.status = PushStateStatus::UnsupportedMethod;
    result.wordOffset = executionOffset;
    result.method = 0x1e94;
    return result;
  }
  rollback.committed = true;
  result.method = 0;
  return result;
}

}
