# C11: Ambient occlusion (SSAO and GTAO)

Class: Complex
Depends on: C01 (hook build, app::Settings shim, host-settings channel). Coordinates with C10 (scene target format and a shared browser `d3d::State()` shim), C08 (multisampled depth), C07 (render scale changes the pass size), C09 (shares the `d3d::` shim and the depth access pattern).
Native sources: src/game/ambient_occlusion.cpp, src/xdk/d3d8.cpp, src/xdk/d3d8.h, src/gpu/backend.h, src/gpu/d3d12.cpp, src/gpu/render_thread.cpp, src/app/settings.cpp, src/app/app.h, src/app/menu.cpp

## Goal

Port the native screen space ambient occlusion to the WebGPU backend with both modes the native setting offers (1 SSAO, 2 GTAO): the guest hook at the start of the translucent pass, the projection and fog parameters, and the three pass chain (occlusion, horizontal blur, vertical blur multiplied into the scene) reading the scene's depth and stencil, at no visible frame time cost at 60 FPS.

## Native behaviour

Setting:
- `ambient_occlusion` 0 off, 1 SSAO, 2 GTAO, default 0 (app.h:54). Old boolean `true` loads as 1; values clamp to 2 (settings.cpp:101-103). Options row AMBIENT OCCLUSION with "OFF", "SSAO", "GTAO" (menu.cpp:43-44, 340, 545). Not gated by shader model; hidden in versus (menu.cpp:315-319). Live: read on every hook call.

Hook (ambient_occlusion.cpp:9-16):
- Replaces `sub_002AC2FC`, `DrawTranslucent(render context, hit proxy)`, cdecl. Context +0 is the scene node; node +8 its parent; node +0x8C the camera to screen matrix.
- Runs `d3d::Occlusion(&node[+0x8C], mode == 2)` before the original only when the mode is on, the hit proxy argument (`esp + 8`) is null and the node has no parent, so hit testing and sub views are left alone. At that point all opaque surfaces are lit and in depth; glass, smoke and the HUD land after.

Parameters (d3d8.cpp:170-176, contract in backend.h:47-50):
- Rejects anything but a D3D perspective projection (`m[11] == 1`, `m[15] == 0`, `m[0]` and `m[5]` non zero).
- `p = {1/m[0], 1/m[5], m[8], m[9], m[14], m[10], fog.slope, fog.offset, fog.amount, gtao}`; view z = `p[4] / (ndc depth - p[5])`.
- Fog comes from `LatchFog` at every draw (d3d8.cpp:123-157, called at d3d8.cpp:805): a fog pass is blend SRCALPHA / INVSRCALPHA with a 64x1 ramp on stage 3 (`kFogRampStage`, `kFogRampWidth`, d3d8.cpp:116-119); the fog plane (vs constant 116) projected on the clip w row (vs constant 107) gives slope and offset when the plane is parallel to view depth; amount is `PsConstant0` alpha.

Backend (d3d12.cpp:2493-2522, then `DrawOcclusionPasses` d3d12.cpp:2471-2490):
- Only the frame's own scene: render target and depth both found, guest size equal to the back buffer, same host size and sample count; viewport non empty and `max_z > min_z`.
- Pass rectangle: the viewport scaled to the target and clamped (`ScaledViewportRect`, d3d12.cpp:2134-2138); viewport of each pass is the whole target.
- Constants (`AoFrame`, d3d12.cpp:2515-2521): `proj = p[0..3]`; `zp = (p[4] * zr, min_z + p[5] * zr)` with `zr = max_z - min_z`; `vp = rect`; `opt = (kAoRadius 80, kAoMaxSpan 0.15 * rect height, step x, step y)`; `fog = p[6..8]` (d3d12.cpp:2130-2131).
- Targets: two `R16G16_FLOAT` at scene size, x occlusion, y view z (d3d12.cpp:2127, 2400-2408).
- Pass 1 `occlusion` (SSAO) or `gtao` into ao[0]; pass 2 `occlusion_blur` with step (1, 0) into ao[1]; pass 3 `occlusion_apply` with step (0, 1) into the scene with blend colour = dst * src, alpha = dst (`EnsureAoPipeline`, d3d12.cpp:2411-2432). Multisampled depth uses `Texture2DMS` sample 0 (`#define MS`).
- Shaders (`kAoHlsl`, d3d12.cpp:306-430): depth at t0, stencil at t2. `Raw` returns depth where stencil bit 0 (the world) is set, else 1 (the sky counts as far). `View` reconstructs view space from pixel, depth and projection. `Normal` takes the nearer neighbour each way. SSAO: 16 spiral taps with a 4x4 dither rotation, cubic falloff, bias 0.05, result `max(1 - a/4, 0)`. GTAO: XeGTAO high preset, 3 slices, 3 steps a side, falloff over the outer 61.5% of the radius, power 2.2. Radius projected to pixels, capped at 15% of viewport height; under 1 pixel returns 1. Blur: 5 taps, outer ones half weight, weights drop with view z difference over 2%. Apply: fades the occlusion by the already drawn fog `fog.z * saturate(fog.x * z + fog.y)`.
- Threaded native backend just forwards the parameters (render_thread.cpp:216, 452).

