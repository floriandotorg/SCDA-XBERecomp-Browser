# C09: Shadow resolution, shadow filter, shadow distance

Class: Complex
Depends on: C01 (hook build, memory helpers, app::Settings shim, settings channel); C07 for the
"automatic" shadow resolution only; uses the multisampled depth helper of C08 when MSAA is on
Native sources: src/game/shadows.cpp, src/game/light_distance.cpp, src/game/unreal.cpp, src/game/unreal.h,
src/gpu/d3d12.cpp, src/gpu/backend.h, src/xdk/d3d8.cpp, src/xdk/d3d8.h, src/app/settings.cpp, src/app/menu.cpp

## Goal

Port the three shadow settings of the native PC port to the browser: a larger shadow map than the
console's 512x512, Chaos Theory's shader model 3 soft shadow filter in place of the console's edge
blur, and a shadow distance multiplier that pushes light and shadow fade distances out. The
defaults must keep today's console look and cost nothing.

## Native behaviour (cite file:line)

Settings
- `shadow_resolution` (0 follows render scale, 512 console default, up to 8192), `shadow_distance`
  (1, 2, 4, 8), `shadow_filter` (1 console, 3 SM3 soft): src/app/app.h:48-50; loaded and clamped at
  src/app/settings.cpp:93-97. Menu tables: src/app/menu.cpp:50-51 and 550-563.

Shadow map targets (src/game/shadows.cpp)
- Hook on `CreateTargets` `sub_002E4552` (thiscall, ecx = D3DRenderDevice): after the original, reads
  the data pointers of the shadow map (device + 0x4CD8, a 512x512 LIN_D16 texture), its colour surface
  (+0x4CEC, Data 0) and the 256..16 swizzled A8R8G8B8 edge blur chain aliased over the map memory
  (+0x4BA4 to +0x4BF4, last 16x16), and stores them in `gpu::g_shadow` under the D3D lock:
  src/game/shadows.cpp:16-45, struct at src/gpu/backend.h:87-92.
- The renderer sizes the shadow map's host target to `shadow_res_` x `shadow_res_` instead of the
  render scale (`CreateTargetResource`, src/gpu/d3d12.cpp:1441-1446). The colour surface is never bound
  (1517-1518, 1526-1528). `kConsoleShadows = 512` (d3d12.cpp:70) decides whether the blur chain is
  treated as a screen effect (1531). A changed setting flushes and recreates all targets
  (`ApplySettings`, 4093-4118).

Soft filter
- Hook on `BlurShadowEdges` `sub_002EF9A1`: only when `shadow_filter >= 3` and the return address on
  the guest stack is `kDrawLevelReturn = 0x2B28C8` (DrawLevel's call), it passes the camera to screen
  matrix at `[ebx] + 0x8C` to `d3d::FilterShadows` before the original and calls
  `d3d::ApplyShadows` after it: src/game/shadows.cpp:24-26, 47-54.
- `d3d::FilterShadows` validates a perspective matrix (m[11] == 1, m[15] == 0) and passes
  {m[14], m[10]} to the backend with the current state: src/xdk/d3d8.cpp:178-188.
- Backend: four passes (256x256 copy of view depth and shadow alpha, 128x128 quarter, 128x128 penumbra
  mask, full size filtered shadow with an 8 tap depth aware disk), formats RG16F, R16F, R16F, R16F,
  sizes scaled by the integer render scale: src/gpu/d3d12.cpp:2140-2235 (WGSL-portable HLSL),
  2240-2245, 2247-2279, 2281-2318. Reads scene depth (multisampled variant under MSAA) and the scene's
  alpha through a feedback copy. `ApplyShadows` writes the filtered value into the alpha channel of
  the scene target, alpha write mask only, after the game's own blur: 2378-2399. Interface:
  src/gpu/backend.h:18, 51-54.

Light distance (src/game/light_distance.cpp)
- Per light: `TurnOffDistance` +0x29C, `ShadowsTurnOffStart` +0x2A4, `ShadowsTurnOffDistance` +0x2A8;
  vtable +0, name +0x20; the scale multiplies all three, the shadow pair only when the light has a
  shadow distance: light_distance.cpp:17-50.
- Hooks: `ALight::PostBeginPlay` `sub_0027902C` tracks and scales each new light (98-107);
  `UObject::SavePackage` `sub_00063E97` writes the map's values back for the save and rescales after
  (109-121); the per-view shadow fade `sub_002AA0B0` applies a changed setting and adopts lights of
  a level restored from a save through the level's actor list (actor +0x270 level, level +0x2C actors,
  +0x30 count) (123-143). Entries are verified against vtable, name and last written values before
  reuse (52-61). Uses `game::unreal::IsA` and `Name("Light")` (src/game/unreal.h:21-33,
  src/game/unreal.cpp:31-35). The setting applies live.

