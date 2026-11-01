#include "browser/push-state.h"
#include <bit>
#include <cstdio>
#include <stdexcept>
#include <vector>

const auto check = [](bool valid, const char* name) {
  if (!valid) { throw std::runtime_error(name); }
};
const auto packet = [](std::vector<uint32_t>& words, uint32_t method, std::initializer_list<uint32_t> values, bool repeat = false) {
  words.push_back((uint32_t(values.size()) << 18) | method | (repeat ? 0x40000000u : 0));
  words.insert(words.end(), values);
};

int main(int argc, char** argv) {
  try {
    gpu::State state;
    state.simple[0] = 123;
    state.rs[gpu::kRsZEnable] = 1;
    state.vsc[191] = {9, 8, 7, 6};
    state.rt.header = 0x1234;
    if (argc == 2) {
      auto file = std::fopen(argv[1], "rb");
      check(file != nullptr, "capture input readable");
      std::vector<uint32_t> words;
      uint32_t word;
      while (std::fread(&word, sizeof(word), 1, file) == 1) { words.push_back(word); }
      std::fclose(file);
      const auto result = browser::ApplyPushState(state, words);
      std::printf("status=%u offset=%zu method=%04x packets=%zu values=%zu words=%zu blend=%x depth=%x color=%x\n", unsigned(result.status), result.wordOffset, result.method, result.packets, result.values, words.size(), state.simple[gpu::kMBlendOp / 4], state.simple[gpu::kMZFunc / 4], state.simple[gpu::kMColorWriteEnable / 4]);
      return result.status == browser::PushStateStatus::StateOnly ? 0 : 2;
    }
    std::vector<uint32_t> words;
    packet(words, gpu::kMAlphaTestEnable, {1, 0, 1, 1, 0});
    packet(words, gpu::kMStencilRef, {3, 4, 5}, true);
    packet(words, gpu::kMZFunc, {0x203, 0x1010101, 1});
    packet(words, 0x1ea4, {190});
    packet(words, 0xb80, {0x3f800000, 0xc0000000, 0x7fc01234, 0, 0x40800000, 0x40a00000, 0x40c00000, 0x40e00000});
    packet(words, 0x1b68, {0x3f800000, 2, 3, 4, 5, 6});
    packet(words, 0x1e9c, {0});
    packet(words, 0x1e94, {6, 0});
    packet(words, 0xb00, {1, 2, 3, 4, 5, 6, 7, 8});
    packet(words, 0x1e94, {4});
    packet(words, 0x100, {0});
    packet(words, 0x110, {0});
    packet(words, 0x1710, {0});
    const auto result = browser::ApplyPushState(state, words);
    check(result.status == browser::PushStateStatus::StateOnly, "recognized state stream accepted");
    check(result.packets == 13 && result.values == words.size() - 13 && result.wordOffset == words.size(), "exact packet and value counts");
    check(state.simple[gpu::kMAlphaTestEnable / 4] == 1 && state.simple[gpu::kMBlendEnable / 4] == 0, "incrementing methods");
    check(state.simple[gpu::kMStencilRef / 4] == 5 && state.simple[gpu::kMStencilMask / 4] == 0, "nonincrementing methods");
    check(state.simple[gpu::kMZFunc / 4] == 0x203 && state.simple[gpu::kMColorWriteEnable / 4] == 0x1010101, "known depth and color registers");
    check(state.vsc[190][0] == 1 && state.vsc[190][1] == -2 && std::bit_cast<uint32_t>(state.vsc[190][2]) == 0x7fc01234, "constant hardware index and float bits");
    check(state.vsc[191] == std::array<float, 4>{4, 5, 6, 7}, "constant upload increments registers");
    check(state.tss[1][gpu::kTssBumpEnvMat00] == 0x3f800000 && state.tss[1][gpu::kTssBumpEnvLOffset] == 6, "bump texture stage mapping");
    check(state.rt.header == 0x1234 && state.simple[0] == 123 && state.rs[gpu::kRsZEnable] == 1, "API state and surface identity preserved");
    const auto before = state;
    const auto reject = [&](std::vector<uint32_t> tail, browser::PushStateStatus status, const char* name) {
      std::vector<uint32_t> bad;
      packet(bad, gpu::kMBlendEnable, {99});
      bad.insert(bad.end(), tail.begin(), tail.end());
      const auto failed = browser::ApplyPushState(state, bad);
      check(failed.status == status, name);
      check(state.simple == before.simple && state.tss == before.tss && state.rt.header == before.rt.header && std::bit_cast<uint32_t>(state.vsc[190][2]) == 0x7fc01234, "failed stream publishes no partial state");
    };
    for (const uint32_t method : {0x17fcu, 0x1800u, 0x1808u, 0x1810u, 0x1818u, 0x1d94u, 0x1d70u, 0x17d0u, 0x1e90u, 0x1500u}) {
      reject({0x40000 | method, 0}, browser::PushStateStatus::OutputWrite, "draw clear report semaphore and transform execution stop");
    }
    for (const uint32_t control : {1u, 2u, 0x20000000u, 0x20000u}) {
      reject({control}, browser::PushStateStatus::UnsupportedControl, "jump call return stop");
    }
    reject({0x40000, 0x97}, browser::PushStateStatus::UnsupportedControl, "object rebind stops");
    reject({0x42300, 1}, browser::PushStateStatus::UnsupportedControl, "other subchannel stops");
    reject({0x40404, 0}, browser::PushStateStatus::UnsupportedMethod, "unknown aligned method stops");
    reject({0x40300}, browser::PushStateStatus::Malformed, "truncated payload stops");
    reject({0x80001ffc, 0}, browser::PushStateStatus::Malformed, "reserved header stops");
    reject({0x801ffc, 0, 0}, browser::PushStateStatus::Malformed, "incrementing register overflow stops");
    reject({0x300}, browser::PushStateStatus::Malformed, "zero count stops");
    reject({0x41ea4, 192}, browser::PushStateStatus::Malformed, "constant load bounds");
    reject({0x41e9c, 136}, browser::PushStateStatus::Malformed, "program load bounds");
    reject({0x41ea4, 191, 0x240b80, 1, 2, 3, 4, 5, 6, 7, 8, 9}, browser::PushStateStatus::Malformed, "constant overflow stops atomically");
    reject({0x41e94, 6}, browser::PushStateStatus::UnsupportedMethod, "unrepresented active hardware program stops");
    reject({0x40100, 1}, browser::PushStateStatus::UnsupportedControl, "interrupting nop stops");
    reject({0x41d84, 4}, browser::PushStateStatus::UnsupportedMethod, "unverified optimization bits stop");
    for (uint32_t n = 0; n < 4; ++n) {
      const std::array<uint32_t, 2> optimization{0x41d84, n};
      check(browser::ApplyPushState(state, optimization).status == browser::PushStateStatus::StateOnly && state.simple[0x1d84 / 4] == n, "retained Z stencil optimization values preserve typed state");
    }
    state = before;
    const std::array<uint32_t, 2> load{0x41ea4, 17};
    const std::array<uint32_t, 5> upload{0x100b80, 0x3f800000, 0x40000000, 0x40400000, 0x40800000};
    check(browser::ApplyPushState(state, load).status == browser::PushStateStatus::StateOnly, "constant load published across captures");
    check(browser::ApplyPushState(state, upload).status == browser::PushStateStatus::StateOnly && state.vsc[17] == std::array<float, 4>{1, 2, 3, 4} && state.simple[0x1ea4 / 4] == 18, "incremental capture constant cursor preserved");
    state = before;
    check(browser::ApplyPushState(state, {}).status == browser::PushStateStatus::StateOnly, "empty stream accepted");
    browser::PushStateLimits limits;
    limits.maxWords = words.size() - 1;
    check(browser::ApplyPushState(state, words, limits).status == browser::PushStateStatus::LimitExceeded, "word budget checked");
    limits = {};
    limits.maxPackets = 1;
    check(browser::ApplyPushState(state, words, limits).status == browser::PushStateStatus::LimitExceeded, "packet budget checked");
    check(state.simple == before.simple, "limit failure leaves state unchanged");
    auto lateFailure = words;
    packet(lateFailure, 0x1d94, {0xf0});
    check(browser::ApplyPushState(state, lateFailure).status == browser::PushStateStatus::OutputWrite && state.simple == before.simple && state.tss == before.tss, "late output rejects all staged packets");
    reject({0x41e9c, 135, 0x200b00, 1, 2, 3, 4, 5, 6, 7, 8}, browser::PushStateStatus::Malformed, "program upload overflow checked");
    const std::array<uint32_t, 2> colorKey{0x40ae8, 0x12345678};
    check(browser::ApplyPushState(state, colorKey).status == browser::PushStateStatus::StateOnly && state.tss[2][gpu::kTssColorKeyColor] == 0x12345678, "color key stage index mapped");
    const std::array<uint32_t, 2> border{0x41be4, 0xaabbccdd};
    check(browser::ApplyPushState(state, border).status == browser::PushStateStatus::StateOnly && state.tss[3][gpu::kTssBorderColor] == 0xaabbccdd, "border color stage index mapped");
    std::vector<uint32_t> maximum(2048, 7);
    maximum[0] = 0x40000300 | (2047u << 18);
    const auto maximumResult = browser::ApplyPushState(state, maximum);
    check(maximumResult.status == browser::PushStateStatus::StateOnly && maximumResult.packets == 1 && maximumResult.values == 2047 && state.simple[0x300 / 4] == 7, "maximum nonincrementing packet count");
    std::puts("push-state-contracts-ok");
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