## Browser today

- `MenuBackend::Occlusion` throws "menu backend unsupported occlusion effect" (menu-backend.cpp:1646). Enabling the hook before this chunk lands would abort the mission.
- The diagnostic recorder only records the request (diagnostic-graphics.cpp:348-352; request names diagnostic-graphics.h:27, boot-session.mjs:141).
- No browser `d3d::Occlusion`, `d3d::State` or fog latch exists; the browser D3D HLE keeps its state in `DiagnosticGraphics::state_` (diagnostic-graphics.h:81), refreshed by `CaptureState` (diagnostic-graphics.cpp:518) before clears and draws (diagnostic-graphics.cpp:1192, 1249). `sub_002AC2FC` is in generated/game/x86_func_mapping.cpp and not hooked.
- Depth is `Depth24PlusStencil8` with `TextureBinding` usage (webgpu-service.cpp:860), so the AO pass can bind it directly. The depth readback pipeline already samples a depth only view as `texture_depth_2d` with `textureLoad` (`pack_depth`, webgpu-service.cpp:367-418); the upload pipeline writes depth and stencil (webgpu-service.cpp:420-472). Uninitialized depth is imported by `initializeDepth` (menu-backend.cpp:775-820).
- Draws are batched per colour and depth pair into one render pass (webgpu-service.cpp:588-593); any non draw operation must end that pass first, as `copy()` does (webgpu-service.cpp:670).
- MenuBackend tracks target versions and dirtiness for feedback copies and readbacks (`Copy` sets `dirty`, `command`, `version`, menu-backend.cpp:1562-1568).
- Scene target is `RGBA8Unorm` today; with C10 it may be an HDR format.

## Tasks

