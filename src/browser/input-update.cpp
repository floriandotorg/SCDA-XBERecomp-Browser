#include "browser/hooks.h"
#include "cpu/x86_ops.h"

// UXboxViewport::UpdateInput: the original and mouse.cpp's keyboard and mouse work after it (game_sub_0004B525), then,
// in a mission build, the simulation sample MissionInput takes from it
BROWSER_GAME_CHAIN(0004B525)
X86_FUNC(sub_0004B525) {
  const uint32_t viewport = ctx.ecx;
  game_sub_0004B525(ctx, base);
#ifdef BROWSER_MISSION
  const auto saved = ctx;
  ctx.ecx = viewport;
  browser::Runtime::ObserveSdkEntry(ctx, base, 132);
  ctx = saved;
#else
  (void)viewport;
#endif
}
