#include "browser/guest-vertex.h"
#include "browser/guest-primitives.h"
#include "browser/cpu_runtime.h"
#include <cstdio>

int main() {
  const auto check = [](bool value) { if (!value) { throw std::runtime_error("resident buffer contract failed"); } };
  const uint32_t expected[10][7] = {
    {0, 1, 2, 3, 4, 5, 6}, {0, 0, 2, 2, 4, 4, 6},
    {0, 0, 2, 6, 8, 10, 12}, {0, 0, 2, 4, 6, 8, 10},
    {0, 0, 0, 3, 3, 3, 6}, {0, 0, 0, 3, 6, 9, 12},
    {0, 0, 0, 3, 6, 9, 12}, {0, 0, 0, 0, 6, 6, 6},
    {0, 0, 0, 0, 6, 6, 12}, {0, 0, 0, 3, 6, 9, 12}
  };
  for (uint32_t n = 0; n < 70; ++n) { check(browser::menu::ExpandedIndexCount(n / 7 + 1, n % 7) == expected[n / 7][n % 7]); }
  check(browser::menu::ExpandedIndexCount(3, UINT32_MAX) == uint64_t(UINT32_MAX) * 2);
  browser::Memory memory;
  const auto before = memory.PhysicalWriteVersion({}, 0x80010000, 4096);
  {
    auto lease = memory.Borrow();
    auto* bytes = memory.Span({}, 0xf0010000, 4096);
    check(!memory.PhysicalWriteVersion({}, 0x80010000, 4096));
    bytes[0] = 17;
    check(!memory.PhysicalWriteVersion({}, 0xf0010000, 4096));
  }
  check(memory.PhysicalWriteVersion({}, 0x80010000, 4096) > before);
  const auto destinationVersion = memory.PhysicalWriteVersion({}, 0x80012000, 4096);
  X86Context context{};
  context.edi = 0xf0012000; context.ecx = 4; context.eax = 73;
  memory.StoreRepeated(context, 4);
  check(memory.PhysicalWriteVersion({}, 0x80012000, 4096) > destinationVersion);
  const auto copiedVersion = memory.PhysicalWriteVersion({}, 0xf0013000, 4096);
  context.edi = 0x80013000; context.esi = 0xf0012000; context.ecx = 4;
  memory.MoveRepeated(context, 4);
  check(memory.PhysicalWriteVersion({}, 0x80013000, 4096) > copiedVersion);
  check(memory.Load<uint32_t>({}, 0xf0013000) == 73);
  memory.Ensure(0x20000, 4096);
  memory.Store<uint32_t>({}, 0x20000, 79);
  check(!memory.PhysicalWriteVersion({}, 0x20000, 4096));
  check(browser::menu::GuestVertex(0x16).format == 32);
  check(browser::menu::GuestVertex(0x40).decode.find("guest_unorm(v.z") != std::string::npos);
  check(browser::menu::GuestVertex(0x72).decode.find(".z") != std::string::npos);
  check(browser::menu::GuestVertex(0x25).format == 17);
  check(browser::menu::GuestVertex(0x35).extraFormat == 16);
  std::puts("resident-buffers-ok");
}
