# C03: Keyboard and mouse gameplay

Class: Simple
Depends on: C01; C05 task 1 (the action and key name table) must exist first, see Risks; C06 for `game::ProjectModel`
(keypad mouse picking only)
Native sources: src/game/mouse.cpp, src/game/mouse_pitch.cpp, src/game/views.cpp, src/game/keypad.cpp,
src/game/quick_save.cpp, src/game/opsat.cpp, src/game/interaction_list.cpp, src/game/analog_keys.cpp,
src/game/name_entry.cpp; pulled in by them: src/game/whistle.cpp, src/game/unreal.cpp, src/game/console.cpp (command
queue only), `game::ProjectModel` from src/game/fov.cpp (ported by C06); logic reference: src/core/input.cpp

## Goal

Make the story game play on keyboard and mouse the way the native PC port does: mouse buttons and wheel bound to
actions, menus driven by a free cursor, wheel zoom for EEV, sniper scope, sticky camera and OPSAT map, walking speed
steps, numpad keypads, quick save and load, the OPSAT key closing the OPSAT, typed profile names, and per view mouse
sensitivity (free look, weapon, sniper). The native game hooks are compiled unchanged on top of C01; the browser
supplies its own implementation of `src/core/input.h`.

## Native behaviour

- `mouse.cpp` is the hub. It hooks the viewport input update `sub_0004B525` (mouse.cpp:306-370) and, after the original
  runs, decides whether a Magma menu has the input (`Menu`, mouse.cpp:196-252: hover focuses, list rows select through
  `kListHitTest 0x2C1190` / `kListSelect 0x2C2040`, clicks tap `IK_Joy A 0xCA`, list arrows tap `IK_AnalogUp/Down
  0xC4/0xC5`), steps menus and the focused interaction list with the wheel (mouse.cpp:333-337), publishes page flags to
  the input core (`SetTitleScreen`, `SetPauseMenu`, `SetMap`, `SetOpsat`, `SetQuickInventory`, `SetTrainingVideo`,
  mouse.cpp:338-349), and sends mouse counts as `IK_MouseX 0xE4` / `IK_MouseY 0xE5` axis events through engine vtable
  +0x78 (mouse.cpp:111-115, 355-361). It then runs `UpdateView`, `MenuTick`, `PromptsTick`, `NameEntryTick`,
  `WhistleKeys`, `KeypadKeys`, `QuickSaveKeys` and queued commands (`Reload`, `Interact` for drop body,
  mouse.cpp:365-368). It also hooks `sub_001853BE` to find the controller message boxes (mouse.cpp:299-303). Cursor to
  page mapping uses `app::Aspect()` and `app::Narrow()` (mouse.cpp:178-191). Synced co-op paths (`PadWheel`,
  `coop_mouse.cpp`) are net only.
- `mouse_pitch.cpp` hooks EPlayerCam rotation `sub_000FD37E` (mouse_pitch.cpp:20-32): while `aMouseX`/`aMouseY`
  (+0x390/+0x394) moved, pitch speed +0x304 is temporarily set to yaw speed +0x300 so vertical mouse look is not 0.625x
  slower.
- `views.cpp` has no hook. `game::UpdateView` (views.cpp:127-169) classifies the player controller state into
  `input::View` (Normal, Eev, ScriptedEvent, Ledge, Lockpick, BackOut, Keypad, StickyCamera with its `ZoomSpeed`) and
  sets targeting, carrying, hanging, ladder and pipe flags. In EEV and sniper states the wheel sets a target zoom level
  and the left thumb is tapped until reached (`Zoom`, views.cpp:34-63; `Sniping` list views.cpp:84-91, `Targeting` list
  views.cpp:93-102).
