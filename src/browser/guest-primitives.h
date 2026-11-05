#pragma once
#include <cstdint>
#include <stdexcept>

namespace browser::menu {
inline uint64_t ExpandedIndexCount(uint32_t primitive, uint32_t count) {
  switch (primitive) {
    case 1: return count;
    case 2: return uint64_t(count / 2) * 2;
    case 3: return count > 2 ? uint64_t(count) * 2 : count == 2 ? 2 : 0;
    case 4: return count > 1 ? uint64_t(count - 1) * 2 : 0;
    case 5: return uint64_t(count / 3) * 3;
    case 6: case 7: case 10: return count > 2 ? uint64_t(count - 2) * 3 : 0;
    case 8: return uint64_t(count / 4) * 6;
    case 9: return count > 3 ? uint64_t((count - 2) / 2) * 6 : 0;
    default: throw std::runtime_error("menu backend unsupported topology");
  }
}
}
