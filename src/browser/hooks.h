#pragma once
#include "browser/cpu_runtime.h"
#include "cpu/x86_ops.h"

// The session native game hooks run against (hook-compat.cpp, hook-d3d.cpp): bound when a story Session is built,
// unbound when it is torn down
namespace browser::hooks {

void Bind(Memory& memory);
void Unbind();
// mem::AllocSystem and AllocContiguous hand out a contiguous block the session reserved before the game ran. Hook
// statics keep its addresses across a story restart, so the next session reserves it at the same address and gets its
// bytes back (PoolUsed, from the old session, before it ends).
inline constexpr uint32_t kPoolBytes = 1u << 20;
void BindPool(uint32_t address);
uint32_t PoolUsed();
// dispatch::HostThunk's runtime
void BindRuntime(Runtime* runtime);
// counts Bind calls: hook statics outlive a story restart, a hook that must not can key them by this
uint64_t Generation();
uint8_t* Base();
Memory& Bound();
const X86Context& NoContext();

}

// A browser override at an address a game hook also takes calls game_sub_<address>: the hook, renamed from
// sub_<address> when its source is selected, else this weak forward to the recompiled body.
#define BROWSER_GAME_CHAIN(address) \
  X86_FUNC(__imp__sub_##address); \
  extern "C" __attribute__((weak)) void game_sub_##address(X86Context& __restrict ctx, uint8_t* base) { __imp__sub_##address(ctx, base); }
