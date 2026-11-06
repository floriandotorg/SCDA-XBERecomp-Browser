# C14b: Co-op lockstep, co-op mouse and local second player

Class: Complex
Depends on: C01 (hook foundation: memory helpers, symbol macros, app::Settings shim, host-settings channel, selected hook source compilation), C14a (virtual LAN transport, browser xnet provider, `SCDC` side channel with `CoopHostValues` and `SetCoopHost`, Coop menu gate lifted). Tasks 5 to 7 (co-op mouse) need C03 (browser input core with `input::TakeMouse`, src/browser/input-core.cpp). Tasks 9 to 11 (local second player) need only C01, but must agree with C03 on how ports and the keyboard player are represented.
Native sources: src/game/coop_sync.cpp, src/game/coop_mouse.cpp, src/game/mouse.cpp, src/xdk/xnet.cpp, src/kernel/boot.cpp, src/app/app.h, src/app/settings.cpp, src/app/menu.cpp, src/core/input.cpp

## Goal

Story co-op in the browser plays as well as on the native PC port. Both peers run the Quazal Net-Z lockstep at the host's raised tick rate and input delay floor. Mouse look is recorded into the lockstep, so it does not desync. Two pads on one machine can start split screen co-op. Networked play uses the C14a virtual LAN; this chunk adds the gameplay side.

## Native behaviour (cite file:line)

Lockstep (src/game/coop_sync.cpp)
- Co-op is Quazal Net-Z's SyncSim, a fixed step lockstep. The session init `sub_0025480C` caps the step frequency at 22 Hz and sets a 50 ms input delay floor. Both are options. Both machines need the same values, so the host's are sent to the client beside the game's traffic (coop_sync.cpp:1-11).
- Constants (coop_sync.cpp:29-44): kMaxRate 250 (31), kFrameLimit 0x4F64D0 (a double, stock 1/22 s), kMaxDelayFloor 200, kSyncSim pointer 0x672974, kCeiling +0x84, kSteps +0x5C, kRate +0x7C, kStockRate 22, auto ceiling timings, and GEngine 0x5ED158 with the local player number at +0xD4 (1 host, 2 client).
- `Session` uses the host's values when this machine is not the host and has received them (coop_sync.cpp:57-66). `Ceiling` drops the ceiling to the steps actually reached and climbs back after holding (coop_sync.cpp:69-103). `Hosting` registers the host's values with xnet while `input::NetSession()` is true and the player number is 1 (coop_sync.cpp:106-116).
- Hooks: step frequency setter `sub_0040A7A0` (coop_sync.cpp:134), input delay floor setter `sub_0040A740` (145), step delta `sub_002535D6` (155). The last one returns the real step time (`1000 / rate` whole ms) and applies the ceiling each step.
- Host values travel in the `SCDC` packet on PRUDP port 1000 (src/xdk/xnet.cpp:104-136, 371). Natively they cross a relaunch on the command line (`--coop-host`, src/kernel/boot.cpp:184-187; xnet.cpp:478-491).
- Net session detection: engine +0xC8 and +0xCC, or +0xDC (src/game/mouse.cpp:53-54, 327), passed to `input::SetNetSession` (mouse.cpp:354). While in a net session, native mouse look is not sent as direct axis events (mouse.cpp:355-361).

Co-op mouse (src/game/coop_mouse.cpp)
- The lockstep records only the six pad axes (Net-Z PlayerInput::SetAxis `sub_002A4114`) and replays every player's input through `UInput::DirectAxis` on each machine. A mouse axis event therefore turned only the local copy and desynced (coop_mouse.cpp:1-6).
- The mouse rides on IK_JoyR 0xE3 and IK_JoyU 0xE8, the trigger axes nothing binds; the replay turns them into IK_MouseX 0xE4 and IK_MouseY 0xE5 (coop_mouse.cpp:15).
- UInput: +0xEEC engine; recording while +0xECC and +0xEDC are set and +0xED0 is clear. Engine: +0xC8 and +0xCC, or +0xDC, in a synced session. Split screen sets +0xCC alone, and there the triggers stay triggers (coop_mouse.cpp:17-20).
- Hook `sub_00246AD6` (UInput::DirectAxis, thiscall) at coop_mouse.cpp:36-50.

