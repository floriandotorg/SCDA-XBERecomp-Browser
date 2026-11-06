# C07: Render scale (internal resolution) in the WebGPU backend

Class: Complex
Depends on: none (no game hooks). Uses the C01 host-settings channel to carry the setting and the output size if it
has landed; otherwise task 2 adds a minimal render-settings word. Shares the canvas sizing contract with
C06-widescreen-and-fov.
Native sources: src/gpu/d3d12.cpp, src/app/settings.cpp, src/app/menu.cpp, src/gpu/shaders.cpp, src/gpu/shaders.h,
src/gpu/backend.h, src/game/shadows.cpp

## Goal

Render the guest's screen sized targets at more (or fewer) pixels than the console's 640x480 and present them sharply
at the browser canvas's real device resolution, as the PC port's RENDER RESOLUTION does, while every guest-visible
surface (locks, readbacks, save thumbnails, depth imports, copies into CPU memory) keeps its console size and
contents. Offer a fixed percentage, a window-matched mode and an optional dynamic mode, all measured with the perf
harness.

## Native behaviour

Scale and setting
- `RenderScale(guest_h) = output_h * render_percent / 100 / guest_h`, at least 0.1 (src/app/settings.cpp:295-298). One
  factor for both axes; the frame's shape comes from the final blit (see C06). RENDER RESOLUTION steps 20 to 200
  percent by 5 and shows the resulting size (src/app/menu.cpp:490-497).
- `LoadSettings` latches it (src/gpu/d3d12.cpp:1325-1334); `ApplySettings` runs every Present, and on a change flushes
  and drops all targets, which are rebuilt lazily at the new size (d3d12.cpp:4095-4121).

