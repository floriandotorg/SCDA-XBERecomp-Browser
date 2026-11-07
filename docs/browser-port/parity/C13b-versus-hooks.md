# C13b: Versus hooks (src/versus) in the browser

Class: Complex
Depends on: C01 (hook foundation: memory helpers, symbol macros, app::Settings shim, host-settings channel, selected hook source compilation), C13a (versus image builds and boots in the browser). Tasks 6 to 10 also need C03 (browser input core implementing `input::`, src/browser/input-core.cpp) and C05 task 1 (action table and key names, `input::Actions` and `input::Binding`). Task 4 may use C06's `d3d::State()` and `d3d::Lock()` shims. Task 11 waits on C05 (settings pages) and C06 (widescreen output).
Native sources: src/versus/*.cpp, src/xdk/hle.h, src/game/magma.h, src/app/app.h, src/app/menu.cpp, src/core/input.cpp, src/app/fps_counter.cpp

## Goal

The versus runtime in the browser carries the native PC port's versus fixes and mouse and keyboard support: full detail meshes, the grenade floor fix, short profile boxes, no 31 fps sleep, frame rate independent camera shake, a working optic camo projection, LAN in place of Xbox Live, keyboard name entry, mouse look and menu cursor, and key names in prompts. Hooks that rely on native only features (the F10 settings rows, widescreen output) are ported only once the browser has those features. They are listed here with what blocks them.

## Native behaviour (cite file:line)

Mechanism
- Each image compiles `src/<game_dir>/*.cpp` as hooks (CMakeLists.txt:124). A hook defines `sub_X`, which replaces the weak alias of `__imp__sub_X`, and calls `__imp__sub_X` for the original (for example src/versus/mesh_lod.cpp:14-23). Versus addresses come from generated/versus/image_symbols.h (`IMAGE_XBE "sc4_adv.xbe"`). Versus has 64 partitions and 14,975 functions; story has 128 partitions. The two images reuse `sub_` names for unrelated code, so a versus hook must never be compiled into the story module, and the reverse.

Pure guest memory hooks
- src/versus/mesh_lod.cpp:14 `sub_000CC710` (skeletal mesh draw): actor LOD bias at +0x1F4 set to 1e6 for the call.
- src/versus/grenade_bounce.cpp:27 `sub_0010BBE0` (trace in the grenade HitWall, return 0x3C23A): a grenade that hits the floor at a shallow angle no longer explodes early.
- src/versus/profile_boxes.cpp:29 `sub_001A06F0`: profile menu boxes are held 1.0 s instead of 3.5 s (+0xFC start, +0x120 box up).
- src/versus/frame_rate.cpp:14 `sub_001E4C90`: device refresh at +0x46D8 is set to 0x7FFFFFFF around present, which removes the 31 fps sleep. Native then caps versus at 240 fps (src/app/app.h:18-19, src/app/main.cpp:279).
- src/versus/camera_shake.cpp:80 `sub_0004E500`: EPlayerCam effects at +0x1958 step at 30 Hz on game time, interpolated per frame.
- src/versus/console.cpp: empty `game::` console stubs.

Renderer coupled hook
- src/versus/projection.cpp:12 `sub_00282B30` (`D3DDevice_GetProjectionViewportMatrix`, config/xdk_symbols_adv.toml:88). Native replaces the XDK SetTransform and SetViewport, so the device's matrix copy stays zero and the optic camo spy pass draws nothing. The hook rebuilds projection times viewport from `d3d::State()` (constants c-38 and c-37, minus the 0.53125 screen offset), projection.cpp:12-31.

Menu text hooks
- src/versus/xbox_live.cpp:166-221 (`sub_00157A90` localization find, `sub_00192390`, `sub_001A71C0`, `sub_001A72F0`, `sub_001C4910`): removes the Live sign in status and button, renames System Link to LAN and the Live options page to Voice. It calls `versus::OptionsText` (options_menu.cpp) and `versus::KeyText` (prompts.cpp).

Input coupled hooks
- src/versus/name_entry.cpp:69-82 (`sub_001ADE90`, `sub_001AE0A0`, `sub_001AE2F0`; append 0x1AE800, delete 0x1AE8E0): typing on P_VirtualKeyboard.
- src/versus/mouse.cpp:563-614: menu cursor and wheel in `sub_00188E80`, mouse look after the native PlayerInput `sub_0003FB80` (look rates at +0x4D8 and +0x4DC, full stick 1000), binocular zoom on the wheel. Sensitivity is per view: free look, weapon raised, sniper (mouse.cpp:120-135; settings src/app/app.h:58-61).
- src/versus/prompts.cpp:378-474 (`sub_002690E0`, `sub_00263C30`, `sub_0014C190`): key names in pad prompts.
- `input::` calls made by these files: TakeMouse, Cursor, Clicks, Pressing, TakeWheel, TakeMenuWheel, TapButtons, SetView, SetMenu, EventKeyHeld(Reload), TakeChars, SetTextEntry, KeyboardActive, Actions, Binding. `app::` calls: Get, Narrow, Aspect, Widescreen, FovScale, Pace, DrawFps, DrawWatermark, Translate, Translation, TitleCase, SettingsRevision, rows::*.
- Versus key bindings and walk or run speeds live in the native input layer (src/core/input.cpp:164-170, 233-260).

Native feature coupled hooks
- src/versus/options_menu.cpp (980 lines): splices RECOMP SETTINGS, MOUSE AND KEYBOARD and CONTROLLER pages into the Magma XML and drives them from the native F10 rows (`app::rows`).
- src/versus/widescreen.cpp:208-363 (`sub_0011B720`, `sub_0025A3F0`, `sub_002620A0`, `sub_00195FC0`, `sub_00252B80`, `sub_001231E0`, `sub_00011BE0`): Magma 2D widening, splash flags in `gpu::` globals, FPS overlay and watermark.
- src/versus/communicator.cpp: Communicator hooks, ported in C14c.

## Browser today (cite file:line; where the work plugs in)

- The browser build compiles no src/game, src/versus or src/app sources. Its hooks live in src/browser and use their own macros, for example `ML_GUEST` and `ML_ORIGINAL` (src/browser/menu-launch.cpp:8-12). C01 adds the mechanism to compile selected native hook sources with browser memory helpers and an app::Settings shim. Versus hooks plug in there, compiled only into the versus module from C13a.
- Graphics: the browser has its own SetTransform and SetViewport boundaries (src/browser/diagnostic-graphics.cpp:169-180, `DG_BOUNDARY`). Whether they still run the XDK originals decides whether projection.cpp is needed.
- Input: keyboard and pad go through src/browser/menu-input.cpp and menu-input.mjs, with mission mouse look in src/browser/mission-input.cpp, which is story only and hard coded to story addresses (mission-input.cpp:9-29, 238-247). No versus input path exists.
- Frame: the browser presents a fixed 640 by 480 4:3 frame (src/browser/menu-shell.mjs:8). There is no widescreen and no native F10 panel; settings come from src/browser/settings.mjs:1-10.
- Menu blockers handled by C13a: menu-route.cpp:250 swallows the Versus press and menu-launch.cpp:106 and 159 refuse sc4_adv.xbe. This chunk assumes C13a has lifted them.

## Tasks (numbered, concrete, each verifiable)

1. Register a versus hook source list with the C01 mechanism, compiled only when `BROWSER_IMAGE=versus` (C13a). Add a build check that fails if a src/versus source is ever linked into the story module.
2. Port mesh_lod.cpp, grenade_bounce.cpp, profile_boxes.cpp, camera_shake.cpp and console.cpp unchanged apart from C01 includes. Verify each hook runs (a counter in a debug build) and that the profile box closes after about 1 s.
3. Port frame_rate.cpp and decide versus pacing: keep the browser present pacing and add a versus cap equivalent to 240 fps if the browser can exceed it. Verify with the FPS counter that the menu is no longer held at 31 fps.
4. projection.cpp: first check whether the browser SetTransform and SetViewport boundaries call the XDK originals. If they do, read the device matrix and skip the hook. If they do not, build the matrix from the browser graphics state (C06's `d3d::State()` shim if available) with the same math (projection times viewport, 0.53125 offset removed). Verify the optic camo spy pass renders in a versus map with a screenshot comparison.
5. Port xbox_live.cpp. Until options_menu.cpp and prompts.cpp are ported, provide browser versions of `versus::OptionsText` and `versus::KeyText` that return the stock text pointer. Verify the versus main menu shows LAN and no Xbox Live button, and the options page reads Voice.
6. Versus input path: use the browser input core from C03 (src/browser/input-core.cpp) for the `input::` calls listed above, and add whatever versus needs that story does not (the versus key set, binocular view, menu wheel). If C03 has not landed, stop here and do tasks 1 to 5 only. Write unit tests first for any new JS side pieces (wheel notches, click counting, text entry queue).
7. Port name_entry.cpp on top of task 6. Verify typing a profile name on P_VirtualKeyboard, Backspace deletes, Enter accepts.
8. Port mouse.cpp on top of task 6, with three new settings for the view sensitivities. Verify mouse look in a versus map, the menu cursor on menu pages and binocular zoom on the wheel.
9. Add the versus action set (src/core/input.cpp:233-260) to the C05 action table, active only while versus runs, and show it on the Controls tab.
10. Port prompts.cpp once task 9 gives a binding table. Verify a prompt shows the key name while the keyboard is active and the pad icon once a pad is used.
11. Record the blocked hooks: options_menu.cpp waits for the settings rows from C05; widescreen.cpp waits for the widescreen output from C06 (which already plans for versus centred menus). Add a short note in this chunk's evidence saying which chunk unblocks each.

## Settings and UI (settings.mjs entries, Options panel / main menu changes)

- `freeLookSensitivity`, `weaponSensitivity`, `sniperSensitivity`: number, 0.1 to 5, step 0.1, initial 1 (src/app/app.h:58-61). Shown on the Controls tab while the versus image runs, in place of `sensitivity`.
- Controls tab key list: the versus bindings while versus runs.
- No main menu change.

## Acceptance criteria

- The versus module contains the ported hooks and the story module contains none of them.
- Profile boxes close after about 1 s. The menu frame rate is no longer limited to 31 fps. Grenades that land at a shallow angle bounce instead of exploding at once.
- The optic camo spy effect renders in a versus map.
- The versus menus show LAN instead of Xbox Live, and the Live options page reads Voice.
- Keyboard name entry, mouse look with the three sensitivities, and the menu cursor work in versus.
- Prompts show key names when the keyboard is active.

## Testing

- Unit tests first for the JS input pieces (wheel, clicks, text queue, binding lookup).
- Native Wasm tests for pure hook logic that can be isolated (camera shake stepping, profile box timing) with a fake guest memory.
- Browser: agent-browser CLI on a separate served build dir (never port 8125 or out/boot-m5), audio muted via out/front-end/mute.js. Screenshots of the main menu (LAN), the options page (Voice), the name entry and a map with optic camo. Use FPS counter samples for the frame rate check.

## Risks and open questions

- Whether the browser graphics boundaries keep the XDK device copies of the transforms is unknown; it decides task 4.
- Can a versus map be reached offline? If not, the map level checks (camo, grenades, mouse look) wait for C14a networking or need a two tab LAN session.
- mouse.cpp and prompts.cpp are large (614 and 496 lines) and assume the native input layer. Porting them may reveal input features the browser lacks (key events with hold state, view modes).
- camera_shake.cpp assumes the game time it reads advances smoothly; browser frame pacing differs and needs checking.

## Out of scope

- options_menu.cpp and widescreen.cpp until their browser prerequisites exist.
- src/versus/communicator.cpp (C14c).
- Versus FPS overlay layout (src/app/fps_counter.cpp).
- Building, identifying and switching to the versus image (C13a).

## Implementation notes

What C13b built.

- `BROWSER_VERSUS_HOOKS` defaults to camera_shake, console, frame_rate, grenade_bounce, mesh_lod, mouse, name_entry,
  options_menu, profile_boxes, projection, prompts, widescreen and xbox_live (all of src/versus but communicator.cpp,
  C14c), compiled unchanged through the hook compatibility layer into the versus module only; crt_math joins them.
  `hookSource(root, name, image)` looks in src/versus (versus) or src/game (story), then src/xdk, so neither image can
  take the other's hooks (tests/browser/inventory.test.mjs). The versus hook manifest lists 25 guest addresses, all at
  versus addresses (for example mesh_lod 000CC710, frame_rate 001E4C90, projection 00282B30, mouse 0003FB80 and
  00188E80, name_entry 001ADE90, 001AE0A0, 001AE2F0).
- The prerequisites the chunk waited on exist now: `d3d::State()` (projection.cpp), the browser input core for the
  `input::` calls (mouse, name entry, prompts), `app::rows` for options_menu.cpp, and the splash globals widescreen.cpp
  sets (loading-screen.cpp joins the build with it).
- The browser input core takes native's versus action and pad tables (`kVersusActions`, `kVersusPadActions`) in the
  versus module, so versus binds, walks or runs and reloads as native.
- The three view sensitivities are the existing `sensitivity`, `weaponSensitivity` and `sniperSensitivity` settings,
  which host settings already carry to `free_look_sensitivity`, `weapon_sensitivity` and `sniper_sensitivity`.

### Checked

- out/versus-boot builds and links with every hook; the versus runtime starts with them and renders its intro.

### Not done

- None of the in-game checks (profile boxes, frame rate, grenades, optic camo, LAN text, Voice page, name entry, mouse
  look, prompts) could be made: the versus runtime stops at its device re-create before the main menu (C13a, Not
  done). The Controls tab still lists the story actions while versus runs, and the hook contract tests on a fake guest
  are not written.