Local players
- `Port { bool keyboard; int pad; }` and `players[2]`, by default player 1 on keyboard and pad 0, player 2 on pad 1 (src/app/app.h:23-27, 71).
- Parsing removes duplicates and keeps player 1 non empty (src/app/settings.cpp:122-130). The menu rule: single player and versus are driven by player 1 alone (src/app/menu.cpp:263-272).
- Pads are read per guest port (src/core/input.cpp:1631, 1658). The keyboard belongs to whichever port has it (`KeyboardPort`, input.cpp:1205-1211).
- Split screen co-op exists in story: the HUD fixes for it are in src/game/hud.cpp:145-165, outside this chunk.

Settings
- coop_tick_rate (default 60; menu choices 0, 30, 50, 60, 100, 111, 125, 142) and coop_input_delay_floor_ms (default 20; choices 0, 10, 20, 30, 40, 50) (src/app/app.h:66-67; src/app/settings.cpp:117-118, 176-177; src/app/menu.cpp:55-56). player1 and player2 (settings.cpp:122-123, 181-182).

## Browser today (cite file:line; where the work plugs in)

- Without C14a, XNet is refused (`XNetStartup` returns 0x276B, src/browser/sdk-provider.cpp:60-68), and the menu route swallows the Coop press (src/browser/menu-route.cpp:250-259). C14a lifts both; this chunk assumes it has.
- Mission mouse look injects IK_MouseX and IK_MouseY directly after `sub_0004B525` (src/browser/mission-input.cpp:9-17, 241-247). In a synced session this desyncs, as described above.
- Input ports: the XPP gamepad record reports `{1, 1, 0}` (src/browser/menu-input.cpp:115), and any handle on a port other than 0 fails with 1167 (menu-input.cpp:139). gamepad-input.mjs publishes only the first connected standard pad (src/browser/gamepad-input.mjs:51) into one 32 byte span (gamepad-input.mjs:3).
- In-process restart keeps the module across front end to mission (src/browser/boot-wasm.cpp:467-496). C14a keeps the received host values in the story state (boot-wasm.cpp:65-70).
- The browser story mission is not yet at 60 FPS (docs/browser-port/06-playable-mission.md:53). This limits the reachable step rate.
- settings.mjs has a Controls tab with sensitivity and invert (src/browser/menu-shell.mjs:16-19). There is no port choice.

## Tasks (numbered, concrete, each verifiable)

Lockstep
1. Port coop_sync.cpp through C01. Replace `input::NetSession()` with the engine flag test from src/game/mouse.cpp:327 (a small helper reading GEngine 0x5ED158). Read `coop_tick_rate` and `coop_input_delay_floor` from the C01 settings shim. Use `xnet::CoopHostValues` and `xnet::SetCoopHost` from C14a. Verify in two tabs that both log the same rate and floor and that the client reports "the host's".
2. Unit test `Ceiling` on the host with a fake clock and a fake step counter: hold, lower after being behind twice, raise by a fifth after the hold, doubling hold after a failed raise. Write the test first by extracting the function behind a small interface.
3. Confirm the frame limit write (kFrameLimit 0x4F64D0 set to 0 for a raised rate) has the intended effect on browser pacing. With a raised rate the lockstep alone must pace the frames.
4. Verify that the received host values survive the co-op front end to mission restart (C14a task 6). The client must log the host's values in the mission.

