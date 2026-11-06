# C05: Input configuration and in-game settings pages

Class: Simple
Depends on: C01 (settings channel, app::Settings shim, compile mechanism); task 1 is a prerequisite of C03 and C04; the
in-game pages (tasks 9 to 12) need C03's input core and C06's hud.cpp element draw chain (for `OptionsElement`)
Native sources: src/core/input.cpp (action tables, key names, Bind/Unbind/BindPad/LoadBindings), src/app/settings.cpp
(keys, controller and player tables), src/app/app.h (Settings, rows API), src/app/menu.cpp (rows: InputRows, PadRows,
kSettingsRows, rows namespace), src/game/options_menu.cpp

## Goal

One binding source of truth for the browser: an action table with native ids, labels and defaults, persisted overrides
in settings.mjs, a Controls UI to rebind keys and controller buttons, the bindings delivered to the guest so the input
core (C03) and the key legends (C04) use them, and the native RECOMP SETTINGS, MOUSE AND KEYBOARD and CONTROLLER pages
inside the game's own options menu so settings can be changed without leaving the game.

## Native behaviour

- Actions: `kStoryActions` (input.cpp:199-231) gives each keyboard action an id, label, default key names ("W",
  "Space,Enter", "LMB", "Wheel Up") and what it presses: a pad target (0-7 analog, 8-15 button bits, 16-19 move keys),
  an event (`KeyEvent`, once per press: zoom, reload, whistle, quick save, quick load, push to talk, hide HUD, settings
  panel) or a host key (`kHostKey`: push to talk, console, hide HUD, settings panel). `play_only` actions do not press
  in menus.
- Pad actions: `kStoryPadActions` (input.cpp:281-300) map each console control to a physical button named in `kPadNames`
  (input.cpp:329-333: A B X Y RB LB LT RT, D-PAD, START, BACK, LS, RS, touchpad halves). Quick save, quick load, push to
  talk and toggle HUD have no default button.
- Key names: `kNames` (input.cpp:363-388) maps Windows virtual keys to names, including mouse buttons and the wheel as
  pseudo keys `0x0E`/`0x0F` (input.cpp:82). Letters, digits and symbols bind by US position (`kPositions`
  input.cpp:392-401).
- Binding rules: `Bind` (input.cpp:1411-1429) has two slots per action, a key taken from the other slot moves, host keys
  cannot take mouse buttons, the settings panel can never be left without a key (`LockedOut` input.cpp:1409); `Unbind`
  (input.cpp:1431-1439); `BindPad` (input.cpp:1532-1557) swaps buttons between actions, events may come off;
  `LoadBindings` (input.cpp:1485-1512) resolves saved names over defaults and settles duplicate pad buttons;
  `ResetBindings`, `ResetPadBindings`. Menus keep the stock pad buttons; the remap applies in play (`Rebind`
  input.cpp:944-975).
- Persistence: `Settings::keys` and `Settings::pad_buttons` are sparse maps of overrides (app.h:72-73), saved as
  `[keys]` and `[controller]` tables (settings.cpp:131-136, 183-188). `players[2]` puts keyboard and controllers on
  guest ports 0 and 1 (app.h:23-27, 71; settings.cpp:122-130).
- Rows: `app::rows` (app.h:159-180) is shared by the F10 panel and the game's options menu. Page 0 is the RECOMP
  SETTINGS list with DISPLAY, GRAPHICS, GENERAL and INPUT DEVICES headings (menu.cpp:322-359); page 1 MOUSE AND
  KEYBOARD: sensitivity, invert, then one bind row per action (menu.cpp:362-378); page 2 CONTROLLER: one pad bind row
  per pad action (menu.cpp:380-389).
- `options_menu.cpp` adds three items to the stock options list (`sub_001A53B5`, options_menu.cpp:748-763) and takes
  over P_Video's list to show a rows page: OnPress `sub_001A4161` (options_menu.cpp:778-813), OnBack `sub_001A5C41`
  (816-841), OnKey Left / Right `sub_001A5786` (844-857). RECOMP SETTINGS first lists the sections named in `kSections`
  = DISPLAY, GRAPHICS, GENERAL (options_menu.cpp:104) and draws them through `OptionsElement` from hud.cpp's element
  chain (options_menu.cpp:586-612). Value rows with choices open the stock popup, whose option vtable gets host
  functions through `dispatch::HostThunk` (options_menu.cpp:20-22, 293-326). `MenuTick` (options_menu.cpp:707-746)
  refills on `app::SettingsRevision()` changes, runs key capture (`input::PressedKey`, `PressedPadButton`, Escape `0x1B`
  cancels, options_menu.cpp:688-705) and clears a binding on Delete. It also exports `StackOwner`/`OnStack`
  (options_menu.cpp:615-645, used by prompts.cpp), `BlankRow` and `MenuPlace` (used by mouse.cpp).

## Browser today

- Keyboard bindings are a fixed `Map` from `KeyboardEvent.code` to `MENU_INPUT` bits (menu-input.mjs:10-25); several
  differ from native (Enter is Start, Space and Z confirm, Tab is Back/OPSAT, F is left thumb).
