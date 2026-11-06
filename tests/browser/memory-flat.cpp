#include "cpu/x86_ops.h"
#include <array>
#include <cstdio>
#include <stdexcept>

int main() {
  try {
    browser::Memory memory;
    X86Context ctx{};
    uint8_t* base = memory.Base();
    const auto check = [](bool valid, const char* reason) { if (!valid) { throw std::runtime_error(reason); } };
    check(&browser::Memory::From(base) == &memory, "flat base does not resolve its owner");
    const auto unmapped = [&](uint32_t address) {
      return reinterpret_cast<uintptr_t>(browser::FlatHost(base, address)) == browser::Memory::kFlatUnmapped + (address & 4095);
    };
    check(unmapped(0x10000) && unmapped(0x7fffffff), "unmapped pages translate to the trap window");
    memory.Map(0x10000, 3 * 4096);
    check(unmapped(0x10000), "mapping without commitment translates");
    memory.Commit(0x10000, 2 * 4096);
    check(!unmapped(0x10000) && !unmapped(0x11fff) && unmapped(0x12000), "commitment does not translate exactly its pages");
    X86_MEM32(0x10ffe) = 0x11223344u;
    check(memory.Load<uint32_t>(ctx, 0x10ffe) == 0x11223344u, "cross-page flat store differs from checked load");
    memory.Store<uint16_t>(ctx, 0x11010, 0xbeef);
    check(uint16_t(X86_MEM16(0x11010)) == 0xbeef, "checked store differs from flat load");
    memory.Decommit(0x11000, 4096);
    check(unmapped(0x11000) && !unmapped(0x10000), "decommitment keeps translation");
    memory.Commit(0x11000, 4096);
    check(uint32_t(X86_MEM32(0x11010)) == 0, "recommitted page is not zeroed");
    X86_MEMF64(0x80010008) = 2.5;
    check(double(X86_MEMF64(0xf0010008)) == 2.5, "physical aliases do not share flat storage");

    const auto version = memory.PhysicalWriteVersion(ctx, 0x80010000, 4096);
    check(version && memory.PhysicalWriteVersion(ctx, 0x80010000, 4096) == version, "unchanged physical page changed version");
    X86_MEM8(0xf0010fff) = 7;
    const auto written = memory.PhysicalWriteVersion(ctx, 0x80010000, 4096);
    check(written && *written > *version, "flat alias store does not advance the physical version");
    check(memory.PhysicalWriteVersion(ctx, 0x80010000, 4096) == written, "physical version advanced without a store");
    check(memory.PhysicalWriteVersion(ctx, 0x80011000, 4096) == memory.PhysicalWriteVersion(ctx, 0x80011000, 4096), "neighbor page changed");
    *X86_PTR(uint32_t, 0x80011004) = 9;
    check(*memory.PhysicalWriteVersion(ctx, 0x80011000, 4) > *written, "flat pointer store does not advance the physical version");

    const auto wide = memory.PhysicalWriteVersion(ctx, 0x80020800, 17 * 4096);
    check(memory.PhysicalWriteVersion(ctx, 0x80020800, 17 * 4096) == wide, "unchanged wide physical range changed version");
    X86_MEM32(0xf0029004) = 3;
    const auto changed = memory.PhysicalWriteVersion(ctx, 0x80020800, 17 * 4096);
    check(changed && *changed > *wide, "store inside a wide physical range did not advance its version");
    check(memory.PhysicalWriteVersion(ctx, 0x80029000, 4) == changed && memory.PhysicalWriteVersion(ctx, 0x80020800, 17 * 4096) == changed,
      "consumed wide range write changed later versions");
    X86_MEM8(0x80031800) = 1;
    check(*memory.PhysicalWriteVersion(ctx, 0x80020800, 17 * 4096) > *changed, "store in the final page of a wide range was missed");
    for (uint32_t n = 0; n < 8; ++n) { X86_MEM8(0x10ff8 + n) = uint8_t(n + 1); }
    ctx.esi = 0x10ff8; ctx.edi = 0x10ffa; ctx.ecx = 6; ctx.df = 0;
    x86_rep_movs(ctx, base, 1);
    const std::array<uint8_t, 8> replicated{1, 2, 1, 2, 1, 2, 1, 2};
    for (uint32_t n = 0; n < 8; ++n) { check(uint8_t(X86_MEM8(0x10ff8 + n)) == replicated[n], "overlapping forward movs is not element ordered"); }
    check(ctx.esi == 0x10ffe && ctx.edi == 0x11000 && ctx.ecx == 0, "movs registers differ");
    ctx.edi = 0x10ff0; ctx.ecx = 8; ctx.eax = 0xa1b2c3d4;
    x86_rep_stos(ctx, base, 4);
    for (uint32_t n = 0; n < 8; ++n) { check(uint32_t(X86_MEM32(0x10ff0 + n * 4)) == 0xa1b2c3d4, "stos across pages differs"); }
    check(ctx.edi == 0x11010 && ctx.ecx == 0, "stos registers differ");
    ctx.esi = 0x10ffc; ctx.edi = 0x11ffc; ctx.ecx = 3; ctx.df = 1;
    x86_rep_movs(ctx, base, 4);
    for (uint32_t n = 0; n < 3; ++n) { check(uint32_t(X86_MEM32(0x11ff4 + n * 4)) == 0xa1b2c3d4, "backward movs differs"); }
    check(ctx.esi == 0x10ff0 && ctx.edi == 0x11ff0, "backward movs registers differ");
    ctx.edi = 0x80012000; ctx.ecx = 4096; ctx.df = 0; ctx.eax = 0;
    const auto beforeFill = memory.PhysicalWriteVersion(ctx, 0x80012000, 4096);
    x86_rep_stos(ctx, base, 1);
    check(*memory.PhysicalWriteVersion(ctx, 0x80012000, 4096) > *beforeFill, "flat fill does not advance the physical version");

    memory.Discard(0x10000, 3 * 4096);
    check(unmapped(0x10000) && unmapped(0x11000), "discarded region still translates");
    std::puts("memory-flat-contracts-ok");
    return 0;
  } catch (const std::exception& exception) {
    std::fprintf(stderr, "%s\n", exception.what());
    return 1;
  }
}
