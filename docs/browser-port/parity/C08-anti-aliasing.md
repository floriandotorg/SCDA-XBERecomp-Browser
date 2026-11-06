# C08: Anti-aliasing (MSAA and SMAA)

Class: Complex
Depends on: C01 (settings channel and app::Settings shim); coordinates with C07 (render scale)
Native sources: src/gpu/d3d12.cpp, src/gpu/smaa_shader.h, src/gpu/smaa_textures.h, src/app/settings.cpp, src/app/app.h, src/app/menu.cpp

## Goal

Give the browser port the two anti-aliasing options of the native PC port: multisampling of the scene
sized targets (MSAA) and SMAA over the finished frame, HUD included. Both must be off by default with
zero added cost, and when on must keep every existing consumer of a target (sampling, CopyRects,
readbacks, released-surface readback, presentation) working on single-sample data.

## Native behaviour (cite file:line)

Settings
- `app::Settings::msaa` (0 off, else samples) and `smaa` (bool): src/app/app.h:46-47, loaded at
  src/app/settings.cpp:91-92. Menu values are OFF, 2x, 4x, 8x: src/app/menu.cpp:41-42, 541-543.
- MSAA is clamped to what the adapter supports for BGRA, the HDR format and D24S8 together, stepping
  down: `Samples()` src/gpu/d3d12.cpp:1548-1561. Latched at `LoadSettings()` 1326-1334.
- A changed MSAA value at runtime flushes and drops every target, which are then recreated at the new
  sample count: `ApplySettings()` src/gpu/d3d12.cpp:4093-4118 (called after each Present, 4085).

MSAA target rule
- Only "screen" targets, those whose guest size equals the back buffer, get samples; colour and depth
  alike; the game's own maps and chains stay single-sample: src/gpu/d3d12.cpp:1447-1453.
- A depth whose sample count differs from the colour target is not bound: 1564-1567.
- The pipeline key carries the sample count: src/gpu/d3d12.cpp:3115-3116.
- Reads of a multisampled colour target go through a single-sample copy resolved on demand and cached
  until the next write (`EnsureFeedbackCopy`, `ReadableResource`): src/gpu/d3d12.cpp:1875-1907.
  Multisampled depth reads are skipped with a log line: 1905-1906.
- CPU uploads into a target are drawn across with a blit pipeline that has the target's sample count:
  1630-1648. CopyRects into a multisampled destination is drawn across instead of copied:
  src/gpu/d3d12.cpp:3800-3803.
- Post passes that read depth (soft shadows, AO) have `#ifdef MS` variants using `Texture2DMS`:
  src/gpu/d3d12.cpp:2145-2151, 2329, 2347-2353.

SMAA
- Configuration prepended to the reference shader: threshold 0.05, 16 search steps, 8 diagonal steps,
  corner rounding 25, luma edge detection, linear and point samplers: src/gpu/d3d12.cpp:248-269.
- Pass wrappers (fullscreen triangle VS, edges, weights, blend): src/gpu/d3d12.cpp:272-304.
- Reference shader text with its licence: src/gpu/smaa_shader.h:1-30 (`kSmaaHlsl`, 1372 lines).
- Lookup data: area 160x560 RG8 (`kSmaaArea[179200]`, src/gpu/smaa_textures.h:35) and search 64x16
  R8 (`kSmaaSearch[1024]`, src/gpu/smaa_textures.h:5638); uploaded at src/gpu/d3d12.cpp:2054-2066.
- Targets: edges R8G8, weights R8G8B8A8, blended output in the frame's format, at the frame's size:
  src/gpu/d3d12.cpp:2029-2031, 2069-2087. Edges are cleared each frame because edge detection
  discards: 2091-2096. Metrics are {1/w, 1/h, w, h}: 2094.
- Runs on the presented target at its host size, before the gamma ramp and the scale to the window:
  `PrepareShownFrame` src/gpu/d3d12.cpp:3923-3932, then `BlitFrame` with gamma 3935-3947.
- Applies live (checked every frame). Combines with MSAA (app.h:47 comment).

## Browser today (cite file:line; where the work plugs in)

