# C10: HDR rendering, tone mapping, HDR output and glow

Class: Complex
Depends on: C01 (hook build, app::Settings shim, host-settings channel). Coordinates with C07 (render scale, `kTexWide`), C08 (SMAA and MSAA run on the resolved frame), C09 (shadow blur chain targets must stay 8 bit), C11 (the occlusion apply pass writes into the HDR scene target).
Native sources: src/gpu/d3d12.cpp, src/gpu/shaders.cpp, src/gpu/shaders.h, src/game/glow.cpp, src/app/settings.cpp, src/app/app.h, src/app/menu.cpp

## Goal

Bring the native PC port's shader model 3.0 picture to the browser: the scene drawn into a 16 bit target with 8x headroom so light passes add past white, the resolve that adds bloom from what passes white and optionally applies auto exposure and a highlight roll off, HDR display output through a WebGPU canvas in extended tone mapping mode, and the game's own glow composite with the native half texel correction. Copies, readbacks, CPU uploads and the gamma ramp presentation must keep working when the scene target is not RGBA8.

## Native behaviour

Settings (all read by the renderer, none by the guest):
- `shader_model` 1 or 3 (app.h:51, settings.cpp:98), `hdr_rendering` (app.h:52, settings.cpp:99), `tone_mapping` (app.h:53, settings.cpp:100), `hdr_output` default true (app.h:44, settings.cpp:89). HDR rendering and tone mapping only take effect at shader model 3: `hdr_ = sm3_ && set.hdr_rendering`, `tone_ = sm3_ && set.tone_mapping` (d3d12.cpp:1325-1340, 4095-4103). Tone mapping without HDR rendering does nothing, `tone_` is only read inside `Resolve`.
- Options rows SHADER MODEL, HDR RENDERING, TONE MAPPING, HDR OUTPUT (menu.cpp:332, 337-339). HDR rows show "OFF" and are dimmed at 1.1, HDR OUTPUT shows "NOT SUPPORTED" without an HDR display (menu.cpp:392-397, 627-628). Versus hides the renderer rows (menu.cpp:315-319).

HDR scene target:
- Format `DXGI_FORMAT_R16G16B16A16_UNORM`, `kHeadroom = 8` (d3d12.cpp:72-75). This is not a float target: colour is stored divided by 8 so the UNORM clamp sits at 8x white, while alpha keeps its 0..1 clamp for the game's blend factors.
- Which targets: every colour target of back buffer size except the shadow blur chain alias (`t.fmt = hdr_ && !blur && screen ? kHdrFormat : kBgra`, d3d12.cpp:1442-1463).
- Entering HDR: at the first depth writing, non pretransformed draw of a frame into such a target with depth bound (`EnterHdrForScene`, d3d12.cpp:2977-2986), the existing contents are blitted onto themselves times 1/8 (`EnterHdr`, d3d12.cpp:1911-1920). Screen quads never start it, so loading fades draw over the last tone mapped picture.
- Per draw constants `hdr[0]` = 1/8 unless the blend multiplies with the target (src factor ZERO or DESTCOLOR), `hdr[1]` = 8 (`FillHdrConstants`, d3d12.cpp:3152-3157; layout shaders.h:72). Pixel shaders end with `outc.rgb = saturate(outc.rgb) * hdr.x` (shaders.cpp:851), so each pass is still clamped like the console and only accumulation exceeds 1.
- Reading an HDR target as a texture does not resolve: the stage gets `kTexHdr` (d3d12.cpp:2736) and the shader does `t.rgb = saturate(t.rgb * hdr.y)` (shaders.cpp:785, 839). Normalization cubes detected by `UpdateNormCube` (d3d12.cpp:2783-2792) get `kTexNormCube` under HDR (d3d12.cpp:2879) and are computed per pixel (shaders.cpp:361-365, 835-836).
- Clears divide the colour by 8 (d3d12.cpp:3606), CPU pixel uploads blit with mul 1/8 (d3d12.cpp:1646), copies from a non HDR format into an HDR target blit with mul 1/8 (d3d12.cpp:3732).