- The Controls tab shows a static key list (menu-shell.mjs:17-22, rendered menu-shell.mjs:184-196) next to the
  sensitivity and invert rows.
- The host panel opens on F1 or P in play (menu-shell.mjs:404-413); natively F1 is OPSAT and F10 the settings panel. In
  the panel, arrows, Escape, Backspace and gamepad buttons navigate (menu-shell.mjs:217-240, 416-429).
- settings.mjs stores flat values (options, booleans, numbers) under `scda.settings` in localStorage
  (settings.mjs:1-68); there is no map type.
- Gamepad: first connected standard mapping pad, buttons mapped by index to `MENU_INPUT` bits (gamepad-input.mjs:6-8,
  51-60). Only guest port 0 is served (menu-input.cpp:139 answers 1167 for other ports) and Coop is refused by
  `MenuRoute` (menu-route.cpp:250), so player port settings have no use.
- `Runtime::HostThunk(X86Func*)` already exists (cpu_runtime.h:482).

### Decisions

- The guest identifies keys by Windows virtual key code, as native. C03's held key bitmap is 256 bits indexed by VK
  code, so `input.cpp`'s `Down(vk)`, `PressedKey`, Escape `0x1B` in options_menu.cpp and every name lookup port
  unchanged.
- Persisted names are native names ("W", "LMB", "Wheel Up", "Numpad1"), so `Keys` in prompts.cpp and text.cpp's
  "MB"/"MOUSE" checks work unchanged. `KeyboardEvent.code` is US positional like native binds.
- Browser default changes, only where the browser reserves the key: `quick_inventory` defaults to Tab (Ctrl held with W
  closes the tab and cannot be prevented outside fullscreen keyboard lock). The host panel moves to the `settings_panel`
  action (default F10, native parity); F1 becomes OPSAT. Escape stays the pause key (C03 turns the pointer lock exit
  into a Start tap).
- Rows hidden in the browser: push to talk (no voice), hide HUD (needs hud.cpp), touchpad buttons (not in the standard
  mapping), player ports.
- Port options_menu.cpp: yes, compiled unchanged; the browser supplies `app::rows` over its own settings.

## Tasks

1. Add `src/browser/input-actions.mjs`: `ACTIONS` (story table of input.cpp:199-231: id, label, defaults, kind, play
   only), `PAD_ACTIONS` (input.cpp:281-300), `KEY_NAMES` (`{ name, vk, codes }` for every `kNames` entry; mouse buttons
   as `button` numbers, the wheel as `wheel: 1 | -1`), `PAD_BUTTONS` (native names to standard gamepad indexes). Unit
   test: every name in `kNames` (parsed from src/core/input.cpp) is present with the same VK, every default resolves, no
   code maps to two VKs.
2. Add `src/browser/input-bindings.mjs`, pure functions ported from input.cpp: `bind(keys, id, name, slot)`, `unbind`,
   `bindPad(pads, id, button)`, `resolve(keys, pads)` (LoadBindings), `reset`, `resetPad`, `label(name, layoutMap)`
   (layout aware label through `navigator.keyboard.getLayoutMap()` where available, as native shows the player's
   labels). Write the tests first from the native rules listed above.
3. settings.mjs: add map settings `keys` and `padButtons` (`{ initial: {}, map: true }`), normalized against
   input-actions.mjs (unknown ids and names dropped, empty string kept as unbound); keep the same localStorage key.
   Tests in settings.test.mjs.
4. Deliver to the guest through the C01 channel: C01's settings block already reserves its byte area for `keys` and
   `pad_buttons` as `name=value` lines (C01 task 8); encode the merged override maps there. The C03 input core reloads
   its tables (`LoadBindings`) when the channel revision changes.