- `keypad.cpp`: `KeypadKeys` (keypad.cpp:144-150) presses numpad keys on an in-use keypad through `kKeyPushed 0x14AE47`
  / `kGlowSelected 0x14AD42`; mouse hover and click pick the button under the cursor by projecting each button's key top
  with `game::ProjectModel` (keypad.cpp:68-138, fov.cpp:144-151). Hooks `sub_0014AE47` (guard against index -1,
  keypad.cpp:155-163) and `sub_001661F2` (first move lands on 5, keypad.cpp:167-187).
- `quick_save.cpp`: `QuickSaveKeys` (quick_save.cpp:39-72) runs the pause menu's save (`kSave 0x18A40D`, slot 0, after a
  frame capture through `[0x6500FC]` vtable +0x24) or the load of the quick slot (`kLoad 0x18A185`); refused in co-op or
  when `[0x5B5AC8] == 2`.
- `opsat.cpp`: hooks the COpsatMenu input handler `sub_00199620` so the `OpsatMenu` action closes the whole OPSAT
  (`kClose 0x19A1B0`, opsat.cpp:35-47) and `sub_00194C19` to remember the goals menu; `GoalsTab` lets a click pick a
  P_Goals tab (opsat.cpp:60-73).
- `interaction_list.cpp`: hooks the list focus `sub_001B9755` (interaction_list.cpp:29-37) so mouse.cpp knows when the
  wheel should step the interaction list.
- `analog_keys.cpp`: hooks `sub_0004AF95` (analog_keys.cpp:17-24) so a held move key counts as a full stick for the
  `IK_Analog*` selector keys.
- `name_entry.cpp`: on `P_VirtualKeyboard` typed letters and digits go into the name (`kAppend 0x370F1`, `kDelete
  0x1C6F3F`, `kAddSpace 0x1C7007`), tracking keyboards through the constructor and destructor hooks `sub_001C6511` /
  `sub_001C65E7` (name_entry.cpp:77-111).
- `whistle.cpp` (`WhistleKeys`) and `console.cpp` (`QueueCommand` / `RunCommands`, console.cpp:71-110 plus the output
  hook `sub_000EE9D1`) are called from mouse.cpp; `unreal.cpp` resolves states and properties by name for views, keypad
  and whistle.
- The context sensitive key mapping is not in the game hooks but in `src/core/input.cpp`: action table
  (input.cpp:199-231), remaps for sticky camera, menus, OPSAT and map (input.cpp:113-126, 586-593), walking speed steps
  and wheel routing (`kSpeeds` input.cpp:161, `Notch` input.cpp:1006-1019, `UpdateStick` input.cpp:708-742, `WheelZoom`
  input.cpp:663-681), menu keys (`PressMenuKeys` input.cpp:621-635), keypad numpad table (input.cpp:1181-1195), typed
  characters (input.cpp:1119-1135).
- Settings read: `mouse_sensitivity`, `invert_mouse` (input.cpp:1097-1107, settings.cpp:105-106). The three view
  sensitivities `free_look_sensitivity`, `weapon_sensitivity`, `sniper_sensitivity` are versus only in native
  (app.h:58-61, versus/mouse.cpp:126-135); story uses one value.

## Browser today

- `src/browser/mission-input.cpp:9-17` already overrides `sub_0004B525` and routes it to `MissionInput::Dispatch` id
  132, which sends its own `IK_MouseX/Y` axis events from shared words 0 and 1 divided by 1000
  (mission-input.cpp:199-249) and skips them when a non HUD page has focus (mission-input.cpp:235). Native mouse.cpp
  defines the same symbol, so the two must be merged, not linked side by side. It is only compiled with
  `BROWSER_MISSION` (tools/browser/CMakeLists.txt:224).
- `src/browser/mission-controls.mjs` owns pointer lock on canvas click (mission-controls.mjs:40-48), applies
  `sensitivity` and `invertY` in JS (mission-controls.mjs:63-69), and pauses the guest on blur or hidden tab through
  words 2 to 4.
