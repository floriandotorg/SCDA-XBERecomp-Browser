# Native parity chunks

Features of the native Windows/D3D12 PC port that the browser port does not have yet, split into chunks that one agent
session can take on its own. Every chunk file has the same sections: goal, native behaviour, browser today, tasks,
settings and UI, acceptance criteria, testing, risks and open questions, out of scope.

The browser build compiles none of `src/app`, `src/game`, `src/versus` or `src/gpu/d3d12.cpp` today. Almost every
game patch therefore waits for C01, which makes native hooks build and run on browser memory and gives them settings.

## Classes

- **Simple**: porting understood code onto infrastructure that exists once its dependencies land. A standard model
  can do it.
- **Complex**: renderer, memory model, multi-module or networking design. Use the highest tier model.

## Chunks

| Chunk | Class | Depends on | Summary |
|---|---|---|---|
| [C01](C01-hook-foundation.md) Hook foundation | Complex | none | Hook compat headers over browser memory, symbol macros, `app::Settings` shim, host settings channel into the guest, selective hook compilation, shared `d3d::` shims, collision handling for hooks the browser already overrides |
| [C02](C02-high-fps-gameplay-fixes.md) High FPS gameplay fixes | Simple | C01 | `fpsfixes`, `camera_shake`, `root_motion` (and verify `magma_time` from C01) so effects keep console speed at 60 FPS and above |
| [C03](C03-keyboard-mouse-gameplay.md) Keyboard and mouse gameplay | Simple | C01, C05 task 1, C06 (`ProjectModel`) | Native mouse look, wheel views, keypads, quick save, OPSAT key, interaction list, name entry; raw key, wheel and cursor input into the guest; per view sensitivities |
| [C04](C04-pc-menus-and-text.md) PC menus and text | Simple | C01, C05, C06 | Keyboard prompts, PC text, Xbox Live removal, Exit item (back to host menu), loading screen keys, developer console |
| [C05](C05-input-config-and-settings-pages.md) Input config and in-game settings pages | Simple | C01 | One action and binding table, key and pad remapping, Controls and Controller tabs, game options menu RECOMP pages, guest to host message rings |
| [C06](C06-widescreen-and-fov.md) Widescreen and FOV | Complex | C01 | Kernel widescreen flag, `fov`/`hud`/`movie_aspect`/loading edge hooks, pillarbox tracking, aspect aware canvas and layout, `aspect`, `fov`, `fmvScale` settings |
| [C07](C07-render-scale.md) Render scale | Complex | C01 settings channel only | Internal resolution in the WebGPU backend: host sized targets, Blit opcode, scaled viewports and rects, WGSL `kTexWide`, console sized readbacks, glow box filter, Match window and Dynamic modes |
| [C08](C08-anti-aliasing.md) Anti-aliasing | Complex | C01, coordinate with C07 | 4x MSAA companions with resolves before every read, SMAA ported to WGSL in Present |
| [C09](C09-shadows-and-light-distance.md) Shadows and light distance | Complex | C01, C07 (automatic resolution only) | `light_distance` first, then shadow map resolution up to 4096, soft shadow filter passes |
| [C10](C10-hdr-tone-mapping-and-glow.md) HDR, tone mapping, glow | Complex | C01 | HDR scene target and resolve, tone mapping, HDR canvas output with feature detection, `glow.cpp` coordinate fix |
| [C11](C11-ambient-occlusion.md) Ambient occlusion | Complex | C01 | SSAO and GTAO as fixed effect passes in the GPU service at the translucent pass hook |
| [C12](C12-simple-settings-pack.md) Simple settings pack | Simple | C01 for most items | Anisotropic filtering, brightness and contrast, frame cap, language, mono audio |
| [C13a](C13a-versus-image-runtime.md) Versus image runtime | Complex | C01, durable saves (soft) | `sc4_adv.xbe` as a second wasm module, identity checks, switching between story and versus with launch data and profiles |
| [C13b](C13b-versus-hooks.md) Versus hooks | Complex | C01, C13a, C03, C05, C06 | Port of `src/versus` hooks, simple memory hooks first |
| [C14a](C14a-network-transport.md) Network transport | Complex | C01 (C13a for versus checks) | Virtual LAN rooms over WebRTC with WebSocket relay, `xnet.cpp` port, relay server, hosting panel |
| [C14b](C14b-coop-and-local-players.md) Co-op and local players | Complex | C01, C14a, C03 | Co-op lockstep and co-op mouse, second local player and controller ports |
| [C14c](C14c-voice.md) Voice | Complex | C01, C14a, C13a, C03, C05 | Microphone capture, Communicator HLE, game voice first, Opus link second |
| [C15](C15-upstream-v3.md) Upstream V3 | Complex | C01, C03, C05, C10, C12 | Ragdoll and water fixes, mouse and controller options, rumble, gyro aiming, Chaos Theory tone mapping, colour blind modes |

Durable saves are a browser gap, not a D3D12 feature. They are planned in
[07-campaign-and-saves.md](../07-campaign-and-saves.md) (P7.1) and are a soft prerequisite for C13a, because profiles
must survive a module switch.

## Suggested order

1. **C01**, alone. Everything else builds on it.
2. **First wave**, independent of each other: C02, C05, C06, C07, C12, and the `light_distance` part of C09.
3. **Input and menus**: C03 (needs C05 task 1 and C06), then C04.
4. **Renderer quality**, one at a time in this order: C08, C09, C10, C11. They all edit `menu-backend.cpp` and
   `webgpu-service.cpp`, so running them in parallel invites merge conflicts. C07 should land before them, because
   each has to work with scaled targets.
5. **Multiplayer**: C13a, then C13b and C14a, then C14b and C14c.

## Parallel work rules

- Chunks that touch the renderer files (C06, C07, C08, C09, C10, C11) should not run at the same time.
- Each implementer builds in their own directory. `out/boot-m5` (port 8125) is the user's build and stays untouched.
- Edits to `cpu_runtime.h` or `x86_ops.h` rebuild all generated chunks (about 8 minutes). C01 is designed to avoid
  them.
- Browser tests use the agent-browser CLI with audio muted (`out/front-end/mute.js`). Anything that can change frame
  time is measured with `out/perf-60/bench.mjs`.

## Decisions for the user

These came up in several chunks and should be settled before the affected chunk starts:

1. **Gamma** (C12, C10): native ignores the game's gamma ramp and builds its own from brightness and contrast. C12
   proposes stacking the native curve on top of the game's ramp. The alternative is matching native and dropping
   the ramp.
2. **Language override** (C12): `03-game-boots.md` says host settings must not override the kernel language. A
   language setting changes that rule.
3. **Settings key** (C05): the host panel moves from F1 / P to F10 so F1 can be OPSAT as in native.
4. **Per view sensitivities** (C03): native has free look, weapon and sniper sensitivity only in versus. C03 brings
   them to story as a browser extension.
5. **Exit item** (C04): the game's Exit returns to the host main menu.
6. **MSAA levels** (C08): only 4x is portable in WebGPU; native offers 2x, 4x and 8x.
7. **Frame rate** (C01, C12): nothing caps the browser frame rate, so a 120 Hz display runs the game at 120 ticks.
   The C12 frame cap is the proposed answer; vsync off is not possible in browsers.