Resolve (`Resolve`, d3d12.cpp:2001-2027), run when the frame is read: `Copy` from it (d3d12.cpp:3794), `Present` (d3d12.cpp:4059), any CPU readback through `PrepareCopySource` (d3d12.cpp:4122-4123):
- Exposure: log average luminance over a 32x18 grid into a persistent 1x1 `R16_FLOAT`, blended towards the new value with `1 - exp(-dt * 1.5)` by wall clock (`kPostHlsl` `exposure`, d3d12.cpp:121-128; `UpdateExposureBlend`, d3d12.cpp:1974-1982; constants d3d12.cpp:1922-1931). Key 0.4, clamp to 0.7..1.0 with tone mapping, fixed 1.0 without.
- Bloom: quarter size `R16G16B16A16_FLOAT` pair, `bright` keeps `max(c * 8 * exposure - 1, 0)`, then two separable 9 tap gaussian passes (d3d12.cpp:129-141, 1984-1998). Strength 0.6. Bloom is added whenever HDR rendering is on, with or without tone mapping.
- `tone` (d3d12.cpp:148-158) writes back into the same target in place, values 0..1, alpha kept: with tone mapping `c * exposure + bloom`, linear to 0.75 then an exponential roll off towards 1; without it `c + bloom`, then saturate. After resolve `t.hdr` is false and `hdr_frame` stops a second entry in the same frame, so HUD draws land unscaled.

Presentation and HDR output:
- The game's gamma ramp is ignored (`SetGamma` is empty, d3d12.cpp:4377-4378); the ramp lookup is built from the `brightness` and `contrast` settings instead (`UploadGammaRamp`, d3d12.cpp:3891-3920) and applied in `BlitFrame` (d3d12.cpp:3935-3947, shader d3d12.cpp:83-108).
- HDR output: Windows HDR detected per monitor (`DetectHdr`, d3d12.cpp:3814-3827) with peak nits and SDR white nits (d3d12.cpp:214-245, defaults 1000 and 200 at d3d12.cpp:78-79); the swap chain switches to `R10G10B10A2` PQ BT.2020 (`ResizeSwap`, d3d12.cpp:3830-3848). `DrawHdrFrame` (d3d12.cpp:3992-4011) blits the gamma corrected frame into an RGBA16 target, then `hdr10` (d3d12.cpp:161-196) does gamma 2.2 to linear, inverse Reinhard on luminance up to peak / paper white, scale by paper nits, BT.709 to BT.2020, PQ encode. The ImGui panel is composited at SDR white. HDR output is independent of HDR rendering.
- Changing render scale, shadow resolution, HDR rendering or MSAA flushes and drops every target (`ApplySettings`, d3d12.cpp:4095-4119).

Glow:
- `glow.cpp:11-25` hooks the game's glow and bloom composite `sub_002EAF44` (thiscall, `textures[4]`, `coords[4]` at `esp + 8`). For each stage whose coords start `1, -1` (a chain level, not the screen copy) it subtracts `0.5/256 - 0.5/viewport.w` and `0.5/256 - 0.5/viewport.h` from the offsets. The viewport is the guest's (`d3d::State().viewport`), so at 640x480 the shift is 0.001171875 by 0.000911458 in uv (0.75 by 0.44 screen pixels): small but present in every native frame.
- The `kTexWide` box filter for glow chain reads (`kGlowBoxTexels`, d3d12.cpp:3159-3175) only applies to upscaled targets and belongs to C07.

## Browser today