## Browser today (cite file:line; where the work plugs in)

- No src/game file is compiled; C01 adds the mechanism. None of the five functions above is hooked in
  src/browser (searched); their generated bodies exist (for example the indirect call returning to
  0x2B28C8 in generated/game/x86_recomp.66.cpp at line 28609, pushed with `X86_PUSH32(0x2B28C8u)`,
  so the native return address check works unchanged).
- `MenuBackend::FilterShadows` and `ApplyShadows` throw (src/browser/menu-backend.cpp:1647-1648).
  The CPU state capture records them (src/browser/diagnostic-graphics.cpp:353-362). Nothing outside
  the native `d3d::` layer calls them.
- `gpu::g_shadow` is declared in the shared src/gpu/backend.h:92 and therefore exists in the game Wasm,
  but nothing writes or reads it in the browser.
- The shadow map is an ordinary depth resource keyed by its guest data, created at the guest size:
  `ensure` src/browser/menu-backend.cpp:669-766; depth textures become D24S8 in the service
  (webgpu-service.cpp:859). Depth-only draws are already supported (menu-backend.cpp:1199-1212).
  Shadow compare sampling uses `textureDimensions`, so a larger host texture needs no shader change
  (src/browser/menu-shaders.cpp:81-120); `texsize` half texel offsets use the guest size, which
  matches native (d3d12.cpp:3160-3166, menu-backend.cpp:1179).
- Size limits on the way: guest descriptors over 4096 rejected (menu-backend.cpp:616-617), 16 MiB per
  resource (`maxResourceBytes`, menu-backend.h:12, not raised for missions at boot-wasm.cpp:157-170),
  128 MiB resident, 256 MiB service `TextureBudget` (webgpu-service.cpp:111), texture edge 8192 in the
  service (845-848). CPU depth import requires matching guest and texture size
  (webgpu-service.cpp:985-986), as does readback decoding (menu-backend.cpp:918-921).
- Ordering: draws, clears and copies are staged and replayed in order inside one batch
  (webgpu-service.cpp:675-715); the soft filter passes must join that stream.

## Tasks (numbered, concrete, each verifiable)

Shadow distance (no renderer work; do first)

1. With C01, compile src/game/light_distance.cpp and src/game/unreal.cpp into the mission build. Check
   that C01's helpers cover `R32`, `GF`, `HostPtr`, `mem::IsCommitted`, `X86_FUNC`,
   `X86_EXTERN_FUNC`; add only what is missing to the C01 shim, not to the copied file.
2. Verify all three hooks are reached in the browser, including through indirect calls (vtable
   dispatch must resolve to the host `sub_X`): one `LOG_FIRST` line per hook in a debug build.
3. Confirm saves written with distance 2x load with the map's values (scale 1) under the browser save
   path (docs/browser-port/07-campaign-and-saves.md); this is the purpose of the SavePackage hook.

Shadow resolution

4. Compile src/game/shadows.cpp. Provide a browser `xdk/d3d8.h` shim in C01's shim directory with
   `d3d::Lock()`, `d3d::FilterShadows(const float*)`, `d3d::ApplyShadows()`; `Lock` maps to the
   `DiagnosticGraphics` lock, the other two to new public `DiagnosticGraphics` methods that copy
   src/xdk/d3d8.cpp:178-188 (matrix check, p = {m[14], m[10]}, call `renderer_->FilterShadows(state_, p)`).