- `src/browser/menu-input.mjs` maps a fixed table of `KeyboardEvent.code` values to the 20 bit `MENU_INPUT` mask
  (menu-input.mjs:1-25); `src/browser/menu-input.cpp:152-174` turns mask plus gamepad into the XInput sample for port 0.
  No mouse buttons, wheel, numpad, function keys, Ctrl or typed characters reach the guest. `boot-session.mjs:223` and
  `menu-runtime.mjs:118` reject any input span other than 2 words.
- No browser file implements `core/input.h`; none of the C03 hooks exist in the browser.

### Decision: what replaces what

- Mouse look: native `mouse.cpp` axis events replace `MissionInput::Dispatch` id 132's axis code
  (mission-input.cpp:200-249). mouse.cpp is compiled with C01's rename chaining (`sub_0004B525=game_sub_0004B525`, C01
  task 5, already in its rename table). The browser keeps the only `X86_FUNC(sub_0004B525)`, moved out of
  mission-input.cpp into a file compiled in every menu build (the front end menus need the cursor too); it calls
  `game_sub_0004B525` (original update, then the native body) and, under `BROWSER_MISSION`, the existing
  `ObserveSdkEntry(..., 132)` for `ObserveSimulation` only. Pause words 2 to 4 and the stepping logic stay as they are.
- Sensitivity and invert move from JS into the browser input core's `TakeMouse`, so the view class can pick the
  multiplier. `mission-controls.mjs` keeps pointer lock, pause and raw accumulation only.
- Keys: `menu-input.mjs`'s static table is replaced by a raw producer (held keys by key index, mouse buttons, wheel,
  cursor, clicks, characters). The mapping to the pad moves into a browser port of `input.cpp`'s platform neutral parts,
  so the native game hooks get the `input::` API they expect.
- Menus: the cursor is free (pointer lock released) whenever the core reports menu or pointer mode, as native
  `SetMenu`/`SetPointer` free the cursor (input.cpp:1137-1156). In play the canvas click locks again.

## Tasks

1. Add `src/browser/input-core.cpp` implementing every `input::` function the C03 to C05 game files call (list from
   input.h:34-128 and the grep of `input::` in the ported files). Port `Keyboard`, `ActionHeld`, `ApplyEventKey`,
   `RemapTarget`, `LatchTarget`, `PressTarget`, `PressMenuKeys`, `StickSpeed`, `WheelZoom`, `DiagonalSpeed`,
   `PickStick`, `UpdateStick`, `Notch`, `TakeKeypadButton`, `TapButtons` from input.cpp with `Down(vk)` replaced by a
   lookup in the shared held key bitmap. Story table only (no versus branch). `KeyboardPort()` returns 0. Verify: native
   and Wasm contract test (pattern of tests/browser/menu-route.test.mjs) feeding key bitmaps and view states and
   checking the composed `Pad`.
2. Raw input span: extend `menu-input.mjs` (or replace with `keyboard-input.mjs`) to publish a frozen `KEYBOARD_INPUT`
   layout: change counter; held key bitmap of 256 bits indexed by Windows virtual key code, translated from
   `KeyboardEvent.code` through C05's `KEY_NAMES` (mouse buttons as `VK_LBUTTON`, `VK_RBUTTON`, `VK_MBUTTON`,
   `VK_XBUTTON1`, `VK_XBUTTON2` as in input.cpp:364), so `Down(vk)`, `PressedKey` and options_menu.cpp's Escape `0x1B`
   port unchanged; signed wheel notch accumulator; click counter; cursor x and y in 640x480 frame pixels plus a valid
   bit; last device word (keyboard or pad, written by both producers for `KeyboardActive`); a bounded UTF-16 character
   ring (64 units as input.cpp:1129); and one guest written flags word (menu, pointer, text entry, map drag). Mirror it
   with a `static_assert`ed C++ struct. Update `image_input_words` (boot-wasm.cpp:628-633), `readInputDescriptor`
   (boot-session.mjs:223) and `menu-runtime.mjs:118`.
3. Wheel: `wheel` listener on the stage, non passive, `preventDefault`; convert `deltaY` per `deltaMode` into notches
   (accumulate pixel deltas, one notch per 100 px or 3 lines, up positive). Unit test the accumulator first.