- All colour textures and the canvas are `RGBA8Unorm`, depth is `Depth24PlusStencil8` (webgpu-service.cpp:841-860, 828-839); `textureBytes` assumes 4 bytes a texel (webgpu-service.cpp:89-92) against a 256 MiB budget (webgpu-service.cpp:111). The device is requested with no required features (webgpu-service.cpp:784).
- Pipelines are built for `RGBA8Unorm` only; `PipelinePacket` has no format field (webgpu-transport.h:13-19, size asserted at :43, mirrored in webgpu-transport.mjs:3). Partial clear pipelines are RGBA8 too (webgpu-service.cpp:333).
- `Copy` is `CopyTextureToTexture` (webgpu-service.cpp:664-674), which requires equal formats. Readbacks decode RGBA8 or packed depth (menu-backend.cpp:918-933). Feedback copies for self sampling go through the same copy (menu-backend.cpp:821-846).
- Presentation (webgpu-service.cpp:488-529, 1017-1040): nearest texel of the 640x480 source, per channel lookup in a 256 entry ramp built from the guest's `SetGammaRamp` (diagnostic-graphics.cpp:1263, menu-backend.cpp:1638-1645, webgpu-service.cpp:474-484, 1014-1016), into an RGBA8 `presentation_target`, then copied to the canvas. Frame capture reads that target back and expects 4 bytes a pixel (webgpu-service.mjs:95-112).
- WGSL generation has no `hdr` constant (menu-shaders.cpp:41-57) and rejects any texture flag (`shadow or HDR texture flags`, menu-shaders.cpp:589-590; menu-pixel-shaders.cpp:160-161).
- The canvas is an `OffscreenCanvas` transferred to the GPU worker (menu-shell.mjs:314-326), so display queries (`matchMedia`) must run on the main thread.
- emdawnwebgpu in the local Emscripten 6.0.13 cache supports HDR canvas configuration: `WGPUSurfaceColorManagement { colorSpace, toneMappingMode }` chained into `WGPUSurfaceConfiguration` (webgpu.h:1655-1659, `WGPUToneMappingMode_Extended` webgpu.h:782-786), translated to `context.configure({ colorSpace, toneMapping: { mode } })` in library_webgpu.js. It also exposes `WGPUFeatureName_TextureFormatsTier1` and `WGPUTextureFormat_RGBA16Unorm` (webgpu.h:429, 691).
- `sub_002EAF44` exists in generated/game/x86_func_mapping.cpp; nothing in the browser hooks it. There is no browser `d3d::State()`.

## Tasks