5. Make menu-input.mjs (C03's raw producer) translate `code` to VK through `KEY_NAMES`; delete the `bindings` Map
   (menu-input.mjs:10-25).
6. Gamepad remap: gamepad-input.mjs publishes physical buttons in `kPadNames` order; port `PadTarget`, `Rebind`,
   `ApplyPadBinds` (input.cpp:936-988) into C03's input core so the remap applies in play and menus keep stock buttons.
7. Controls UI in menu-shell.mjs: the Controls tab keeps C03's sensitivity and invert rows, then one row per visible
   action with two slots (key and alternate); a new Controller tab lists pad actions with one slot each; each tab has
   Restore defaults. A slot press shows "Press a key" (or "Press a button"), takes the next key, mouse button or wheel
   notch (or gamepad button), Escape cancels, Delete clears. Focused rows scroll into view (`focus({ preventScroll: true
   })` today, menu-shell.mjs:106, 113). Extend the `TABS` row descriptor with `bind` and `padBind` kinds.
8. Host panel key: read the `settings_panel` binding instead of F1 / P in menu-shell.mjs `keydown`; keep the gear
   button.
9. Add options_menu to C01's `BROWSER_GAME_HOOKS` (allowlist `app/translations.h`); delete C03's stubs for `MenuTick`,
   `MenuPlace`, `BlankRow` and C06's pass-through `OptionsElement`. Provide `dispatch::HostThunk` forwarding to the
   current `Runtime::HostThunk`.
10. Add `src/browser/settings-rows.cpp`, the browser `app::rows` (Page, Value, Step, Defaults, Choices, Choose, Shown,
    Enabled) plus `app::SettingsRevision`, `app::MenuOpen` (true while the host panel is open), `app::ConsoleOpen` (C04
    overlay). Page 0 headings must be DISPLAY, GRAPHICS and GENERAL because options_menu.cpp names them; page 1
    sensitivities, invert and keyboard binds; page 2 pad binds.
11. Message rings (C01 leaves guest writes thread local and has no reverse channel, see C01 Risks): add
    `src/browser/host-messages.h` and `host-messages.mjs`, one span owned by `StoryState` (survives restarts) with two
    single producer single consumer rings of bounded entries (`kind`, UTF-8 payload up to 256 bytes): guest to host
    (`set name=value`, `quit` for C04's Exit, `console` output lines for C04) and host to guest (`console` command lines
    for C04). Export `image_messages_address` / `image_messages_words` and add the span to `readInputDescriptor`'s
    overlap checks (boot-session.mjs:226-246). `Step`, `Choose`, `Defaults`, `Bind`, `Unbind`, `BindPad` and the browser
    `app::Save()` post `set` entries instead of writing `app::Get()`; menu-shell.mjs drains the ring every presented
    frame, applies `settings.set` (which persists) and the C01 writer pushes the new snapshot; `SettingsRevision` counts
    pushes so `MenuTick` refills (options_menu.cpp:718-721). Unit test both rings first (wrap, full, torn entry).
12. Host keys during in-game capture: while a capture waits, the host must not act on the settings panel or console key
    (native `HostKey` input.cpp:1458).

## Settings and UI

- `keys`: map, default `{}`; Controls tab bind rows; guest field `keys`.
- `padButtons`: map, default `{}`; Controller tab; guest field `pad_buttons`.
- In-game pages (all reach the guest through the C01 channel, edits come back through task 11):
  - DISPLAY: Picture (`scaling`), Upscaling (`filter`).
  - GRAPHICS: Frame rate (`showFps`).
  - GENERAL: Master volume (`volume`), Skip intros (`skipIntros`) and Pause when unfocused (`pauseOnFocusLoss`), both
    from C01's `Game` tab.
  - MOUSE AND KEYBOARD: free look, weapon and sniper sensitivity, Invert look (C03), then the binds.
  - CONTROLLER: the pad binds.
- Fullscreen is host only (needs a user gesture).

## Acceptance criteria

- Rebinding any visible action in the Controls tab changes what the key does in the next mission and survives a page
  reload; Restore defaults returns to the table.
- Mouse buttons and the wheel can be bound; host keys refuse mouse buttons; the settings panel key cannot be left
  unbound.
- Swapping two controller buttons swaps them in play while menus still use A to confirm and B to go back.
- C04's legends show the new key after a rebind.
- In a mission's pause menu, Options shows RECOMP SETTINGS, MOUSE AND KEYBOARD and CONTROLLER; changing a value or a
  binding there updates the host Controls tab and localStorage at once, and a change in the host panel shows on the open
  in-game page.
- F10 opens the host panel in play; F1 opens the OPSAT.

## Testing

- Unit tests first (node --test tests/browser/*.test.mjs): input-actions table cross check against src/core/input.cpp,
  input-bindings rules (two slots, moves, lockout, pad swaps, events off, duplicate settling), settings normalization of
  maps, Controls tab capture flow with a fake document (Escape cancels, Delete clears, wheel capture).
- Native and Wasm contract test for settings-rows.cpp and the guest to host write path.
- Build in a separate directory (for example out/parity-c05), never out/boot-m5.
- agent-browser with out/front-end/mute.js: rebind fire to a key, reload the page, start a development mission, fire
  with that key; open the game's pause menu, Options, MOUSE AND KEYBOARD, rebind a key in-game, check the host Controls
  tab; screenshot each page.

## Risks and open questions

- C01 does not carry guest to host writes, so task 11 adds the rings; C04's Exit and console depend on them. If C01
  later grows a reverse channel, fold the rings into it.
- Browser reserved keys: confirm with agent-browser that F1, F5, F10, Tab, Space and Backtick can be prevented in the
  target browsers; Ctrl+W, Ctrl+T, Ctrl+N cannot.
- `getLayoutMap()` is Chromium only; elsewhere labels are US names.
- A guest worker blocked in a long wait (C04 loading wait) delays in-game revision refills; harmless but visible.
- The settings revision push and `MenuTick` refill must not fire during a popup or capture (options_menu.cpp:718 already
  guards this).

## Out of scope

- Player ports, split screen and a second keyboard or controller port.
- Versus action and pad tables.
- Touchpad halves and non standard gamepad mappings.
- Native F10 rows the browser has no backend for (monitor, resolution, MSAA, shadows, HDR, co-op tick rate, microphone).