4. Mouse buttons: `mousedown`/`mouseup` buttons 0 to 4 into the bitmap; suppress `contextmenu` and `auxclick` on the
   canvas; count a click only for a press and release both seen while the guest flags say menu or pointer
   (input.cpp:1084-1088).
5. Cursor: while the flags word says menu or pointer, release pointer lock, show the cursor over the canvas and publish
   canvas relative coordinates scaled to 640x480 (works for fit, integer and stretch scaling since the guest frame is
   always 640x480). C01's `app::Aspect()` must return 4/3 and `app::Narrow()` 1 so mouse.cpp:178-191 and
   keypad.cpp:110-120 map exactly.
6. Characters: `keydown` with a printable `key` (and Backspace as `\b`) goes into the ring only while the text entry
   flag is set; those keys then press nothing else (input.cpp:558-559).
7. Merge the hook as decided above: delete the axis event code from `MissionInput::Dispatch` id 132
   (mission-input.cpp:200-249), keep `ObserveSimulation`, and route `sub_0004B525` through `game_sub_0004B525`. Contract
   test: one axis event per axis per update, none while `(*storage_)[2]` is set.
8. Add to C01's `BROWSER_GAME_HOOKS`: mouse, mouse_pitch, views, keypad, quick_save, opsat, interaction_list,
   analog_keys, name_entry, whistle, console (and unreal unless C06 already added it). Extend C01's include allowlist
   with `core/input.h` (the real header, implemented by input-core.cpp), `game/console.h` and `game/unreal.h`. C01's
   inventory hashes each selected source.
9. Keypad projection: C06 compiles fov.cpp and keeps `game::ProjectModel` (fov.cpp:133-137, 144-151) exported. Until C06
   lands, link a stub `ProjectModel` that returns false: numpad keys still work, mouse picking of keypad buttons does
   nothing.
10. Link stubs: add `src/browser/game-stubs.cpp` with no-op `game::MenuTick`, `game::MenuPlace`, `game::BlankRow` (C05,
    options_menu.cpp) and `game::PromptsTick` (C04). Each later chunk deletes its stub. Verify the story link has no
    unresolved `game::` symbols.
11. Per view sensitivity in `TakeMouse`: sniper multiplier while the view is `Eev` (EEV and every `Sniping` state,
    views.cpp:133-134), weapon while `SetTargeting(true)`, free look otherwise; then invert Y; then scale. Values come
    from the C01 settings channel. `mission-controls.mjs` stops scaling (its `sensitivity`/`invertY` options are
    removed, tests updated).
12. Escape in play: the browser exits pointer lock on Escape itself. When the lock is lost while in play (not menu, not
    host panel, window focused), tap Start once so the game's own pause menu opens, matching native `pause = Escape`
    (input.cpp:226). Verify with agent-browser.
13. Try `requestPointerLock({ unadjustedMovement: true })` first and fall back to the plain call (raw counts like native
    raw input, input.cpp:1050-1066).

## Settings and UI

- `sensitivity` keeps its key and range (settings.mjs:5) and is relabelled `Free look sensitivity`.
- New `weaponSensitivity` and `sniperSensitivity`: `{ initial: 1, min: 0.1, max: 5, step: 0.1 }`, same range and step as
  native (menu.cpp:614-615).
- `invertY` unchanged.
- All three sliders and Invert look sit at the top of the Controls tab (menu-shell.mjs:17-22), above C05's binding list.
- Add the four to C01's host settings schema (host-settings.mjs and the matching header indices) as `f32` scalars and
  one flag bit: `free_look_sensitivity`, `weapon_sensitivity`, `sniper_sensitivity`, `invert_mouse` of the
  `app::Settings` shim; changes apply on the next input update.

## Acceptance criteria

- In a mission, left click fires, right click alt fires, middle click uses or equips, wheel steps walking speed, and in
  EEV, sniper scope and sticky camera the wheel zooms one level per notch and stops at the ends.