- Guest D3D8 HLE hands draws, clears, copies and Present to `MenuBackend`
  (src/browser/diagnostic-graphics.cpp:1269-1296 for Swap).
- `MenuBackend` resources are keyed by guest data; colour targets are RGBA8, depth is D24S8. The
  backbuffer size is the `Init` size, fixed 640x480: src/browser/menu-backend.cpp:1030-1036.
- Pipeline key is the raw `PipelinePacket` bytes plus shader serial: src/browser/menu-backend.cpp:1236-1318.
  `PipelinePacket` has no sample count (src/browser/webgpu-transport.h:13-19, size asserted 112 at :43).
- GPU service: every texture is created with sample count 1 (src/browser/webgpu-service.cpp:840-878);
  pipelines have no `multisample` state (881-952); draws open passes with `loadOp=Load`,
  `storeOp=Store`, batched while colour and depth stay the same (530-610, batch pass 20-25, 590-593).
- Other single-sample-only paths that MSAA touches: partial clear pipelines (302-349), opaque alpha
  fix-up after copies (350-366), depth packing for readback (`pack_depth` 367-419), CPU depth import
  (`prepare_depth_upload` 420-473 and `UploadDepth` 981-1012), `copyTextureToTexture` copies (664-674),
  readbacks via `copyTextureToBuffer` (1059-1095), feedback copies used when a draw samples its own
  target (src/browser/menu-backend.cpp:821-846).
- Presentation: `Present` binds the source with `textureLoad` and the 256x1 gamma ramp, draws into a
  canvas-sized intermediate and copies it to the surface: src/browser/webgpu-service.cpp:488-528,
  1017-1047. SMAA plugs in between the source and this pass.
- Texture budget: 256 MiB in the service (`TextureBudget`, webgpu-service.cpp:111), counted at 4 bytes
  per texel (89-93). Mission renderer limits: 512 resources, 128 MiB resident
  (src/browser/boot-wasm.cpp:157-170). `maxPipelines` keeps its default 256 (menu-backend.h:16).
- Settings UI: src/browser/settings.mjs:1-10 and tabs in src/browser/menu-shell.mjs:10-28.

## Tasks (numbered, concrete, each verifiable)

MSAA

1. Settings: add `msaa` (0 or 4) and `smaa` (bool) to the C01 shim mapping onto `app::Settings::msaa`
   and `::smaa`. Values other than 0 and 4 normalise to 4 when above 1 (WebGPU core only guarantees
   sample counts 1 and 4; native 2x and 8x are not offered). Verifiable by a settings.mjs unit test.
