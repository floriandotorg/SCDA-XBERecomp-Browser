# C13a: Versus image runtime (build, identity, image switching)

Class: Complex
Depends on: C01 (hook foundation: browser memory helpers, symbol macros, app::Settings shim, host-settings channel, selected hook source compilation). Soft dependency on durable save persistence (M7 P7.1 or its parity chunk), see Risks.
Native sources: CMakeLists.txt, config/sc4_adv.toml, config/xdk_symbols_adv.toml, generated/versus/*, src/kernel/boot.cpp, src/kernel/xboxkrnl.cpp, src/xdk/xapi.cpp, src/app/handover.cpp, src/app/main.cpp

## Goal

Picking Versus on the game's own game mode list in the browser story front end replaces the running story runtime with a recompiled sc4_adv.xbe runtime, which boots to the versus main menu. Leaving versus through its own menu returns to the story front end. Both directions work without a page reload and without selecting the ISO again. This chunk delivers the second image build, its identity checks, the runtime switch with launch data and kept display, the profile hand-over and a memory measurement. Porting the src/versus hooks is C13b. Networked versus is C14a to C14c.

## Native behaviour (cite file:line)

Build and identity
- One executable per image: `scda_image(SCDA default.xbe scda_v2.toml 128 game)` and `scda_image(SCDA_Versus sc4_adv.xbe sc4_adv.toml 64 versus)` (CMakeLists.txt:161-162). Each image gets `src/<game_dir>/*.cpp` as hooks (CMakeLists.txt:124) and its own generated code DLL (CMakeLists.txt:145-159). The executable embeds the xbe SHA-1 (CMakeLists.txt:125-130).
- config/sc4_adv.toml:1-11: title 5553005E (the same title id as story), output `generated/versus`, 64 partitions (story has 128), code sections `.text D3D D3DX XGRPH DSOUND XNET XONLINE XPP`. XDK symbol names come from config/xdk_symbols_adv.toml.
- generated/versus/x86_recomp_stats.txt: 14,975 functions, 1,092,256 instructions. Story (generated/game/x86_recomp_stats.txt): 33,172 functions, 1,737,201 instructions. On disk generated/versus is 137 MB, generated/game 219 MB.
- generated/versus/image_symbols.h defines `IMAGE_XBE "sc4_adv.xbe"` and the same SYM_ set as story except `SYM_D3DResource_Release`, which versus lacks (story: generated/game/image_symbols.h:231). Browser code already guards that one with `#ifdef` (src/browser/diagnostic-graphics.cpp:66).
- Both images use `sub_XXXXXXXX` names for unrelated code at the same addresses, so the two images cannot be linked into one binary.

Front end entry
- The story game mode list is Solo 0, Coop 1, Versus 2, Credits (src/game/exit_menu.cpp:124-126, press handler `sub_0018697A`).
- default.xbe holds `D:\sc4_adv.xbe` at VA 0x564944. Its only reference is the push at 0x0026AE01 inside `sub_0026AB34`, followed by `call 0x00055666` (`XLaunchNewImageA`, generated/game/image_symbols.h:361) at 0x0026AE06, with launch data `[ebx+0x1208]`, taken when `[ebx+0x4C]` is nonzero (generated/game/x86_recomp.59.cpp:23396-23417).
- sc4_adv.xbe returns to story through its `XLaunchNewImageA` at 0x0001A435: `sub_001732B0` pushes `d:\DEFAULT.XBE` (0x33663C) at 0x0017334D; `sub_00185CB0` pushes 0x336E30 at 0x00185D72 and 0x00185E52; `sub_00011910` pushes `d:\default.xbe` (0x31C3F4) at 0x00011B75 right after `D3DDevice_PersistDisplay` (0x002840F0), inside a loop bounded by 4 (generated/versus/x86_recomp.29.cpp:15425, x86_recomp.31.cpp:1172 and 1303, x86_recomp.0.cpp:2782).

Image switching
- src/app/handover.cpp only covers the receiving side: the new instance opens its window after its first frames and releases the old one (handover.cpp:24-38). The switch itself is elsewhere:
- The XAPI hook lets a launch through only for a recompiled image (src/xdk/xapi.cpp:121-131). `ImageExe` maps a path ending in `default.xbe` to SCDA and `sc4_adv.xbe` to SCDA_Versus, matching after `\` or `;` (src/kernel/boot.cpp:160-170).
- `HalReturnToFirmware(2)` writes the 4 KiB launch data page to `launch.bin` and calls `Relaunch` (src/kernel/xboxkrnl.cpp:899-917).
- `Relaunch` starts the other executable suspended and keeps the old window until the new one presents (src/kernel/boot.cpp:172-216). It closes sockets first (`xnet::CloseSockets`, boot.cpp:189) and carries co-op host values on the command line (boot.cpp:184-187).
- The new instance reads `launch.bin` into a contiguous page at `LaunchDataPage` and loads the persisted display (src/kernel/boot.cpp:139-152).
- Profiles: both images share title id 5553005E, so story and versus read the same save namespace on the native host disk.

## Browser today (cite file:line; where the work plugs in)

- Build hard codes the story image: sources from `generated/game` 0..127 (tools/browser/CMakeLists.txt:84-87), include dir `generated/game` (171, 204), compact copy through prepare-cpu.mjs which reads `generated/game` (tools/browser/prepare-cpu.mjs:12-13; CMakeLists.txt:191-198). Inventory requires exactly 128 partitions and `config/scda_v2.toml` (tools/browser/inventory.mjs:87-101). Provenance pins `assets/default.xbe` (tools/browser/provenance.mjs:4-37). The mission heap ceiling is 1 GiB (tools/browser/CMakeLists.txt:118-121). The published module is `story_boot` (tools/browser/CMakeLists.txt:124-162). The served story wasm is about 57.7 MB (out/boot-m5/boot-web/story_boot.wasm).
- The worker entry statically imports `./story_boot.mjs` and `./boot-manifest.mjs` (src/browser/boot-worker.mjs:1-2).
- ISO validation accepts only `default.xbe` (src/browser/iso-validation.mjs:27, 64, 76).
- Launch gates: `MenuLaunch::Dispatch` refuses any path not ending in `default.xbe` with eax 2 (src/browser/menu-launch.cpp:106-114). The firmware restart throws for any other executable (menu-launch.cpp:159-163).
- Menu route swallows Coop and Versus: `if (selection != 1 && selection != 2) return true;` and then returns the press as handled (src/browser/menu-route.cpp:250-259).
- In-process restart: `image_restart` rebuilds the session from the same image bytes with the saved launch page and persisted display (src/browser/boot-wasm.cpp:467-496). The JS loop is src/browser/boot-session.mjs:419-430. `Restore` writes page and display into the new runtime (boot-wasm.cpp:303-320).
- Story only providers with hard coded default.xbe addresses: src/browser/menu-route.cpp:5-19 (`sub_0018697A`, 0x28FA86 and others), src/browser/sdk-provider.cpp:16-27 (signature hooks at 0x534B8 and 0x5351A, declared weak), src/browser/loading-io.cpp:5-36 (`sub_004B8EC0`, 0x5E3E24), src/browser/mission-input.cpp:9-29 and 238-247.
- Session teardown exists: the worker broker terminates all module workers (src/browser/boot-worker-broker.mjs:26-29, 85-90), used by src/browser/boot-runtime.mjs:144.
- The writable temporary filesystem lives in the Wasm heap (src/browser/temporary-filesystem.h:15-18), so it is lost when the module is replaced.

Where the work plugs in: a second link target fed from `generated/versus`, a versus worker entry, an image switch in boot-session.mjs and boot-runtime.mjs, launch gates in menu-launch.cpp and menu-route.cpp, and per image provider selection in boot-wasm.cpp.

## Tasks (numbered, concrete, each verifiable)

1. Give inventory.mjs, prepare-cpu.mjs and provenance.mjs an image descriptor `{ id, xbe, config, symbols, generated, partitions }`. Story: default.xbe, scda_v2.toml, xdk_symbols.toml, generated/game, 128. Versus: sc4_adv.xbe, sc4_adv.toml, xdk_symbols_adv.toml, generated/versus, 64. Verify story output is unchanged by comparing input-manifest.json and compact copy hashes before and after.
2. Add a `BROWSER_IMAGE` cache option (story by default, or versus) to tools/browser/CMakeLists.txt. It selects the descriptor, the partition range, the include dir and the output names (`versus_boot.wasm`, `versus_boot.mjs`, `versus-manifest.mjs`). Build versus in its own build dir (for example out/versus-boot). Never touch out/boot-m5. Verify the link passes with `-sERROR_ON_UNDEFINED_SYMBOLS=1`.
3. Make the story only providers image conditional: compile menu-route.cpp, loading-io.cpp, mission-input.cpp and the sdk-provider signature hooks only for story, and install providers per image in boot-wasm.cpp. Verify a versus build contains no browser hook for `sub_0018697A` (name section of a trace build).
4. Produce a generation record for sc4_adv.xbe with provenance.mjs and a versus disc identity in the manifest: `image: 'sc4_adv.xbe'`, SHA-256, size, and the required versus files under `adv/`. Take the file list from a real disc listing; do not guess names.
5. Extend iso-validation.mjs to validate the manifest's own `image` (default.xbe or sc4_adv.xbe) while keeping the story error codes. Write the unit tests first: a versus manifest accepts sc4_adv.xbe with the right hash, rejects size and hash mismatches, and a story manifest never accepts sc4_adv.xbe.
6. Add a versus worker entry (versus-boot-worker.mjs importing `./versus_boot.mjs` and `./versus-manifest.mjs`) and let boot-runtime.mjs pick the entry by image id. Publish both modules into one served directory.
7. Accept `sc4_adv.xbe` in `MenuLaunch::Dispatch` (menu-launch.cpp:106) and `InstallFirmware` (menu-launch.cpp:159) with the native `ImageExe` path rules, and record the target image in `MenuLaunchRequested`. Unit test the matcher: `\Device\CdRom0;sc4_adv.xbe`, `D:\sc4_adv.xbe` and `d:\DEFAULT.XBE` are accepted; `y:\xboxdash.xbe`, `D:\downloader.xbe` and `xsc4_adv.xbe` are refused.
8. A launch to the other image ends `image_execute` with a new code `IMAGE_SWITCH`. Export the target image id, the 4 KiB page and the persisted display snapshot (`PersistedDisplaySnapshot`, boot-wasm.cpp:473). `image_restart` stays same image only.
9. Implement the switch in boot-session.mjs. On `IMAGE_SWITCH`: copy the page and display out of the heap, stop the session (terminateAll), keep the ISO handle and canvas, start the other module, and validate its xbe from the same ISO. Then pass the page and display in through a new export before `image_bind`, so `Restore` (boot-wasm.cpp:303-320) writes them. Show the kept display or the loading layer during the gap. Verify with a fake module that the page bytes arrive unchanged and that an abort mid switch leaves a clean stopped state.
10. Lift the Versus gate only: in menu-route.cpp:250, let selection 2 reach the original handler so `sub_0026AB34` calls XLaunchNewImageA. Coop (selection 1) stays gated until C14a. Verify in agent-browser that pressing Versus produces `IMAGE_SWITCH` with `D:\sc4_adv.xbe`.
11. Profile hand-over: until durable persistence exists, serialize the temporary filesystem namespace for title 5553005E to JS before teardown and restore it into the new module before boot. If durable persistence has landed, flush it before teardown instead. Verify a profile created in story is listed by the versus profile menu, and a change made in versus is visible back in story.
12. Check the versus import coverage list from the first boot (`image_coverage_*`). Add any kernel or XDK provider sc4_adv.xbe needs that story did not, each with a unit test.
13. Measure memory with measure-memory.mjs on the versus menu and, if one loads offline, a versus map. Record the Wasm heap, translated backing and GPU residency next to the table in docs/browser-port/m5-evidence.md:27-35. Keep the 1 GiB ceiling unless the numbers say otherwise.
14. Return path: a versus exit through `sub_001732B0` or `sub_00185CB0` produces `IMAGE_SWITCH` back to story, and the story module boots with the page and display. Verify two full story to versus to story cycles without a page reload.

## Settings and UI (settings.mjs entries, Options panel / main menu changes)

- There is no new main menu button: versus is entered through the game's own game mode list, as on the console and native.
- Loading layer: "Switching to Versus" and "Returning to story" states while modules swap, with the kept display behind them.
- Developer tab: optional `startInVersus` (boolean, initial false). It boots sc4_adv.xbe directly with an empty launch page, for testing.

## Acceptance criteria

- `BROWSER_IMAGE=versus` builds `versus_boot.wasm` from generated/versus in its own build dir. Story builds are unchanged (same compact source hashes and manifest identity).
- With the real ISO, selecting Versus from the Solo menu's game mode list switches to the versus runtime without a page reload or ISO reselection. The versus main menu renders and accepts pad and keyboard input.
- A wrong sc4_adv.xbe (size or hash) gives a named error, and the story runtime is restarted rather than lost.
- Leaving versus through its own menu returns to the story front end. Over two cycles, the Wasm heap and worker count stay bounded and do not grow after the first cycle.
- Profiles created in story are visible in versus within one page session, and the reverse.

## Testing

- Unit tests first (node, no build): launch path matcher, manifest snapshot and validation for both images, descriptor selection in inventory and prepare-cpu, and the switch protocol in boot-session with a fake module (page and display bytes, error paths, abort during switch).
- Native Wasm tests: `MenuLaunch` with a versus path returns a switch request instead of eax 2; the firmware restart accepts sc4_adv.xbe.
- Browser: agent-browser CLI against a separate served build dir (not port 8125, not out/boot-m5), with audio muted by out/front-end/mute.js. Script: open, select ISO, open Solo, pick Versus, wait for the versus menu, screenshot, back out, wait for the story menu, repeat.
- Memory: measure-memory.mjs runs and heap samples across two cycles.

## Risks and open questions

- Download size: story 57.7 MB plus versus, estimated at 35 to 40 MB from the instruction ratio. Measure it, and consider loading the versus module only on demand.
- Versus build time with 64 partitions is unmeasured; story takes about 8 minutes.
- What does the story launch data at `[ebx+0x1208]` contain for versus, and does versus read any of it (profile, language)? Capture the page bytes on the first switch.
- `sub_00011910` launches `d:\default.xbe` after PersistDisplay in a loop of up to 4. Is this an error path or a disc eject path? Unknown.
- Does the story module tear down fully (GPU worker, audio worklet, ISO worker) so a second module can start in the same page, or must the GPU service restart too? The existing Stop and Start flow suggests it works; confirm.
- Versus audio: src/xdk/dsound.cpp:994 notes versus creates each sound buffer at the sound's size and fills it afterwards. Check that src/browser/audio-provider.cpp handles that order.
- The profile hand-over must be atomic with the switch. A crash between teardown and restore must not lose profiles.
- Can a versus map load offline at all? If versus requires a session to load a map, map level acceptance moves to the C14 chunks.

## Out of scope

- Porting the src/versus hooks (C13b).
- Versus networking, LAN discovery and voice (C14a, C14b, C14c).
- Widescreen output for either image.
- Playing with native PC players. Xbox Live.