1. Settings entries and shim fields (see Settings and UI). Failing tests first in tests/browser/settings.test.mjs for normalization and defaults; the C01 channel fills `shader_model`, `hdr_rendering`, `tone_mapping`, `hdr_output` of the app::Settings shim.
2. Formats in the GPU service. Add a format code to `TexturePacket` (0 RGBA8, 1 HDR scene) and a `format` member to the service `Texture`; size accounting per format. Request `texture-formats-tier1` when the adapter has it and use `RGBA16Unorm` for the HDR scene (exact native semantics); otherwise `RGBA16Float`, with the differences in Risks. Report the chosen format in the adapter diagnostics. Contract test in tests/browser/webgpu-service.test.mjs: create, clear, read back an HDR texture.
3. Pipeline format. Add `color_format` to `PipelinePacket` (update the static_assert and the .mjs mirror), include it in the MenuBackend pipeline cache key, build partial clear and force opaque pipelines per format.
4. WGSL. Add `hdr: vec4<f32>` to `Constants` and fill it like `FillHdrConstants`. Final colour becomes `vec4(clamp(rgb, 0, 1) * u.hdr.x, a)` in both fixed function and combiner paths. Accept `kTexHdr` (multiply by `u.hdr.y`, clamp) and `kTexNormCube` (`normalize(dir) * 0.5 + 0.5`, alpha from the cube) in menu-shaders.cpp and menu-pixel-shaders.cpp; keep rejecting `kTexWide` until C07. Failing cases first in tests/browser/menu-shaders.cpp and menu-pixel-shaders.cpp, compared against the HLSL from `gpu::PixelHlsl` for the same keys.
5. MenuBackend HDR state. Per resource: `hdr`, `hdrFrame`. HDR format for back buffer sized colour surfaces except the C09 shadow blur alias. Port `EnterHdrForScene` and `EnterHdr` (a service blit with mul 1/8 from the feedback copy), clear colour divided by 8, CPU uploads into an HDR target uploaded as RGBA8 into a staging texture and blitted with mul 1/8. Set `kTexHdr` when sampling an HDR resource. Port the normalization cube detection or reuse a browser equivalent (open question 4).
6. Resolve. New service operation, ordered with posted draws: copy target to scratch, exposure into a persistent 1x1 `R16Float` with blend constant `a` from `UpdateExposureBlend` (computed in MenuBackend, wall clock), bright into 160x120 `RGBA16Float`, two blur passes, tone back into the target. WGSL ports of `exposure`, `bright`, `blur`, `tone`. MenuBackend calls it before `Present`, before `Copy` from an HDR source, and before every readback of an HDR resource (`Sync`, `SyncLate`, `RecordLate`, `DeliverLate`, the readback in `ReleaseResource`). Mark the target dirty and bump its version.
7. Mixed format copies and readbacks. `copy()` falls back to a blit when formats differ (mul 1/8 when the destination is in HDR). Readback of an HDR format texture converts to RGBA8 bytes in the service (a pack pass like `pack_depth`, webgpu-service.cpp:367-418) so `EncodeSurface` is unchanged. Feedback textures take the source format.
8. Safe switching. HDR rendering changes every scene pipeline and target format. Latch `hdr_` only at a present boundary while the mission is not stepping (front end, pause menu, or level load); on change, resolve, convert existing back buffer sized resources by blit and drop cached pipelines for the old format. Measure the pipeline rebuild count; if a live switch remains, reset the `MissionStepper` backlog (mission-stepper.h:18) on that frame so the 2 s watchdog (mission-stepper.h:16, 34-36) does not trip. Tone mapping is a uniform only and switches live.
9. HDR output. Main thread: `matchMedia('(dynamic-range: high)')` with a change listener, sent to the GPU worker. Worker: when HDR output is on and the display is HDR, `Configure` uses `RGBA16Float` with `WGPUSurfaceColorManagement { SRGB, Extended }`; confirm through an `EM_JS` call of `getContext('webgpu').getConfiguration().toneMapping?.mode === 'extended'`, else reconfigure SDR and report unsupported. Presentation pipeline variant: ramp lookup, `pow(c, 2.2)`, inverse Reinhard on luminance up to the headroom, sRGB encode extended past 1, into an `RGBA16Float` presentation target. Frame capture (`Readback` of resource 0) renders an SDR RGBA8 copy on demand so captures and the perf harness keep working.
10. Glow hook. Compile src/game/glow.cpp unchanged through C01. Provide a browser `d3d::State()` (shared with C11 and C09) returning the live `DiagnosticGraphics` state; the viewport is current after `SetViewport` (diagnostic-graphics.cpp:618). Contract test: fake guest stack with four coord quads, two starting `1, -1`, check only those two move by the values above.
11. Diagnostics. Add HDR state (format, resolves per frame, exposure value) to the menu backend snapshot so tests and the debug channel can assert it.
12. Measure and compare (see Testing) and record results in docs/browser-port/06-playable-mission.md style evidence.

## Settings and UI

