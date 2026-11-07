# C15: Upstream V3 single player features

Class: Complex
Depends on: C01 (hook build, host settings), C03 and C05 (input core, settings pages), C10 (HDR resolve), C12 (frame cap)
Native sources: upstream commits a694598 to ee5b440 (V3): src/core/input.cpp, src/core/gyro.cpp, src/game/gyro_look.cpp,
src/game/night_vision.cpp, src/game/fpsfixes.cpp, src/game/options_menu.cpp, src/gpu/d3d12.cpp, src/app/app.h, src/app/menu.cpp

## Goal

Bring the native port's V3 story features to the browser. Multiplayer (versus, co-op) is out of scope by the user's
decision, so the versus parts of V3 (AO and MSAA in versus, the versus world icon shape, versus pad look scaling) are
not ported.

## What the browser has now

- **High frame rate fixes** (upstream 40ce791, feda875): `fpsfixes.cpp` is a selected hook already, so the ragdoll
  repose between Havok steps and the 30 Hz dynamic water come with it unchanged.
- **Mouse** (92e71dd): vertical sensitivity (a scale on every view's sensitivity) and an inverted X axis. The browser
  keeps its free look, weapon and sniper sensitivities (C03), which cover native's aim sensitivities.
- **Controller** (0baa6b6, 39a7d19, 05731a7): horizontal, vertical and aim sensitivities through `input::PadLookScale`
  (read by `mouse_pitch.cpp`), inverted X and Y on the right stick, a dead zone moved onto the game's own, and a
  vibration strength over the game's rumble. The browser had no rumble: `XInputSetState`'s motors now reach the
  gamepad record and `gamepad-input.mjs` plays them as a standard pad's dual rumble.
- **Gyro aiming** (0ef591e, ee5b440): `src/browser/gyro.cpp` is native's gyro with the device's own motion sensor as
  the controller: a phone or tablet held in landscape, with or without a clip-on pad. `gamepad-input.mjs` writes
  `devicemotion` readings, turned into the picture's axes by the screen's rotation, into a ring in the gamepad record
  while gyro aiming is on; the game takes them at each look update with native's drift, smoothing and axes.
  `gyro_look.cpp` is compiled whenever `mouse_pitch` is. Browsers report no free pad buttons, so there is no gyro
  button or recenter button. iOS asks for motion permission on the next tap after gyro aiming is turned on.
- **Chaos Theory tone mapping** (1f942e4): the WebGPU resolve takes native's exposure, bright and tone passes, the
  level presets and their adaptation times. `night_vision.cpp` is always compiled and selects the night vision preset.
- **Colour blind modes** (a694598): native's daltonize matrices over the presented frame, after the gamma ramp.
- **Frame rate** (f4e849d): the frame cap defaults to the display rate, as native now defaults to the refresh rate.
- **Settings pages**: the in-game CONTROLLER page has native's GENERAL, GYRO and BUTTONS sections (the GYRO rows show
  only where the page reads a motion sensor), and the host panel has the same settings.

Not ported: texture dumping and replacement (a675411) need files beside the executable; a browser version would need
a folder picker or OPFS packs. The D3D12 error box, the compiler window and the monitor resolution fix have no browser
counterpart.

## Fixed on the way

- `ReadPad` read both sticks one word early (the left X from the menu mask), so a pad's and the touch screen's
  on-screen sticks were scrambled in play. Menus read the right words and were not affected.
- Numeric choices sent from the in-game pages (`frameCap=30`, `anisotropy=4`) were rejected by `settings.mjs`, so
  those rows did not change anything.

## Checked

- Native and wasm contract tests: host settings, input core (stick words, invert, dead zone, look scales, rumble),
  gyro (axes, ring overflow, stalls, calibration), menu input (rumble sink), gamepad input (rumble effects, motion
  ring, orientation), settings, boot session spans, inventory.
- The new WGSL compiles and its pipelines validate in Chrome; the colour blind presentation matches native's matrix
  exactly, and the exposure pass matches native's formula across the dark, normal and bright ranges.
- Story hooks that changed upstream compile against the hook compatibility layer.

## Not checked

- No full build or play test: `generated/` and `assets/` were missing when this landed.
- No real phone: gyro signs and axes follow the DeviceMotion specification, and iOS has reported
  `accelerationIncludingGravity` with the opposite sign in the past, which would only affect the WORLD axis.
