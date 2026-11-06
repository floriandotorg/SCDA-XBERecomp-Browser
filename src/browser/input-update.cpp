#include "browser/hooks.h"
#include "browser/host-messages.h"
#include "cpu/x86_ops.h"
#include "game/console.h"

// UXboxViewport::UpdateInput: the original and mouse.cpp's keyboard and mouse work after it (game_sub_0004B525), which
// also runs queued console commands; then, in a mission build, the simulation sample MissionInput takes from it
BROWSER_GAME_CHAIN(0004B525)
X86_FUNC(sub_0004B525) {
  const uint32_t viewport = ctx.ecx;
  namespace messages = browser::host_messages;
  while (const auto message = messages::Take()) {
    if (message->kind == messages::kConsole) { game::QueueCommand(message->text, game::Route::Player); }
  }
  game_sub_0004B525(ctx, base);
  for (const auto& line : game::TakeOutput()) { messages::Post(messages::kOutput, line.substr(0, messages::kPayloadBytes)); }
#ifdef BROWSER_MISSION
  const auto saved = ctx;
  ctx.ecx = viewport;
  browser::Runtime::ObserveSdkEntry(ctx, base, 132);
  ctx = saved;
#else
  (void)viewport;
#endif
}