Co-op mouse
5. Port coop_mouse.cpp (`sub_00246AD6`), feeding it the browser mouse delta through `input::TakeMouse` from the C03 input core.
6. Make mission-input.cpp skip its direct IK_MouseX and IK_MouseY events (lines 241-247) while the engine flags say a synced session is active, and leave split screen (+0xCC alone) unchanged. Unit test the flag predicate for the three cases: solo, synced, split screen.
7. Verify in two tabs that mouse look moves the correct player on both screens for 10 minutes without desync or a session drop.

Measurement
8. Measure steps per second reached against the set rate at 22, 60 and 142 Hz, over WebRTC and over relay, with the auto ceiling. Record the results in this chunk's evidence with the host and browser used.

Local second player
9. gamepad-input.mjs: publish up to 4 standard pads in fixed slots keyed by `Gamepad.index`, each with its own span, and keep a slot stable when a pad reconnects. Unit tests with a fake `navigator.getGamepads`: two pads, unplug and replug, a non standard mapping ignored.
10. menu-input.cpp and the mission input: build the XPP connected bits from the configured ports, return per port samples and per port rumble, and keep 1167 for ports without a device. The keyboard follows the port that owns it. Native Wasm tests: port 1 opens when configured and fails when empty.
11. Verify that the game's split screen co-op start accepts player 2's pad, and that both players move independently.

## Settings and UI (settings.mjs entries, Options panel / main menu changes)

- `coopTickRate`: options 0, 30, 50, 60, 100, 111, 125, 142; initial 60; 0 shown as "Original (22 Hz)". Network tab, or Controls if no Network tab exists yet.
- `coopInputDelayFloor`: options 0, 10, 20, 30, 40, 50 (ms); initial 20.
- `player1Port`: options keyboard, pad 1 to 4, keyboard and pad 1; initial keyboard and pad 1.
- `player2Port`: options none, keyboard, pad 1 to 4; initial pad 2.
- The port rows follow src/app/settings.cpp:124-129 and src/app/menu.cpp:263-272: a device sits on one port only, and a choice that would leave player 1 empty is refused.
- A short note on the co-op rows: "Both players should use the same values; the host's are used while joined."

## Acceptance criteria

- In two tabs or two machines, a co-op mission starts, plays for 10 minutes and survives a mission restart without desync or session drop, at 60 Hz with a 20 ms floor over WebRTC and at the stock 22 Hz over relay.
- The client logs the host's tick rate and floor. Changing them on the client has no effect while joined.
- Mouse look in co-op moves only the local player, on both screens.
- Two pads on one machine start split screen co-op, and both players are controllable.
- Solo play is unchanged: the mouse look injection still runs outside synced sessions.

## Testing

- Unit tests first: `Ceiling`, the session flag predicate, gamepad slot assignment, port setting normalisation.
- Native Wasm tests for per port input.
- Browser: agent-browser CLI with two sessions against a separate served build dir and a local relay on its own port. Never use port 8125 or out/boot-m5. Mute audio with out/front-end/mute.js. Log step rate, ceiling changes and RTT to the diagnostics output for the soak.
- Split screen: agent-browser cannot plug real pads, so inject two fake pads through a test hook on `navigator.getGamepads`.

## Risks and open questions

- Browser frame rate: a peer that cannot hold the rate lowers the ceiling for both. The auto ceiling limits slow motion but does not remove it.
- Does the split screen start path need anything beyond port 1 input (for example a second viewport cost that breaks the 1 GiB heap budget)? Measure.
- The native mouse delta is in Windows counts. The browser pointer lock delta scale differs, so the `Speed=0.001` binding may need a browser side scale to feel the same.
- Lockstep determinism across browsers and machines: the generated code is deterministic, but host provided values (timers, random) must not differ in ways the game feeds into the simulation. Watch the first desync logs carefully.

## Out of scope

- The transport, the xnet provider, the `SCDC` packet itself, rooms and the Coop menu gate (C14a).
- Voice and the Communicator (C14c).
- Split screen HUD layout fixes (src/game/hud.cpp) and split screen rendering performance.
- Versus. It is client and server (src/audio/voice_link.cpp:4), and the native port has no versus lockstep hooks.