1. Setting. settings.mjs `ambientOcclusion` (see below) with failing tests first in tests/browser/settings.test.mjs; C01 maps `'off' | 'ssao' | 'gtao'` to `ambient_occlusion` 0, 1, 2 in the app::Settings shim.
2. Browser `d3d::` shim (new src/browser file, shared with C09 and C10). `d3d::Occlusion(const float* m, bool gtao)` checks the matrix as d3d8.cpp:172, builds `p[10]`, refreshes state the way the draw path does, and calls the active renderer's `Occlusion(state_, p)`. It must respect `DiagnosticGraphics::CheckRecorder` rules (owning worker, no borrowed memory lease). Unit test: a known projection gives the expected `p`; non perspective matrices produce no call.
3. Fog latch. Port `IsFogPass` and `LatchFog` into the browser draw path before `renderer_->Draw` (diagnostic-graphics.cpp:1249), describing the stage 3 texture through guest memory reads instead of `HostPtr`. Failing test first with synthesized state: a parallel plane latches slope, offset, amount; a tilted plane or wrong ramp leaves the previous values.
4. Hook. Compile src/game/ambient_occlusion.cpp unchanged through C01 (it uses `R32`, `G<float>`, `app::Get()`, `d3d::Occlusion`).
5. Transport. New service operation `Occlusion` (number allocated at merge with C07 to C10): `{ color target id, depth target id, mode, rect x y w h, constants[20] }` with a size static_assert, mirrored in webgpu-transport.mjs and the protocol tests.
6. MenuBackend. Replace the throw: resolve `state.rt` and `state.zs` to resources without creating new ones; return silently unless both exist, the colour surface is 640x480 guest size, depth matches it, the viewport is non empty and `max_z > min_z`. Call `initializeDepth`, flush staged draws (or stage the operation in the mixed batch so order is kept), send the packet, then set `dirty`, `command`, `++version` on the colour target. Contract test with a fake transport: mismatched sizes send nothing; a matching call sends exactly one packet after the preceding draws; a later readback of the target sees the new version.
7. Service passes. Lazily create two `RG16Float` targets at scene size (counted in `texture_bytes`, 2.4 MiB at 640x480) and four pipelines: occlusion, gtao, blur (all to `RG16Float`), apply (to the scene's format, blend colour src `Zero` dst `Src`, alpha src `Zero` dst `One`, which keeps alpha). Bindings: depth only view as `texture_depth_2d`, stencil only view (`WGPUTextureAspect_StencilOnly`, sample type `Uint`) as `texture_2d<u32>`, previous AO target as `texture_2d<f32>`, a uniform buffer. End the batch pass first; viewport the whole target, scissor the rect. Prefer `wgpuDeviceCreateRenderPipelineAsync` at first use or warm up when the setting turns on so the first AO frame does not stall.
8. WGSL port of `kAoHlsl`, one module per entry point or one module with four fragment entries. Mappings: `Raw` uses `textureLoad(stencil, p, 0).r & 1u`; integer clamps with `vec2<i32>`; `kDither` as a `const` array; HLSL `sign`, `acos`, `saturate` to WGSL `sign`, `acos(clamp(x, -1, 1))`, `clamp`; keep every constant bit for bit. Compile check of all entries in a real browser (tests/browser/menu-shaders-browser.html pattern) with `getCompilationInfo()` errors failing the test.
9. Numeric reference. A JS port of `View`, `Normal`, SSAO, GTAO, blur and apply run on a synthetic 64x48 depth and stencil (floor, box, sky) compared with a real GPU run through the service in tests/browser/webgpu-service.html, tolerance 1/256 per pixel.
10. MSAA (with C08). When C08 adds multisampled scene targets, add the `texture_depth_multisampled_2d` and multisampled stencil variant reading sample 0 as native `MS` does, and the apply pipeline at the scene's sample count. Until then reject `samples > 1` like native rejects mismatches.
11. HDR (with C10). The apply pipeline is keyed by the scene format; multiplying is scale invariant so no headroom constant is needed.
12. Measure and compare (see Testing).

## Settings and UI

- settings.mjs: `ambientOcclusion: { initial: 'off', options: ['off', 'ssao', 'gtao'] }` (native default 0).
- Graphics tab (shared with C07 to C10, created by whichever lands first), row "Ambient occlusion" with values Off, SSAO, GTAO. Not dimmed by shader model. Hidden in versus as native does.
- Flow: settings.mjs, C01 host-settings channel, app::Settings shim `ambient_occlusion`, read by the hook on every call. No pipeline key of the game's draws changes, so switching is live mid mission; the first frame after turning it on builds four effect pipelines at most.

## Acceptance criteria

- Off: no `Occlusion` packets, frames byte identical to today.
- SSAO and GTAO: contact shadows in corners and under objects in the opening and control views; the sky, glass, smoke, HUD and fogged distance are not darkened; first person scope and sub views (node with a parent) and hit tests are unaffected.
- Numeric reference test passes for both modes; the WGSL compiles without warnings in Chrome.
- Switching modes mid mission never trips the 2 s backlog watchdog (mission-stepper.h:16, 34-36) and never raises a service error.
- bench.mjs: mean frame interval with SSAO and with GTAO within 0.5 ms of off on the M1 Max host, p95 within 1 ms.

## Testing

- Unit and contract tests first: settings; `d3d::Occlusion` parameter construction; fog latch; MenuBackend packet sequence and versioning with a fake transport; protocol size checks for the new operation.
- GPU tests: WGSL compile check and the numeric reference in the existing real browser service harness.
- Visual: agent-browser on a separate build directory (never out/boot-m5, port 8125), muted with out/front-end/mute.js, development start, captures at world time 30 and 60 for off, SSAO and GTAO. Native references: the user runs the Windows build at 640x480, render percent 100, MSAA off, and captures with F3; the capture log contains the `occlusion rt ... ds ...` line (d3d12.cpp:2506-2509) and the frame before and after it. Compare the darkened regions by eye and by a difference mask; byte equality is not expected (D24 vs the browser's depth implementation, filtering differences).
- Per draw debugging: the out/boot-debug channel; log the packet's constants once per mission to compare with the native `LOG_FIRST` line (d3d12.cpp:2510-2512).
- Performance: out/perf-60/bench.mjs A/B, off vs SSAO vs GTAO, idle host, interleaved pairs. Expected GPU work at 640x480: about 307k pixels, SSAO about 40 depth and stencil loads a pixel (16 taps plus normal), GTAO about 22 `View` calls a pixel, two 5 tap blurs; well under 1 ms on M1 Max where the GPU service is mostly idle. CPU cost is one packet per frame. Re-measure under C07 render scale since cost grows with pixel count.

## Risks and open questions

1. The hook runs inside generated code. How C01 lets a hook call into `DiagnosticGraphics` (current instance pointer, memory lease release) is not defined yet; `d3d::Occlusion` needs it.
2. Stencil bit 0 marks the world. If the browser's stencil emulation or depth import (`UploadDepth`) does not preserve it exactly, `Raw` returns 1 everywhere and AO silently does nothing. Add a probe in the debug build counting stencil bit 0 pixels at the hook.
3. Depth precision: `Depth24PlusStencil8` may be backed by a 32 bit float depth on Apple GPUs; view z reconstruction is unaffected in theory, but compare the GTAO horizon results against native at long range.
4. Binding the stencil aspect of a combined depth stencil texture as a sampled `uint` texture must work on every target browser (Chrome on Metal, Safari). Verify; fallback is a small pass copying the stencil bit into an `R8Uint` texture.
5. Ordering: posted and batched commands must keep the AO operation after all opaque draws and before translucent draws; a misordered batch would occlude glass or miss geometry.
6. Fog parameters latch from the previous fog draw; on the first frame of a level they may be stale, as native.
7. `CaptureState` before the hook call: confirm the render target and depth pointers are current at `sub_002AC2FC` entry without a draw in between.
8. Opcode numbering conflicts with C07 to C10 (allocate at merge).

## Out of scope

- AO at render scales other than 1 and multisampled depth (follow C07 and C08, tasks 10 and 11 only prepare the hooks).
- HDR target formats themselves (C10).
- Soft shadows that read the same depth (C09).
- Any AO in versus (native hides it).

## Implementation notes

What C11 built.

- Setting `ambientOcclusion` ('off', 'ssao', 'gtao'); host word 26 fills `ambient_occlusion` 0 to 2, read by the hook
  on every call (live). Rows: Graphics tab "Ambient occlusion" (Off, SSAO, GTAO), in-game AMBIENT OCCLUSION.
- `ambient_occlusion` is in the default hook list, compiled unchanged. Its `d3d::Occlusion`, the matrix check, the
  parameters and the fog latch were already in place (hook-d3d.cpp, hook-graphics.cpp, covered by hook-d3d.test.mjs).
- Service opcode `Occlusion` (25), `OcclusionPacket { depth, gtao, constants[20] }` (kAoHlsl's proj, zp, vp, opt,
  fog). `kAoWgsl` is the native HLSL in WGSL with four entries (`occlusion`, `gtao`, `occlusion_blur`,
  `occlusion_apply`): depth through a depth only view, stencil bit 0 through a stencil only `texture_2d<u32>` view,
  multisampled variants reading sample 0 for 4x depth, rg16f pair at the scene's size, the apply multiplying colour
  and keeping alpha into the scene (its HDR format and 4x companion when it has them). Passes cover the whole target
  with the viewport rectangle as scissor.
- `MenuBackend::Occlusion` replaces the throw with native's guards (the frame's own scene, depth at its host size and
  samples, non-empty viewport, `max_z > min_z`), the rectangle in host pixels (radius 80, span 15% of its height),
  then marks the scene drawn. The command is posted in order with the draws. `Supports` now answers true for every
  effect.

### Checked in the browser (out/parity, development start)

- Off, SSAO and GTAO to world 60, and GTAO with MSAA 4x and HDR rendering: no validation errors; `drawnEffects`
  counts the calls (about 3500 by world 60 with AO on, 0 with it off). In the cave cutscene at world 30 SSAO darkens the
  rock creases; the sky stays as it is (stencil bit 0 is kept by the browser's depth path).
- Perf (bench, one run each): off 16.2 / 16.4 ms mean (opening / control), SSAO 16.1 / 16.4, GTAO 16.1 / 16.4.

### Not done

The numeric reference against a JS port, the WGSL compile test in menu-shaders-browser.html (compiled in Chrome in the
runs above), the MenuBackend packet sequence test (that harness already fails at HEAD), comparisons with native
captures, and hiding the row in versus (C13).