Which targets scale
- `CreateTargetResource`: every render target, colour or depth, gets `t.w = gw * scale_`, except the shadow map, which
  gets `shadow_resolution / gw` (default 512, the console's, so 1x) (d3d12.cpp:1441-1446, 70). The shadow map and its
  aliased swizzled blur chain are identified by `gpu::g_shadow`, written by the src/game/shadows.cpp hook
  (src/gpu/backend.h:87-93). MSAA applies only to guest back buffer sized targets (d3d12.cpp:1451-1454). Textures the
  game uploads are never scaled.

Drawing at scale
- Viewport covers the whole host target; the guest viewport stays in shader constants in guest pixels
  (`FillTransformConstants`, d3d12.cpp:3228-3243); the scissor is the guest viewport and scissor times `W / gw`
  (`SetViewportScissor`, d3d12.cpp:3458-3477).
- Clears: guest rects through `ScaledRect` (d3d12.cpp:3575-3580, 3582-3610).
- Slope scaled depth bias is per console pixel, multiplied by `bound_ds_->w / gw` (d3d12.cpp:3084-3089).
- Copies: same format `CopyByRegion` scales rects by the destination's factor (d3d12.cpp:3741-3775); different formats
  or scales go through `CopyByDraw` with `taps = ceil(src_scale / dst_scale)` (d3d12.cpp:3706-3739).
- Sampling an upscaled target into a smaller target (`k = t.w / t.gw > kUpscaledMin = 1.01`, sampled while drawing a
  target with smaller guest size) sets `kTexWide` (d3d12.cpp:2714-2736); `FillTexelConstants` sets `spread` to average
  a 3 console texel box per tap (`kGlowBoxTexels`, d3d12.cpp:3159-3174); the HLSL `spreadtap` takes `n x n` taps
  (src/gpu/shaders.cpp:446-455; flag and constant at src/gpu/shaders.h:22, 73). Render targets are sampled half a
  texel back in guest units (`texsize.zw = -0.5 / size`).
- `UnitTexcoords`: small draws whose float2 inputs all lie in 0..1 get clamp samplers when a stage wraps, because
  upscaled pixel centres reach the far half texel and wrap the opposite edge in (crosshair ticks)
  (d3d12.cpp:3409-3453).

Guest-visible surfaces stay console sized
- `Sync` reads a target back through `PrepareCopySource`, which `Shrink`s a scaled (or HDR) target to guest size first
  (d3d12.cpp:4122-4153, 4262). `SyncLate`/`RecordLate` keep a ring of shrunk copies (d3d12.cpp:4157-4260).
- `UploadTarget` decodes CPU-written guest pixels at guest size and blits them up into the scaled target; after a Sync
  only the pixels the CPU changed are drawn over, so the high resolution picture survives (d3d12.cpp:1628-1653).
- Presentation: `BlitFrame` scales the host-size frame into the swap chain viewport (d3d12.cpp:3935-3948).

## Browser today

- One `Resource` per guest data address and colour/depth kind, used both as sampled texture and as render target; its
  GPU texture is always `desc.width x desc.height` (`ensure`, src/browser/menu-backend.cpp:669-767; `surface` sets
  `renderTarget`, 769-774). CPU changes are detected by write version or byte compare and recreate the whole texture
  from guest memory (menu-backend.cpp:713-725, 736-767).
- Draw: `texsize` in guest texels with the half texel offset for targets (menu-backend.cpp:1179);
  `BuildConstants(state, target.desc.width, target.desc.height, ...)` sets `surf` (menu-backend.cpp:1327,
  src/browser/menu-shaders.cpp:474-481); viewport packet is the whole target and the scissor is guest pixels
  (menu-backend.cpp:1339-1351); colour and depth must match in size (menu-backend.cpp:1206); depth bias slope is
  applied per host pixel with `dpdx` (menu-backend.cpp:1328-1334, menu-shaders.cpp:673).
- Shadow and border sampling compute texel positions from `textureDimensions` of the real texture
  (menu-shaders.cpp:97-160), so they follow a host size automatically.
- The pixel shader translator consumes `gpu::PixelKey`/`TexInfo` from the shared header but generates its own WGSL and
  rejects any `TexInfo.flags` (src/browser/menu-pixel-shaders.cpp:159-161). The shared HLSL `spreadtap` is not used by
  the browser.
- Clear: guest rects (menu-backend.cpp:1458-1506); the service clamps rects to the texture
  (src/browser/webgpu-service.cpp:646-656).
- Copy: regions in guest pixels via `copyTextureToTexture`; the service checks them against the texture size
  (menu-backend.cpp:1508-1569, webgpu-service.cpp:664-674). Feedback copies are whole-texture copies at `desc` size
  (menu-backend.cpp:821-846).
- Depth import: `UploadDepth` requires the payload size to equal the texture size (menu-backend.cpp:775-818,
  webgpu-service.cpp:967-1012).
- Readback: whole texture (webgpu-service.cpp:1059-1140); `decodeReadback` requires exactly `desc.width * desc.height
  * 4` bytes (menu-backend.cpp:918-935); async slots refuse more than 4 MiB (webgpu-service.cpp:153-158) and packets
  are capped at 4 MiB (src/browser/webgpu-transport.h:7). Frame capture refuses canvases over 4 MiB
  (src/browser/webgpu-service.mjs:103).
- Budgets: GPU `TextureBudget` 256 MiB including the presentation target (webgpu-service.cpp:111, 522-528);
  `MenuBackendLimits` count WASM-side mirrors at `desc` size (src/browser/menu-backend.h:10-16; 128 MiB set in
  src/browser/boot-wasm.cpp:159-160).
- Present renders into `presentation_target` (canvas size), then copies it to the surface texture every frame
  (webgpu-service.cpp:1017-1047); canvas size comes only from Configure at `Init` (menu-backend.cpp:1030-1036).
- `Occlusion`, `FilterShadows`, `ApplyShadows` throw (menu-backend.cpp:1646-1648), so the native SM3 soft shadow and
  SSAO scale paths do not apply.

## Tasks

1. Pure sizing helper with failing tests first (C++ host test beside the existing menu-backend tests): input guest
   size, back buffer size, mode, percent, output height, device max texture size; output host size per resource and
   the factor. Cases: console mode is identity; 100% of a 1080 row canvas gives 1440x1080 for 640x480; 50% of 720 rows
   gives 480x360; clamps to 8192 and to the adapter limit; non-screen sizes stay 1x.
2. Setting transport: carry `resolution`, `resolutionScale` and the canvas device size (CSS box times
   `devicePixelRatio`, from menu-shell.mjs `layout()`) to `MenuBackend` through the C01 channel, or a small shared
   word block if C01 is not there yet. Latch at Present.
3. Scale rule, phase 1: scale a resource only when it is a render target (or a copy destination) whose guest size
   equals the device back buffer size, colour and depth alike. Everything else, including the 512x512 shadow map,
   stays 1x. Phase 2 (only after measuring): all render targets except depth-only targets of a non screen size,
   mirroring native's "all but the shadow map" without `g_shadow`.
4. `Resource` gains `hostWidth/hostHeight`; GPU textures for scaled resources are created at host size. Resident byte
   accounting (`residentBytes`, `trimFeedback`) and the GPU `TextureBudget` count host bytes.
5. New service opcode `Blit` (render pass, source rect to destination rect, linear or point, optional `taps` box
   filter, colour and a depth variant writing `frag_depth`). It serves tasks 6, 9, 11, 12 and 13.
6. CPU content into a scaled target: create a guest-size staging texture from the decoded bytes and `Blit` it up. When
   the change follows a `Sync`/`DeliverLate` publish, upload only the rows that differ from `item.uploaded` so the
   high resolution picture is not replaced by its downsampled copy (native "over"). Promote a resource first seen as a
   1x texture the same way when it becomes a render target.
7. Draw: viewport packet at host size, scissor scaled with native's truncation, `surf` and `texsize` stay guest,
   `depth_bias.y *= host / guest`.
8. Clear rects scaled in `MenuBackend::Clear` with `ScaledRect` semantics.
9. Copy: equal factors scale the regions; different factors use `Blit` with `taps = ceil(src_k / dst_k)`. Feedback
   copies at host size.
10. Wide sampling in WGSL: compute native's `kTexWide` condition in `MenuBackend::Draw`, allow exactly that flag in
    menu-pixel-shaders.cpp and the fixed function path in menu-shaders.cpp, emit a WGSL `spreadtap` equivalent with
    `textureSampleLevel`, and append `spread: array<vec4<f32>, 4>` at the end of `ShaderConstants` (keep the existing
    offset `static_assert`s at menu-shaders.cpp:16-19 valid). Port `UnitTexcoords` for scaled targets, limited to
    draws whose vertices the backend already holds on the CPU.
11. Readback: `Sync`, `RecordLate`, `DeliverLate` of a scaled colour target `Blit` it to a guest-size temporary (box
    filter) and read that; depth uses a point sampled depth `Blit` into a guest-size depth texture before the existing
    pack path. Payload sizes then match today's, inside the 4 MiB slots.
12. Depth import into a scaled depth target: upload at guest size and `Blit` (point) up, or extend the `UploadDepth`
    shader to index `floor(position / k)` with `k` in its parameters.
13. Setting change: at the next Present, resample every scaled resource into its new size with `Blit` (contents kept),
    then switch; no GPU-only content is lost, unlike native's drop.
14. Canvas and presentation: Console mode keeps C06's `round(480 * aspect) x 480` canvas. Match window sets the canvas
    to the device size from task 2 (capped at 8192 and by budget) via Configure; the presentation pass samples
    linearly when sizes differ. Render straight into the surface texture and draw `presentation_target` only while a
    capture is pending, to avoid a full-canvas copy every frame at large sizes.
15. Frame capture at large canvases: read back a guest-size or half-size downscale, or tile the readback, so
    `capture()` keeps working above 4 MiB.
16. Dynamic resolution (optional, last): a controller in `MenuBackend` driven by the Present wait (time until the GPU
    worker answers with one frame in flight) and frame interval, lowering or raising `resolutionScale` in 5% steps
    with hysteresis (for example lower after 1 s above 17.5 ms p90, raise after 3 s below 14 ms), bounded by the
    user's percent as a ceiling. Keep its floor above `kUpscaledMin` or warm both `kTexWide` variants so steps never
    change pipeline keys.
17. Options rows and value labels as below.

## Settings and UI

New entries in src/browser/settings.mjs, Display tab, below Aspect ratio (C06) and above Upscaling:
- `resolution`: options `console`, `window`, `dynamic`; initial `console` (today's cost). Label "Resolution": "Console
  (480 lines)", "Match window", "Dynamic".
- `resolutionScale`: 50 to 200, step 5, initial 100. Label "Resolution scale", shown as "100% (1440x1080)" from the
  helper of task 1, greyed in Console mode; in Dynamic it is the ceiling.
Upscaling (`filter`) keeps choosing the CSS `image-rendering` in Console mode and the presentation sampler whenever
render and canvas sizes differ. Pixel perfect stays meaningful in Console mode; in Match window the canvas is already
device sized.
Path: settings.mjs to menu-shell.mjs (also measures the device size) to the host-settings channel to `MenuBackend`,
which sends Configure and resizes resources at a Present boundary.

## Acceptance criteria

- Console mode: captures and perf identical to today; no new opcodes on the hot path.
- Match window at 1920x1080 device pixels (16:9 with C06): edges visibly sharper, HUD text and crosshair without
  wrapped edge lines, glow and night vision as wide and bright as at 1x, scope blur and loading pictures correct.
- Every lock and readback returns console-sized data: quick save thumbnail in the load menu matches a 1x run visually;
  late readbacks keep their delivery behaviour; depth readback tests still pass.
- Switching resolution mid-mission causes no validation error, no watchdog trip and no lost picture.
- GPU texture bytes stay within the budget at the largest offered size, reported in the diagnostics JSON.

## Testing

- Unit first: the sizing helper (task 1); `ScaledRect` and scissor truncation; copy region scaling and taps;
  `kTexWide` decision table taken from native `BindTargetTexture`.
- Service tests (WEBGPU_SERVICE_TESTING build): `Blit` colour box filter against a CPU reference, depth point `Blit`
  exactness for D24 and D16 values, readback of a scaled target equals the 1x readback for flat colour content.
- WGSL: `spreadtap` output equals the HLSL formula on synthetic textures; all existing synthetic shaders still
  compile.
- agent-browser with muted audio (out/front-end/mute.js), separate build directory (never out/boot-m5): title, mission
  start, OPSAT, sniper scope, night and thermal vision, quick save then load menu thumbnail, each at Console, 100% and
  200%.
- Per-draw debug channel (out/boot-debug technique) to confirm which resources were scaled and that `kTexWide` draws
  are the glow and blur chains only.
- Perf: `node out/perf-60/bench.mjs <webdir> <port> out/perf-60/c07-<mode>.json c07-<mode>` for console, 100% at 1080
  rows, 150% and 200%; report mean, p95, command and readback waits, and `webgpu_resources` bytes. Run a soak
  (`PERF_SOAK`) for the dynamic mode.

## Risks and open questions

- How many back buffer sized resources exist in a mission (front, back, depth, scene copies, feedback copies)? Each
  costs `s^2 x 1.2 MB`; at 4.5x (2160 rows) that is about 25 MB each, so ten of them exceed the 256 MiB budget. Count
  them with the resource inventory before choosing the cap; WebGPU exposes no VRAM size.
- GPU cost grows with `s^2` for fill, feedback copies and the presentation; today's 60 FPS margin on an M1 Max is
  mostly CPU, but weaker GPUs will drop frames. Dynamic mode mitigates only if the Present wait is a reliable GPU
  signal.
- Partial CPU writes after a readback (task 6) are guessed from native; which guest paths do that in this game is
  unknown.
- Which surfaces does the game read back (save thumbnail source, any depth reads)? Confirm with the debug channel
  before relying on point sampled depth shrink.
- Odd guest rects scaled by fractional factors can leave one-pixel seams between adjacent clears or copies; native
  truncates the same way, verify visually.
- Changing scale crosses `kUpscaledMin` and changes pipeline keys for glow passes; with about 120 pipelines a full
  recompile mid-mission can trip the 2 s `MaximumBacklog` (src/browser/mission-stepper.h:16, 100 ns ticks). Apply
  changes while the settings panel holds the game, or warm the variants.
- Phase 2 scaling needs a way to tell the shadow map apart without the shadows.cpp hook.

## Out of scope

- MSAA, SMAA, HDR rendering, SSAO and SM3 soft shadows (native only, and the browser backend rejects the latter
  three).
- The widescreen shape itself, hooks and pillarbox (C06).
- Anisotropic filtering and shadow resolution settings.

## Implementation notes

What C07 built (phase 1 of the scale rule).

### Settings and sizing

- settings.mjs: `resolution` (console, window, dynamic; Display tab "Resolution": Console (480 lines), Match window,
  Dynamic) and `resolutionScale` (50 to 200 by 5, shown with the render size it gives). In-game RECOMP SETTINGS >
  DISPLAY rows RENDER RESOLUTION and RESOLUTION SCALE.
- Host settings words 10 (mode), 11 (`render_percent`) and 12, 13 (the frame box in device pixels, computed by
  menu-shell's `layout()`).
- `src/browser/render-scale.h`: `Compute` (Console keeps C06's `round(480 * aspect)` x 480 canvas at 1x; Match window
  and Dynamic size the canvas to the device pixels and scale by `rows * percent / 100 / 480`, clamped to 0.5 to 3, so
  a target is at most 1920 x 1440), `HostSize` (only targets of the back buffer's size scale) and `Dynamic` (a step
  of 5% down after a second averaging above 17.5 ms, up after three seconds below 14 ms, between half the ceiling and
  the ceiling). Contract test `tests/browser/render-scale.cpp`. boot-wasm's output source applies it once a Present.

### Backend

- `Resource::hostWidth/hostHeight`: screen sized render targets (colour and depth) get the scaled size; everything
  else, the shadow map included, stays 1x. A resource first sampled as a texture is promoted (resampled) when it
  becomes a target; Draw resolves its targets before its textures so a promotion never orphans a bound feedback copy.
- New service opcode `Blit` (source rectangle over a destination rectangle: colour box filtered when shrinking,
  bilinear otherwise, optional opaque alpha; depth nearest, stencil not written). CreateTexture accepts no pixels for
  a single level colour texture (a target the backend fills itself).
- CPU content into a scaled target: a console sized staging upload blitted up; after a readback only the rows the CPU
  changed are drawn over (native's UploadTarget "over"). Non-uniform depth imports index the console sized values from
  the scaled target (UploadDepth's shader takes the ratio).
- Draw: viewport at the host size, scissor truncated as `ScaledRect`, slope scaled depth bias multiplied by the
  scale, `surf` and `texsize` stay in guest units. Clear rectangles are scaled per target. Copies between equal scales
  scale their regions; between different scales they blit.
- Readbacks (`Sync`, `RecordLate`/`DeliverLate`, a dirty release) read a console sized blitted copy, so payloads keep
  today's sizes; a scaled depth target's readback has stencil 0.
- A scale change at a Present resamples every render target to its new size, keeping its picture (native drops them).

### Checked in the browser (out/parity, 1280x720, development start)

- Match window at 100%: canvas 1280 x 720, targets 960 x 720, visibly sharper edges, no validation errors.
- Switching to 50% mid-mission resampled to 480 x 360 without a validation error, a fault or a lost picture.
- Console mode unchanged (853 x 480 canvas, no errors).

### Not done

- `kTexWide` wide sampling (task 10) and `UnitTexcoords`: glow and blur chains sampling an upscaled target take
  fewer console texels per tap, so glow and night vision may look thinner above 1x, and wrapped edge texels can show
  on small HUD quads.
- Rendering straight into the surface texture (task 14) and frame capture above 4 MiB (task 15): `capture()` refuses
  canvases larger than 1024 x 1024 pixels' worth of bytes in Match window.
- The perf harness runs, the 200% and dynamic soak, and the quick save thumbnail check were not run.
