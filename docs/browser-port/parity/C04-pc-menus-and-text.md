# C04: PC menus and text

Class: Simple
Depends on: C01; C06 (hud.cpp element and page draw chain); C05 (action table, key names,
`input::Actions`/`input::Binding`, the guest and host message rings); C03 (input core: `KeyboardActive`, `InMenu`,
`SkipPromptShown`, `WaitForPress`, `game::KeyboardPlayer`, game/console.cpp)
Native sources: src/game/prompts.cpp, src/game/text.cpp, src/game/xbox_live.cpp, src/game/exit_menu.cpp,
src/game/intro.cpp, src/game/loading.cpp, src/game/console.cpp, src/app/console.cpp; reviewed only: src/game/intro.cpp
(ported by C01), src/app/splash.cpp, src/app/handover.cpp, src/app/focus_pause.cpp; hook dependency: src/game/hud.cpp
(ported by C06)

## Goal

Make the front end and HUD read like a PC game in the browser: keyboard key legends in place of pad icons while the
keyboard is in use, PC wording in game text, no Xbox Live items, an Exit item that returns to the host main menu, the PC
loading screen waits and skip keys, and a developer console reachable from the Developer settings. Confirm which native
host features (splash, handover, pause on focus loss, intro skip) the browser already covers or another chunk delivers.

## Native behaviour