settings.mjs entries:
- `shaderModel`: options `['1.1', '3.0']`, initial `'1.1'` (native default 1). Maps to `shader_model` 1 or 3.
- `hdrRendering`: boolean, initial `false`. `toneMapping`: boolean, initial `false`.
- `hdrOutput`: boolean, initial `true` (native default; only acts on an HDR display with a supporting browser).
- `hdrHeadroom`: number 1 to 10, step 0.5, initial 5 (browser only: browsers expose no peak luminance; 5 is native's fallback 1000 / 200). Open question 6.

Placement: a Graphics tab in `TABS` (menu-shell.mjs:10-28) shared with C07, C08, C09, C11; whichever chunk lands first creates it. Rows: Shader model, HDR rendering, Tone mapping, HDR output, HDR highlights. HDR rendering and Tone mapping show "Off" and are dimmed at 1.1; HDR output shows "Not supported" when detection fails; HDR highlights only shows with active HDR output. The row model needs a disabled state and a dynamic label (new in menu-shell.mjs).

Flow: settings.mjs to the C01 host-settings channel to the app::Settings shim, read by MenuBackend at present boundaries (task 8). `hdrOutput`, `hdrHeadroom` and the display flag go to the GPU worker as a presentation configuration, not through the guest. Versus hides these rows as native does.

## Acceptance criteria

- With shader model 1.1, or 3.0 with HDR rendering off, frames are byte identical to today on the opening capture (no format, shader or constant change leaks in).
- With HDR rendering on, the opening mission and the control view render without service errors; bright multi pass lit surfaces exceed the 8 bit result and bloom appears around highlights; with tone mapping on, exposure adapts over about a second.
- Copies from the scene (the game's screen copies and glow chain), guest readbacks and frame capture return correct RGBA8 data after resolve; no "invalid texture copy region, format or alias" errors.
- Glow chain offsets match native within 1e-6 in the contract test; glow placement matches a native 640x480 capture.
- HDR output on Chrome with an HDR display shows highlights above SDR white; elsewhere it falls back to SDR and the row says "Not supported".
- Mean frame interval with HDR rendering and tone mapping on stays within 1 ms of off in bench.mjs; switching HDR rendering never trips the mission watchdog.

## Testing

- Unit and contract tests (failing first): settings normalization; WGSL output for `kTexHdr`, `kTexNormCube` and `hdr` scaling against `gpu::PixelHlsl` for the same keys; MenuBackend packet sequence (EnterHdr before the first depth written draw, Resolve before Present, Copy and Sync of an HDR target, none for screen quads); glow coordinate hook; resolve math as a CPU reference in JS for a 4x4 input compared with a real GPU run in tests/browser/webgpu-service.html.
- Visual: agent-browser against a separate build directory (never out/boot-m5), muted with out/front-end/mute.js, development start, captures at world time 30 and 60 for each combination (1.1; 3.0 HDR; 3.0 HDR plus tone mapping). Native references must come from the user: F3 capture on the Windows build (`captures/<stamp>/final` and the `_hdr_` pre resolve dumps, d3d12.cpp:2002-2006) at 640x480 with render percent 100. Compare by eye and by mean luminance and highlight histograms, not byte equality.
- Performance: out/perf-60/bench.mjs `<web-dir> <port> <out.json> <label>` A/B, HDR off vs on vs on plus tone mapping, idle host, two interleaved pairs; record pipeline counts from the snapshot. Expected GPU cost at 640x480 is small (one 640x480 copy, five small passes, 2.4 MiB extra per HDR target) but per frame packet and copy overhead must be measured.
- Per draw issues: use the out/boot-debug technique.

## Risks and open questions

1. `RGBA16Float` fallback differs from native UNORM: values above 8x white and alpha above 1 are not clamped, so additive passes followed by DESTALPHA blends can differ. Is `texture-formats-tier1` (with `RGBA16Unorm` renderable, blendable and filterable) available in Chrome on M1 Max and in Safari? Verify before choosing the default path.
2. Opcode numbers: C07, C08, C09, C11 also add service operations; allocate numbers once at merge time and keep webgpu-transport.h, webgpu-transport.mjs and the protocol tests in step.
3. Mid mission HDR switch rebuilds roughly every scene pipeline (about 120) and may exceed `maxPipelines` 256 (menu-backend.h:16) while both formats are cached.
4. The browser builds normalization cubes from GPU copies of faces (docs/browser-port/06-playable-mission.md, dark models fix), so `UpdateNormCube`'s pixel check may not see the data at upload; find where to detect.
5. HDR canvas support: Chrome ships `toneMapping`; Safari and Firefox support is unverified. `getConfiguration()` may be missing on older versions; treat missing as unsupported. CSS `dynamic-range-limit` on the page must not cap the canvas.
6. No web API gives peak luminance; `hdrHeadroom` is a guess the user tunes. Chrome clamps extended values to the display's real headroom, which may clip the expansion.
7. Native replaces the game's gamma ramp with brightness and contrast (d3d12.cpp:3891-3920, 4377-4378); the browser applies the guest ramp. Which chunk owns that parity is unassigned; this chunk keeps the guest ramp and only makes the presentation format aware.
8. Does a `C09` shadow blur alias ever have back buffer size in the browser? The HDR format must not reach it.
9. Wall clock exposure in a worker can jump after tab throttling; clamp `dt` to 0.25 s as native does.

## Out of scope

- Render scale and the `kTexWide` glow box filter (C07); MSAA and SMAA (C08); soft shadows (C09); ambient occlusion passes (C11), beyond writing into whatever format the scene has.
- Brightness and contrast sliders replacing the game ramp (open question 7).
- An in-canvas settings panel; the browser UI is HTML and composited by the browser.
- PQ / BT.2020 output; the browser path is extended sRGB only.