- In every game menu (main menu, pause, OPSAT pages, load game list, P_Goals tabs) the cursor is visible, hover moves
  focus, click selects; the wheel scrolls lists and the focused interaction list.
- The OPSAT key closes the OPSAT from any page; F5 and F8 quick save and quick load in single player; numpad digits and
  mouse clicks work on keypads; typing a new profile name works.
- Vertical and horizontal mouse look turn at the same rate; each of the three sensitivities only affects its view class.
- No regression in pause on focus loss (C01's `pauseOnFocusLoss`), stepping metrics or gamepad play.

## Testing

- First, failing unit tests: wheel delta accumulator, click counting, character ring, `KEYBOARD_INPUT` layout bounds
  (tests/browser/keyboard-input.test.mjs); update tests/browser/mission-controls.test.mjs for the removed scaling and
  the Escape-to-pause tap.
- Native plus Wasm contract test for input-core: walking speeds, diagonal speed, ladder and pipe back key, lockpick
  quadrants, menu and OPSAT remaps, wheel routing per view, keypad numpad table, per view sensitivity selection.
- Build in a separate directory (for example out/parity-c03), never out/boot-m5. Editing cpu_runtime.h or x86_ops.h is
  not needed and must be avoided.
- agent-browser with the mute init script out/front-end/mute.js: start a development mission, check pointer lock, fire
  with LMB, wheel through walking speeds, open EEV and zoom, open pause with Escape and pick RESUME with the mouse, open
  OPSAT, click a goals tab, close with the OPSAT key; screenshot each.

## Risks and open questions

- Ownership of the action and key name table: C05 is the single source of truth, but C03's input core needs it first.
  Either C05 task 1 lands before C03, or C03 creates `input-actions.mjs` exactly as C05 specifies it.
- The input core port is large for a Simple chunk (about 600 lines of input.cpp logic); consider splitting it out if the
  implementer stalls.
- Re-locking the pointer after a menu closes needs a user gesture in Chromium after an Escape exit; a click on the
  canvas will be required. Confirm whether a programmatic re-lock after our own `exitPointerLock()` is allowed.
- Browser `movementX` units differ from Windows raw counts; the default 1.0 may feel different from native. Calibrate
  against the current browser feel before changing defaults.
- Is EEV better served by the sniper or the free look sensitivity? This plan treats every `View::Eev` as sniper.
- Mouse4 and Mouse5 may trigger history navigation in some browsers despite `preventDefault`.
- If C06 is late, keypad mouse picking stays off (stub `ProjectModel`); do not add a second `sub_002898CB` override.
- `G<T>` references held across `CallGuest` (C01 risk list): keypad.cpp and name_entry.cpp only use `G<uint8_t>` for
  immediate reads and writes, but audit mouse.cpp's `Menu` loop before relying on flat memory only builds.

## Out of scope

- Synced co-op paths (`PadWheel`, coop_mouse.cpp, net session flags) and split screen keyboard player selection.
- Key rebinding UI, persistence and controller remapping (C05).
- Keyboard legends and tutorial text (C04).
- Versus (sc4_adv.xbe) mouse handling.

## Implementation notes

What C03 built. C05 landed in the same change and shares the input core.

### Input core

- `src/browser/input-core.cpp` implements all of `src/core/input.h` by porting `src/core/input.cpp` with `Down(vk)`
  reading the page's held key bitmap and the gamepad record read from gamepad-input.mjs. Windows cursor clipping,
  `PostMessage` and SDL are gone; the page reacts to the guest's flags instead.
- `Pump()`, called by every reader, takes what the page delivered since the last call: wheel notches (as
  `OnRawInput`), typed characters (as `OnChar`), Escape taps, activity for `KeyboardActive`, and the hide HUD key. It
  writes the guest flags back (menu, pointer, text entry, map drag, movie, capture).
- `MenuInput::SetPadSource(browser::input_core::Sample)` makes XInputGetState return `input::Poll(port)` for every
  port; ports without the keyboard or a pad answer 1167.
- `TakeMouse` picks the sniper sensitivity in the EEV and every scope, the weapon sensitivity while aiming, free look
  otherwise, then applies Invert look. Movement arrives in thousandths of a count.
- Contract test `tests/browser/input-core.cpp` (native ASan/UBSan and wasm): walking speeds and the wheel, diagonal
  speed, ladder and pipe back key, lockpick quadrants and rest, menu, pause menu and OPSAT remaps, the cursor's left
  button in menus, wheel routing to EEV and menus, numpad keypad buttons, per view sensitivity and invert, binds from
  the page, a guest rebind posted to the page, the settings panel lockout, inactive page, pad binds in play and stock
  buttons in menus.

### Raw input span

- `src/browser/keyboard-input.{h,mjs}` (`KEYBOARD_INPUT`, 64 words, module static, `image_keyboard_address/words`):
  change counter, 256 bit held keys by virtual key code, wheel up and down notch counters, counted press and clicks,
  cursor x, y, w, h in CSS pixels over the canvas, active, mouse movement, Escape taps, a 64 unit character ring, the
  guest flags, and the host panel and console words C05 uses.
- The cursor is published with the canvas size, so `mouse.cpp` and `keypad.cpp` map it with `app::Aspect()` exactly.
- Wheel: 100 pixels, 3 lines or one page a notch, a turn the other way drops the part notch.
- Clicks count only for a left press and release both made while the guest flags say menu or pointer.
- Unit tests: `tests/browser/keyboard-input.test.mjs` (layout against the header, wheel accumulator, keys, clicks,
  character ring, movement, Escape tap, our own lock release).

### Hooks and pointer lock

- Default `BROWSER_GAME_HOOKS` adds mouse, mouse_pitch, views, keypad, quick_save, opsat, interaction_list,
  analog_keys, name_entry, whistle, console, unreal (and options_menu for C05). The include allowlist has
  `core/input.h`, `game/console.h`, `game/unreal.h` and `app/translations.h`.
- `sub_0004B525` moved to `src/browser/input-update.cpp`, compiled in every menu build, renamed for mouse.cpp
  (`0004B525:input-update`); in a mission build it still samples the simulation (id 132). MissionInput no longer sends
  axis events (`tests/browser/mission-input.cpp` checks that).
- `src/browser/game-stubs.cpp` has weak `game::PromptsTick` (C04) and `game::ProjectModel` (C06); the chunk that
  selects prompts.cpp or fov.cpp overrides them, nothing to delete.
- mission-controls.mjs keeps pointer lock (raw `unadjustedMovement` first) and pause; it no longer scales movement or
  handles Escape, and it never takes the lock while the guest frees the cursor. keyboard-input.mjs frees the lock when
  a menu or pointer opens, retakes it after its own release when play resumes (where the browser allows it), and taps
  Start once when the lock is lost in play by anything else (the browser's Escape).

### Checked in the browser (out/parity, port 8140, development start)

- Walking with W and the pause menu on Escape through the input core.
- In the pause menu the cursor hover moved focus and clicks opened Options and its pages.
- All 33 hook addresses bound in Diagnostics, no fault.

### Not checked

- Automation cannot hold real pointer lock or deliver the browser's own Escape exit, so EEV wheel zoom, sticky
  camera, keypads, quick save and load, OPSAT key and name entry are covered only by the contract test and need the
  user's playtest. Mouse4 and Mouse5 history navigation was not checked.

### Static lifetime

| Static | Restart safe because |
| --- | --- |
| input-core state (`g_speed`, `g_view`, flags, binds) | Set again by the hooks every input update; binds reload on the settings revision |
| keyboard-input `Shared()` | Module static by design, the page keeps writing it |
| mouse.cpp `g_page`, `g_clicks`, `g_point`, `g_messages` | Compared against fresh values every update; a stale page id only skips one hover |