5. In `MenuBackend`, add a per-resource host extent (coordinate with C07, which introduces host sized
   targets; reuse its field rather than adding a second one). The depth resource whose data equals
   `gpu::g_shadow.map` gets `shadow_resolution` x `shadow_resolution` (512 keeps today's behaviour,
   0 means C07's render scale). The colour surface `g_shadow.color` stays unbound as today.
6. Budget rules: exempt the shadow map from `maxResourceBytes`, charge its host bytes
   (edge * edge * 4) to resident and service budgets, and cap the browser choices at 4096
   (64 MiB). Fail with a named diagnostic, never silently fall back.
7. Host sized depth must not import CPU depth: if `initializeDepth` would upload guest bytes into a
   host sized shadow map, clear it instead (the game clears the map before drawing; verify) or
   resample nearest. Readbacks of it (released-surface path) shrink to the guest size with a nearest
   pick in the depth packing pass, like native `PrepareCopySource` (d3d12.cpp:4122-4134).
8. Apply changes at a frame boundary: destroy and recreate only the shadow map resource. No pipeline
   key changes, so this is safe mid-mission. Log the new size once.

Soft filter

9. Service: a new staged opcode `ShadowFilter` (inside `RenderBatch` mixed streams, so ordering with
   draws is preserved) taking the scene colour resource, its depth resource, the constants
   {zp, vp, sz} and the scissor rectangle, and an `ApplyShadow` opcode. Port the HLSL at
   src/gpu/d3d12.cpp:2140-2235 to WGSL (`texture_depth_2d` or `texture_depth_multisampled_2d` for
   the depth, `textureLoad` and `textureSampleLevel` only). Targets rg16float and r16float at
   256 * s, 128 * s and full size, where s is the integer render scale as in native 2240-2245.
10. `MenuBackend::FilterShadows`: same guards as native 2250-2262 (target is the frame's scene, depth
    matches its size and samples, viewport non-empty), flush staged draws as needed, take a feedback
    copy of the scene (existing `feedbackTexture`, menu-backend.cpp:821-846) for its alpha, stage the
    filter; remember the target. `ApplyShadows`: stage the alpha-only apply pass into the same target
    if it is still the remembered one (native 2379-2384), bump the resource version and mark dirty.
11. The four filter pipelines and the apply pipeline are fixed; create them on first use (or prewarm
    at level load when the setting is on). They do not touch guest pipeline keys.

Settings and UI

12. Add the three settings to settings.mjs, the C01 mapping and the Graphics tab (see below).

## Settings and UI (settings.mjs entries, defaults, Options panel placement, how they reach guest/renderer)

- `shadowResolution: { initial: 512, options: [512, 1024, 2048, 4096] }`, label "Shadow resolution";
  add `0` ("Automatic") only after C07 lands. Applies at the next frame.
- `shadowFilter: { initial: 'console', options: ['console', 'soft'] }`, label "Shadow filter", values
  "Console" and "Soft (SM3)"; maps to `shadow_filter` 1 or 3. Live.
- `shadowDistance: { initial: 1, options: [1, 2, 4, 8] }`, label "Shadow distance", values 1x to 8x.
  Live (native applies at the next view, light_distance.cpp:63-70).
- Placement: Graphics tab (created by whichever of C08, C09, C12 lands first), below the
  anti-aliasing rows.
- Path: settings.mjs -> C01 host-settings channel -> `app::Settings` fields in the game Wasm. The
  game hooks read `app::Get()` directly; `MenuBackend` reads `shadow_resolution` at Present.

## Acceptance criteria

- Defaults (512, console, 1x): rendering and frame time unchanged; the new hooks run but write nothing
  beyond what native writes at scale 1.
- 2048: shadow edges of characters and props in the opening are visibly sharper; no validation errors;
  4096 works within budget on M1 Max or fails with the named diagnostic.
- Soft: shadow edges are soft and depth aware with no halo along silhouettes; with MSAA 4x (C08) the
  filter reads the multisampled depth without validation errors.
- Distance 4x: a light or shadow known to fade near the start fades visibly further away; a save made
  at 4x and loaded at 1x has the map's distances (inspect the light fields).
- Added GPU time for 2048, 4096 and soft is measured and recorded.

## Testing (unit/contract tests first where practical, agent-browser visual checks with muted audio, perf harness)

Failing tests first:
- light_distance logic in a native test harness (tests/browser style C++ test with a fake guest memory
  image from C01): a light with distances {1000, 500, 800} becomes {4000, 2000, 3200} at scale 4; a
  light with ShadowsTurnOffDistance 0 keeps its shadow pair; SavePackage writes 1x then restores 4x;
  a stale entry (vtable changed) is forgotten.
- shadows.cpp: with a fake device block, the `sub_002E4552` hook fills `g_shadow` from the four header
  offsets; the `sub_002EF9A1` hook calls FilterShadows and ApplyShadows only for `shadow_filter == 3`
  and the 0x2B28C8 return address.
- menu-backend.test.mjs with a fake `Execute`: the resource matching `g_shadow.map` is created at the
  configured edge, other depth resources at guest size; budget exemptions and the 4096 cap; staged
  order of draw, ShadowFilter, draw, ApplyShadow.
- webgpu-service.test.mjs: WGSL filter passes compile; on a synthetic scene (half lit alpha, a depth
  step) the penumbra mask marks only the boundary and the apply pass changes alpha only.
Then agent-browser captures with out/front-end/mute.js at default, 2048, soft, both, at the opening and
the cave; perf with out/perf-60/bench.mjs and `PERF_SOAK=60` per setting, separate build dir.

## Risks and open questions

1. The browser build must reach the hooks through indirect calls (`x86_icall` for 0x2EF9A1 is a
   vtable call). Confirm with C01 that the browser dispatch table resolves to the host override.
2. Does the game clear the shadow map every frame before drawing it? If not, host sized maps need a
   resample on import (task 7).
3. Budget headroom: the current peak `texture_bytes` in a heavy level is unknown. Measure before
   enabling 4096; 8192 (256 MiB) is not offered.
4. Native also limits the blur chain to the screen when the resolution differs from 512
   (d3d12.cpp:1531); in the browser this only matters once C07 scales targets. Agree with C07.
5. Stealth: scaling TurnOffDistance changes which far lights render; whether the stealth light meter
   uses rendered lights is unverified. Native ships it, but check the meter at 8x near a fading light.
6. Required console shadow parity is an M6 item (docs/browser-port/06-playable-mission.md:22, 155);
   this chunk assumes console shadows already render correctly and must not be used to hide defects.
7. Versus (`sc4_adv.xbe`) uses different addresses; this chunk covers the story XBE only.

## Out of scope

- Ambient occlusion, HDR and shader model 3 features other than the shadow filter.
- Render scale and the "Automatic" shadow resolution logic itself (C07).
- MSAA (C08); this chunk only consumes its multisampled depth view.
- Console shadow correctness work owned by P6.1.

## Implementation notes

What C09 built.

- `shadows` and `light_distance` are in the default hook list (light_distance uses the `mem::IsCommitted` shim).
  shadows.cpp's CreateTargets hook fills `gpu::g_shadow` (logged "shadows: map ..., edge blur ..."), and its
  BlurShadowEdges hook calls `d3d::FilterShadows` / `ApplyShadows` for `shadow_filter` 3 at DrawLevel's call.
- The `d3d::` effect calls now pause the guest worker's execution lease before reaching the backend, as every guest
  D3D call does (a hook runs inside generated code; the backend refused to wait with the lease held).
- Shadow resolution: the depth resource whose data is `g_shadow.map` (512 x 512) gets
  `shadowResolution` x `shadowResolution` through C07's host extent; a change resamples it at the next Present.
  Its draws, depth bias, imports and readbacks follow C07's scaled target rules.
- Soft filter: service opcodes `SoftShadows` (scene, its depth, zp, vp, sz) and `ApplyShadows`. The native HLSL is in
  WGSL (`kSoftShadowWgsl`): copy (rg16f, 256 x s), quarter and penumbra mask (r16f, 128 x s), filtered (r16f at the
  scene's size, in its viewport), then an alpha-only apply into the scene. A multisampled depth is read through
  `texture_depth_multisampled_2d` sample 0; a multisampled scene takes the apply in every sample and resolves.
  `MenuBackend::FilterShadows` has native's guards (the frame's scene, depth at its size and samples, a non-empty
  viewport) and `Supports` now answers true for the shadow effects.
- Diagnostics: `hookStatus.drawnEffects` counts effect calls the backend drew (word 6).
- Settings (Graphics tab and in-game GRAPHICS): Shadow resolution 512 (console), 1024, 2048, 4096 (next frame);
  Shadow filter Console or Soft (SM3) (live); Shadow distance 1x to 8x (live, as native at the next view). Host
  words 21 to 23 fill `shadow_resolution`, `shadow_filter` (1 or 3) and `shadow_distance`.

### Checked in the browser (out/parity, development start)

- Soft, 2048 and 2x distance, with and without MSAA 4x: no validation errors; `drawnEffects` about two a frame.

### Not done

The light_distance and shadows contract tests, the synthetic service test of the penumbra mask, a save at 4x loaded
at 1x and visual comparison in a scene with visible character shadows. Perf is measured in C07: 4096, soft and 8x
at 1080p with every other enhanced feature run at display rate once SoftShadows and ApplyShadows are posted.