- `prompts.cpp`: hooks SetNavigation `sub_002915C1` (prompts.cpp:712-726): remembers each navigation bar (owner, page,
  action string, type) and relabels its buttons with key names when `Keyboard(page)` is true (prompts.cpp:450-454:
  `input::KeyboardActive()` and, in split screen, the keyboard player's own menus). `PromptsTick` (prompts.cpp:689-709)
  calls SetNavigation again for every remembered bar still on a stack (`game::OnStack`, options_menu.cpp) when the
  device changes. Labels come from `Label` (prompts.cpp:157-196) using `Keys` over `input::Actions`/`input::Binding`
  (prompts.cpp:135-155); menus show `ENTER`, `ESC`, and `BACKSPACE` on P_Pause. Per element draws `WordingElement`
  (tutorial and message text through `KeyWording`, prompts.cpp:605-640), `HintElement` ("Press (A) To Continue", "Skip
  Video", prompts.cpp:665-686), `EventPrompts` (cinematic Skip bar, calls `input::SkipPromptShown`, prompts.cpp:517-525)
  and `ComboElement` (combo escape key names) are called from hud.cpp's element draw hook `sub_002C64E0`
  (hud.cpp:638-651); `game::DrawingPage` comes from hud.cpp's page draw hook `sub_002BE990` (hud.cpp:604-632). It
  allocates a guest block with `mem::AllocSystem` (prompts.cpp:198-201).
- `text.cpp`: hooks `Localize` `sub_00070FC9` (text.cpp:146-157): "LAN" for System Link, PC wording for the storage
  messages, the swapped THERMO / NIGHTVISION help, translated through `app::Translation` (translations.cpp).
  `game::KeyWording` (text.cpp:130-144) rewrites pad wording in English tutorial text into bound keys via
  `game::KeyNames` (prompts.cpp:514).
- `xbox_live.cpp`: hides the Live status line (`sub_00291D64`, xbox_live.cpp:50-55), drops the solo menu Live item by
  return address `0x1A67DA` (`sub_002C5160`, xbox_live.cpp:58-62), drops the pause menu Live item (`sub_0018B00D`,
  return `0x18CBC0`, xbox_live.cpp:65-71), relays out the co-op buttons (`sub_001A7137`, xbox_live.cpp:75-104) and
  redirects focus off `B_Online` (`sub_001A6132`, xbox_live.cpp:107-111).
- `exit_menu.cpp`: adds an "Exit" item to `Lst_OfflineOnline` after the page setup `sub_00185C2F`
  (exit_menu.cpp:107-122); the press handler `sub_0018697A` (exit_menu.cpp:127-139) and the back handler `sub_00186B5D`
  (exit_menu.cpp:144-153) open the game's own OK / Cancel box (`kMessageBox 0x29388A`, id `0x7E`, borrowed listener
  vtable `0x546EC8`); `sub_001ACEAB` OnAccept posts `WM_CLOSE` (exit_menu.cpp:157-166); `sub_001AD175` OnCancel clears
  the state. Strings are translated with `app::Translation`.
- `intro.cpp`: with `skip_intros` (settings.cpp:110) the front end PlayVideo `sub_00039AD4` returns 0 for the LogosVideo
  name at `[0x651344]` (intro.cpp:26-36) and ShowScreen `sub_00037274` returns 0 for `esrb.tga` (intro.cpp:39-51).
- `loading.cpp`, in three groups:
  - Input waits: during the mission load (`sub_000444B4`) the launch data writer `sub_0026AB34` draws the full bar and
    calls `input::WaitForPress()` before the relaunch, for every map but `menu` and networked co-op (loading.cpp:83-106,
    `Waits` loading.cpp:76-78). The controller layout screen (`sub_00038C3F`, `sub_000367E4`) is answered by any key
    (loading.cpp:453-466). The movie skip check `sub_0004B46F` also accepts keyboard skip keys and hides a button held
    when the movie started (loading.cpp:477-490). `sub_00038F96` marks movie frames (loading.cpp:494-500).
  - "Press any key" text painted into the 640x480 loading picture with Title_18 glyphs (`sub_002D1BA0`, `sub_00051673`,
    `BeforeShowScreen` loading.cpp:400-422); the glyph page is read back from the renderer's texture.
  - Renderer and relaunch fixes (`sub_002E65A4`, `sub_001CD157`, `BothBuffers`, `sub_004AE0A7`, kept picture through
    `kernel::SavedDataAddress`) that depend on gpu/ and the D3D12 back buffer.
- `game/console.cpp`: `QueueCommand`/`RunCommands` run lines through `UPlayer::Exec` on the input update; output is
  captured by hooking `FOutputDevice::Serialize` `sub_000EE9D1` for the null device `[0x5B57F0]` (console.cpp:41-53,
  83-110). `app/console.cpp` is the ImGui window on the console key (console.cpp:61-103).
- `splash.cpp` shows a logo window before the game window; `handover.cpp` keeps the old process window until the
  relaunched one presents; `focus_pause.cpp` holds guest time and audio while unfocused, gated by `pause_on_focus_loss`
  (focus_pause.cpp:28-46).

## Browser today

- Covered already:
  - Splash: the host front end shows its own loading layer until the first presented frame (menu-shell.mjs:286-299,
    329-338).
  - Handover: relaunch is in process (`MenuLaunch` / `MenuLaunchRequested`, menu-launch.h:9-31) and the display is kept
    across restarts (`StoryState::display`, boot-wasm.cpp:65).
  - Focus pause: missions pause on blur or hidden tab through `mission-controls.mjs:24-39` and words 2 to 4 consumed by
    `MissionInput`. There is no setting and the front end (menus before a mission) does not pause.
  - Development start already skips every video through `MissionInput::Dispatch` id 135 (mission-input.cpp:189-196).
  - Skip intros: C01 ports intro.cpp as its settings proof (rename `00039AD4`, `skipIntros` setting, `Game` tab) and
    makes `pauseOnFocusLoss` a setting. Nothing left for C04.
- Conflicts with native hooks:
  - `sub_0018697A` is already overridden by `MenuRoute` (menu-route.cpp:5-8, 214-260), which rejects Coop and Versus
    (selection 1 and 2). exit_menu.cpp cannot link next to it.
  - exit_menu.cpp includes windows.h and posts `WM_CLOSE`; it must be ported, not compiled.
- No browser hook draws per Magma element or page today. C06 compiles hud.cpp (`sub_002BE990`, `sub_002C64E0`) and links
  pass-through stubs for `WordingElement`, `HintElement`, `EventPrompts`, `ComboElement` until prompts.cpp lands;
  hud.cpp also calls `game::loading::BeforeShowScreen`, `AfterShowScreen`, `BothBuffers`, `g_movie`.
- The host pause panel already has "Quit to main menu" calling `controller.stop()` (menu-shell.mjs:275-279,
  menu.html:163).
- Options tabs: Display, Audio, Controls, Developer (menu-shell.mjs:10-28); Developer has development start, frame rate,
  diagnostics.

### Decisions

- Exit: keep the game's own item and OK / Cancel box; OK asks the host to quit to its main menu, exactly as the pause
  panel's Quit does. The browser never closes the tab.
- prompts.cpp plugs into C06's hud.cpp chain by replacing C06's stubs; C04 adds no draw hook of its own.
- loading.cpp is not compiled whole (it needs gpu/backend.h, gpu/texture.h and the D3D back buffer). C04 ports only its
  input wait hooks into a browser file, including `sub_00038F96`, which also sets `game::loading::g_movie` (an inline
  thread_local in loading.h:18) that hud.cpp reads. `sub_00051673`, `BeforeShowScreen`, `AfterShowScreen` and
  `BothBuffers` stay with C06 (C06 task 7 names this chunk as the loading-screen chunk; agree that C06 builds its
  minimal file). The Press Any Key painting and the renderer group stay unported.

## Tasks

1. Remove C06's pass-through stubs for `WordingElement`, `HintElement`, `EventPrompts`, `ComboElement` once prompts.cpp
   is selected. Verify through C06's chain that the original element draw still runs exactly once for every element
   prompts.cpp does not handle.
2. Add prompts, text and xbox_live to C01's `BROWSER_GAME_HOOKS` and `src/app/translations.cpp` to the hook-compat
   sources (allowlist `app/translations.h`, `game/magma.h`, `core/input.h`); delete the `game::PromptsTick` stub from
   C03's game-stubs.cpp. C01's shim must provide `app::BootLanguage()`; until a browser language source exists it
   returns "int".
3. Port exit_menu.cpp to `src/browser/exit-menu.cpp`: same constants and strings, the page setup, back, OnAccept and
   OnCancel hooks; fold its press handler check into `MenuRoute::Dispatch` (after the existing Coop / Versus rejection)
   instead of a second `sub_0018697A`. OnAccept raises a "quit to main menu" host request instead of `WM_CLOSE`.
4. Host request: a `quit` message on C05's guest to host ring (C01 has no reverse channel, C01 Risks) that
   menu-shell.mjs turns into `quit()`. Unit test the shell side.
5. Loading waits: port the input wait group of loading.cpp (`sub_000444B4`, `sub_0026AB34`, `sub_00038C3F`,
   `sub_000367E4`, `sub_0004B46F`, `sub_00038F96`) into `src/browser/loading-input.cpp`; none of these addresses is
   overridden in the browser today (loading-io.cpp only owns `sub_004B8EC0`). `WaitForPress` in the browser input core
   polls the shared key bitmap and gamepad sample with `Runtime::Checkpoint` between polls so Stop still cancels. Skip
   `FullBar` (renderer specific) unless the WebGPU backend can draw it.
6. Developer console: a DOM overlay in menu.html styled like the options panel, opened by the `console` action key (C05
   table, default Backtick) only when the `console` setting is on. Lines go host to guest on C05's host to guest ring
   into `game::QueueCommand(line, Route::Player)`; `game::TakeOutput()` lines come back on the guest to host ring and
   are shown. game/console.cpp itself is compiled by C03. History with Up and Down as app/console.cpp:35-45. While open,
   keyboard input to the game is blocked (`input::SetBlocked` equivalent in C03's core).

## Settings and UI

- `skipIntros` and `pauseOnFocusLoss` and the `Game` tab come from C01; C04 adds none of them.
- Developer tab: `console`: `{ initial: false }`, row "Console". Host only; when on, the console key opens the overlay.
- Keyboard legends have no setting: they follow the last used device (`KeyboardActive`), as native.

## Acceptance criteria

- With the keyboard last used, the main menu, pause menu, OPSAT, message boxes and the in-game cinematic Skip bar show
  key names (for example "(ENTER) Select", "Skip (ESC)"); pressing a gamepad button switches them back to icons on the
  next update, and back again on a key press.
- English tutorial texts name bound keys; a rebind in C05 shows up in the next text drawn.
- No Xbox Live line, solo Live item or pause Live item appears; the co-op button text reads LAN.
- The game mode list ends with Exit; Exit or Back on that page asks "Quit Game"; OK returns to the host main menu with
  no error screen; Cancel keeps the selection.
- Mission loading waits for a key on the loading picture; any key answers the controller screen; Enter, Space or Escape
  skip movies; a key held when a movie starts does not skip it.
- With Console on, Backtick opens the console; a line is echoed, and an unknown command prints "unrecognized command:
  ..." (console.cpp:108) back into the overlay.

## Testing

- Unit tests first: settings.test.mjs for the `console` setting; a shell test that a quit host request returns to the
  title like the pause panel; console overlay history and blocking.
- Native and Wasm contract tests for `MenuRoute::Dispatch` with the exit index (extend tests/browser/menu-route.cpp):
  selection 1 and 2 still rejected, exit index raises the request, other selections untouched.
- Contract test for loading-input.cpp: the movie skip check hides a button held on the first call and accepts a skip key
  released later; `WaitForPress` returns on a press and release and unwinds on runtime Stop.
- Build in a separate directory (for example out/parity-c04), never out/boot-m5.
- agent-browser with out/front-end/mute.js: screenshot the title and game mode list with keyboard legends, press a
  gamepad button (if the harness can) or check the legend change through the state report, pick Exit and confirm the
  host title, start a mission and screenshot the loading wait, open the console from the Developer setting.

## Risks and open questions

- C06 is Complex and may land after C04 is ready; until then prompts.cpp cannot draw legends (SetNavigation relabelling
  in `sub_002915C1` still works without hud.cpp). Do not add a second `sub_002C64E0` override as a shortcut.
- The Press Any Key text needs Title_18 glyph texels read from the renderer (loading.cpp `KeepPage`); the WebGPU backend
  may not expose them. Fallback: a host overlay line during the wait, or no text.
- `WaitForPress` blocks the guest worker; make sure the host still presents the loading picture while blocked and that
  the stepper's backlog logic does not count the wait.
- The guest and host rings are designed in C05; if C05 slips, a single quit bit in C03's guest flags word is enough for
  Exit, and the console waits.
- Which language does the browser kernel report to the game, and should `app::BootLanguage()` follow it?
- `mem::AllocSystem` is used by prompts.cpp, exit_menu.cpp and name_entry.cpp: confirm C01's helpers provide it.

## Out of scope

- The renderer group of loading.cpp (bar into both buffers, kept picture reload, 640x480 copy).
- Widescreen HUD placement from hud.cpp.
- Host splash image and process handover (not applicable in a browser).
- Key rebinding and in-game RECOMP SETTINGS pages (C05).

## Implementation notes

What C04 built.

- `prompts`, `text` and `xbox_live` are in the default `BROWSER_GAME_HOOKS`; their weak stand-ins (PromptsTick,
  EventPrompts, ComboElement, HintElement, WordingElement) and the ProjectModel one are gone from game-stubs.cpp, which
  keeps only `app::DrawFps` and `app::DrawWatermark`. `app::BootLanguage()` is in host-settings.cpp (English until C12).
- `src/browser/exit-menu.cpp` ports exit_menu.cpp (page setup, back, OnAccept, OnCancel; the same constants and
  strings). Its press check is `browser::exit_menu::Press`, which MenuRoute's `sub_0018697A` asks before the
  Coop / Versus route (a weak `false` keeps menu-route.cpp building alone). OnAccept posts a `quit` entry on the guest
  to host ring; menu-shell turns it into the pause panel's Quit to main menu.
- `src/browser/loading-input.cpp` ports the input wait group of loading.cpp: the mission loading wait (with the full
  bar, polling `input::PressReleased` with a runtime checkpoint between polls, so a stop unwinds it), the controller
  layout screen, the movie skip check with keys and the held button guard, and movie frames (`g_movie`).
  `MarkPersistedDisplay` and the Press Any Key text are not ported.
- Console: the `console` setting (Developer tab) lets the `console` binding (Backtick) open a DOM overlay over the
  game. Lines go to the guest as `console` entries; `src/browser/input-update.cpp` queues them with
  `game::QueueCommand(line, Route::Player)` before mouse.cpp's update runs them, and sends `game::TakeOutput()` lines
  back as `output` entries. The overlay keeps a bounded log and Up / Down history (`src/browser/console-model.mjs`,
  `tests/browser/console-model.test.mjs`, which also covers the quit and output routing); the game gets no keys while
  it is open.

### Checked in the browser (out/parity)

- Title: "PLEASE PRESS (ENTER) TO BEGIN" (text.cpp).
- Game mode list: Solo, Coop, Versus, Credits, Exit with "(ENTER) Select (ESC) Back" (prompts.cpp, exit-menu.cpp).
- Console in a mission: `nonsensecommand` printed "unrecognized command: nonsensecommand" in the overlay.

### Not checked

- Exit's OK returning to the page's main menu: automation kept landing on the attract movie, which starts while the
  title waits, so the box was not reached. The routing is unit tested.
- The mission loading wait (development start skips the front end), the controller screen, movie skipping with keys,
  and the pad / keyboard legend switch need the user's playtest.

### Static lifetime

| Static | Restart safe because |
| --- | --- |
| exit-menu `g_block`, `g_list`, `g_exit_index` | `g_block` is in the hook pool, which keeps its bytes; the list is found again by address and refilled |
| loading-input `g_mission_load`, `g_held_at_start` | Set around one call; a held bit clears on the next release |
| prompts.cpp bars and block | The block is in the hook pool; bars are matched against the stacks every tick |