2. Transport: add `uint32_t sample_count` to `PipelinePacket` and a `samples` field to
   `TexturePacket`; update the static_asserts. The packets are only encoded in C++ (no layout in
   src/browser/*.mjs). The service rejects anything but 1 or 4.
3. Service textures: a texture created with `samples = 4` gets a companion multisampled texture
   (usage RenderAttachment and TextureBinding, one level) next to its single-sample texture. The
   single-sample texture stays the resource every other path uses. Track two flags per texture:
   `resolve_pending` (multisampled newer than single) and `seed_pending` (single newer than
   multisampled). Count the companion as 4x bytes against `TextureBudget`.
4. Service draws and clears: when the colour or depth target has a companion, attach the companion.
   Colour keeps `storeOp=Store` so later passes can load it; set `resolve_pending` on pass end. A full
   clear clears the companion and, through `resolveTarget`, the single texture in the same pass.
   Partial clears, depth import and opaque alpha fix-up get 4x pipeline variants (index the existing
   arrays by sample count).
5. Lazy resolve: a single helper `readable(Texture&)` runs before any consumer of the single texture:
   draw sampling, feedback and region copies, readback, presentation, SMAA. For colour it records an
   empty render pass on the companion with `loadOp=Load` and `resolveTarget` set. For depth (no depth
   resolve in WebGPU) it draws a fullscreen pass that reads sample 0 from
   `texture_depth_multisampled_2d` and writes `frag_depth`; `pack_depth` gets the same multisampled
   variant so readbacks never need the single depth first. Decide the depth policy in open question 2.
6. Seeding: writes into the single texture by copy destination, `CreateTexture` initial contents or
   CPU upload set `seed_pending`; the next draw into the target first blits single to companion with
   a 4x fullscreen pipeline (every sample receives the texel), mirroring native
   src/gpu/d3d12.cpp:1630-1648 and 3800-3803.
7. MenuBackend rule: a resource is multisampled when MSAA is latched on and its guest size equals the
   `Init` size and it is used as a colour or depth attachment, matching native 1447-1453. Colour and
   depth of a draw must agree, else the draw fails loudly (native silently drops depth at 1567; the
   browser already requires matching dimensions at menu-backend.cpp:1206). Set
   `pipeline.sample_count` from the attachment.
8. Latch MSAA at session start (`MenuBackend::Init`), not per frame. Mid-mission changes would rebuild
   roughly 120 pipelines and can trip the 2 s `MaximumBacklog` watchdog
   (src/browser/mission-stepper.h:16, 35-37). The Options row shows "applies on next start".
9. Soft shadow filter (C09) and any later depth reading pass must use the multisampled depth view
   when the depth has a companion; expose a `depth_view(Texture&, bool multisampled)` helper for C09.

SMAA

10. Port `kSmaaHlsl` with the native configuration to WGSL as a new shared header
    (`src/gpu/smaa_wgsl.h`, keeping the original licence text). Only the paths native uses: luma edge
    detection, blending weight calculation with diagonal and corner detection, neighbourhood
    blending. Replace every implicit-derivative sample (`SMAASamplePoint`, `SMAASample`) with
    `textureSampleLevel(..., 0.0)` so WGSL uniformity analysis accepts the loops and the discard.
    Constant offsets map to the `offset` argument of `textureSampleLevel`.
11. Service: include src/gpu/smaa_textures.h in webgpu-service.cpp, create `rg8unorm` 160x560 and
    `r8unorm` 64x16 lookup textures on first use, plus three targets (rg8unorm edges, rgba8unorm
    weights, rgba8unorm output) sized to the presented source, recreated when it changes size. Add
    the header to the hashed sources in tools/browser/inventory.mjs:133-136.
12. Present: add `uint32_t smaa` to `PresentPacket` (webgpu-transport.h:40). When set, run edges
    (cleared each frame), weights, blend on the readable source, then feed the blended texture into
    the unchanged presentation pass, so the gamma ramp still applies after SMAA as in native.
13. MenuBackend `Present` reads `app::Get().smaa` each frame (live, like native 3928) and sets the flag.
14. Pipelines for the three SMAA passes are created once; creation happens on the first enabled
    frame. Prefer `wgpuDeviceCreateRenderPipelineAsync` and present without SMAA until ready, so a
    mid-mission toggle never stalls the guest.

Shared

15. Add a Graphics tab to the Options panel with both rows (see below) and wire the values through
    the C01 channel.
16. Extend the diagnostics overlay evidence with `msaa` samples in use and `smaa` state so captures
    are self-describing.

## Settings and UI (settings.mjs entries, defaults, Options panel placement, how they reach guest/renderer)

- `msaa: { initial: 0, options: [0, 4] }`, row label "Multisampling (MSAA)", values Off / 4x, flag
  "applies on next start".
- `smaa: { initial: false }`, row label "SMAA", On / Off, live.
- Placement: new `graphics` tab between Display and Audio in src/browser/menu-shell.mjs:10-28. C09 and
  C12 add rows to the same tab; whichever chunk lands first creates it.
- Path: settings.mjs value -> C01 host-settings channel -> `app::Settings::msaa/smaa` in the game Wasm
  -> `MenuBackend` (latch at `Init` for msaa, per Present for smaa) -> packet fields to the GPU worker.

## Acceptance criteria

- With both off: no packet or pipeline differs from today (byte-identical pipeline keys apart from the
  new zero field), no added GPU passes, frame time unchanged within noise on out/perf-60/bench.mjs.
- MSAA 4x: geometry edges in the opening scene are visibly smoothed; HUD, fonts, vision overlays,
  feedback effects, CopyRects consumers and released-surface readbacks render as with MSAA off; no
  WebGPU validation errors in a 60 second soak.
- SMAA: edges of geometry and HUD text are smoothed; flat areas are bit-identical to SMAA off;
  toggling during a mission never stalls longer than one frame.
- MSAA plus SMAA together work.
- Measured added GPU time on M1 Max at 640x480 is recorded for each option and for both; the mission
  mean stays within the 16.7 ms budget or the cost is documented as exceeding it.

## Testing (unit/contract tests first where practical, agent-browser visual checks with muted audio, perf harness)

Write these first and confirm they fail:
- tests/browser/settings.test.mjs: `msaa` normalisation (2 and 8 become 4, garbage becomes 0), `smaa`
  boolean, persistence.
- Transport contract test (extend tests/browser/webgpu-service-protocol.test.mjs): new packet sizes,
  rejection of sample counts other than 1 and 4, `smaa` flag values above 1 rejected.
- tests/browser/menu-backend.test.mjs with a fake `Execute`: screen sized targets get
  `samples = 4` and `sample_count = 4` pipelines, a 512x512 target stays at 1, pipeline keys with
  MSAA off are unchanged.
- tests/browser/webgpu-service.test.mjs (real GPU page): draw into a 4x target, then sample, copy,
  read back and present; each sees resolved data. Copy into a 4x destination then draw over half of
  it; the untouched half keeps the copied pixels (seeding). Depth readback of a 4x depth equals
  sample 0.
- SMAA: lookup texture sizes and a checksum of both arrays; on a synthetic image (vertical black/white
  step, a diagonal line, a flat colour) the edges target is non-zero only next to edges, the flat image
  is unchanged, and blended values along the step lie strictly between the two colours.
Then:
- agent-browser visual check with out/front-end/mute.js loaded: captures of the opening at off, MSAA,
  SMAA, both; compare zoomed crops of a roof edge and HUD text.
- Perf: four runs of `node out/perf-60/bench.mjs <web> <port> <out.json> <label>` (set the settings
  through `menuShell.settings.set` before boot, as bench.mjs:16 already does for developmentStart),
  plus `PERF_SOAK=60`. Use a separate build directory, never out/boot-m5.

## Risks and open questions

1. Pipeline count: screen and off-screen variants of the same state may both exist, against the 256
   pipeline caps (menu-backend.h:16, webgpu-service.cpp:883). Measure `FrameMetrics.pipelines`.
2. Does the game ever sample or read back a screen sized depth surface? Native skips multisampled
   depth reads (d3d12.cpp:1905-1906). Choose between a sample-0 depth resolve (task 5) and failing
   loudly; collect evidence with a counter first.
3. Resolve frequency: lazy resolves happen at each read; a frame with many feedback reads of the scene
   could resolve several times. Count resolves per frame in the diagnostics.
4. Memory: each 640x480 4x companion is about 4.9 MB; with C07 at 2x scale about 19.7 MB each. Several
   screen sized copies may press the 256 MiB service budget.
5. C07 interaction: the "screen" rule must use guest size, not host size, and SMAA must run at the
   host size of the presented source before any scaling to the canvas. Agree on a shared
   `host_extent` field so both chunks size companions and SMAA targets the same way.
6. MSAA mid-session: only session start in this chunk. Applying at the next level load needs a
   pipeline prewarm design; left open.
7. SMAA port correctness: there is no native reference image on this machine. Optionally produce one
   offline by compiling the native HLSL with DXC to SPIR-V and translating with Tint or naga, then
   compare against the hand port on the synthetic images.
8. Alpha: SMAA blending writes alpha from the source; presentation ignores alpha, so this is safe.

## Out of scope

- Internal resolution scaling and supersampling (C07).
- 2x and 8x MSAA, alpha-to-coverage, per-sample shading.
- HDR rendering, tone mapping, ambient occlusion and the soft shadow filter itself (C09 uses the
  multisampled depth helper from task 9).
- Reworking the browser front end beyond adding the two rows.
