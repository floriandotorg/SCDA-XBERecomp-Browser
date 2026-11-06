# C06: Widescreen output and field of view

Class: Complex
Depends on: C01-hook-foundation (memory helpers, symbol macros, app::Settings shim, host-settings channel, compiling
selected src/game/*.cpp). Shares the canvas sizing contract with C07-render-scale.
Native sources: src/app/settings.cpp, src/app/app.h, src/kernel/xboxkrnl.cpp, src/game/fov.cpp, src/game/hud.cpp,
src/game/movie_aspect.cpp, src/game/loading.cpp, src/game/unreal.cpp/.h, src/game/magma.h, src/xdk/d3d8.cpp,
src/gpu/backend.h, src/gpu/d3d12.cpp

## Goal

Show the story game at the browser window's shape (or a chosen aspect) the way the PC port does: the 3D view widens
instead of stretching, Magma 2D (HUD, menus, OPSAT, scopes, loading pictures, movies) is placed per page as hud.cpp
places it, 4:3 pictures are pillarboxed, and a Field of view setting widens gameplay views. On a 4:3 window nothing
changes.

## Native behaviour

Aspect source
- `app::Output()` is the configured mode or the desktop (src/app/settings.cpp:262-266); `Aspect()` is `max(w / h,
  4/3)` with no upper clamp (settings.cpp:285-288). `Narrow() = 4/3 / Aspect()` is the share of the width a 4:3
  picture takes, `Widescreen()` is `Aspect() > 1.34` (src/app/app.h:106-110).
- `FovScale()` is `tan(fov / 2) / tan(75 / 2)` (settings.cpp:290-293); `fov` is clamped 45 to 140, default 75
  (settings.cpp:83, app.h:38); `fmv_scale` 0 fill, 1 a 4:3 frame (settings.cpp:87, app.h:42). Options rows: FIELD OF
  VIEW and FMV SCALE (src/app/menu.cpp:328, 335, 499-506).

Kernel
- `VideoFlags()` returns `0x00010000` (widescreen) when `Widescreen()`, always `| 0x00080000` (480p)
  (src/kernel/xboxkrnl.cpp:926-927). It answers `ExQueryNonVolatileSetting` index 8 and the EEPROM image (index
  0xFFFF, offset 0xEC) (xboxkrnl.cpp:937-960) and is ORed into `AvSendTVEncoderOption` option 6 as `0x00400104 |
  VideoFlags()` (xboxkrnl.cpp:976-982). The comment there: D3D lists widescreen modes only when these say widescreen,
  and the game fits movies anamorphically only in a widescreen mode.

Frame shape (anamorphic, not a wider back buffer)
- The guest back buffer stays the size the title asks, 640x480 (src/xdk/d3d8.cpp:197-202). The renderer scales every
  target uniformly (src/gpu/d3d12.cpp:1441-1446), and `BlitFrame` stretches the 4:3 shaped frame into `vw = min(w, h *
  aspect)`; with `pillarbox` the viewport is `vh * 4/3` wide (d3d12.cpp:3935-3948). The game therefore draws a
  horizontally narrowed picture that the final blit widens.
- Pillarbox decision: `gpu::g_splash`, `g_in_splash`, `g_shown_splash`, `g_splash_next` (src/gpu/backend.h:76-82).
  Every draw outside `g_in_splash` clears `g_splash` (d3d8.cpp:806, 815, 855); Swap latches it into `g_shown_splash`
  and passes it to `Present` (d3d8.cpp:417-422). hud.cpp's ShowScreen hook sets it for 4:3 pictures.

3D view (src/game/fov.cpp)
- `sub_002898CB` FCameraSceneNode constructor: for calls from the frame view (`kFrameView = 0x1CDCED`), widens the
  degree angle by `FovScale()` only in gameplay states (not the `kOther` states such as scripted events, sniping,
  cameras), without letterbox bars (viewport +0x154) and without `bCinActor`; Canvas DrawActor and DrawPortal callers
  are skipped (fov.cpp:20, 100-157). It also keeps the node's WorldToScreen for `game::ProjectModel` (used by
  keypad.cpp).
- `sub_002878A3` FDynamicActor::Render: held-up models (lock pick, keypad, oscilloscope, detonator meshes) are drawn
  grown by `d3d::ViewScale(g_scale)` so they keep their size while the view widens (fov.cpp:159-185). `ViewScale`
  scales `vsc[58].xy` (d3d8.cpp:98-106, 164-168).
- `sub_00285530`: the view's x scale, 1 on 4:3 and 3/4 on widescreen times the 10:11 pixel widening; the hook undoes
  the 3/4 and multiplies by `Narrow()` (fov.cpp:187-195). This is what makes 3D widescreen at all, independent of the
  FOV setting.
- `sub_00286BB6` and `sub_00289051` clear the cached-x-scale bits at 0x673108 and 0x673110 so an aspect change takes
  effect (fov.cpp:197-206).

2D (src/game/hud.cpp, summary at hud.cpp:1-2)
- `sub_0028EDCC` FCanvasUtil::Flush narrows the screen to clip matrix (+0x74 scale, +0xA4 move) by `Narrow()` and a
  per page offset (hud.cpp:398-414).
- `sub_0028F090` DrawQuad applies the Fill, Extend, Bars, Shade, Spread modes to Magma images (hud.cpp:265-395); page
  and element tables `kHud`, `kOverlay`, `kSniper`, `kMenu`, `kPages` (hud.cpp:57-122).
- `sub_002CC260` narrows rect shapes drawn past the flush (hud.cpp:417-433).
- `sub_002E3EC4` and `sub_002F117E` narrow the scope blur's opening mask through `d3d::State().vsc[166..167]`, alpha
  func/ref and three TSS words at `SYM(D3D_DeferredTextureState)` (hud.cpp:440-491).
- `sub_002E3011` screen quads: keeps `ri + 0x4D04` (the boot-time widescreen byte) equal to `Widescreen()`, refits
  full screen movies (`Refit`, honours `fmv_scale`), caps the menu background movie at 16:9 (`StretchNoWider`), keeps
  the ESRB notice 4:3, narrows other quads (hud.cpp:567-602).
- `sub_002BE990` page draw, `sub_002C64E0` element draw, `sub_002303E8` Magma frame (decides in-level full width
  versus centred menus), `sub_002E5F1F` ShowScreen fade drawn in two halves, `sub_002E5F95` ShowScreen (sets
  `g_splash` for 4:3 pictures) (hud.cpp:605-752).
- hud.cpp also calls `game::EventPrompts`, `ComboElement`, `HintElement`, `WordingElement` (prompts.cpp),
  `OptionsElement` (options_menu.cpp), `app::DrawWatermark`, `app::DrawFps`, `app::g_hide_hud`, and
  `game::loading::BeforeShowScreen/AfterShowScreen/BothBuffers/g_movie` (hud.cpp:23-29, 627-628, 686, 724-752).

Movies and loading pictures
- movie_aspect.cpp: `sub_002E6D5B` and `sub_002E708A` mark a movie wider than 4:3 (`g_wide_movie`,
  `loading::g_movie`), and `sub_001D05CB` ("the level is the menu") answers 0 for it so 16:9 cinematics are not
  letterboxed in the menu map (movie_aspect.cpp:1-5, 43-72). The same file's `BinkWait` hook is the 60 fps menu fix,
  not aspect.
- loading.cpp `PaintEdges` blacks columns 0 and 639 of the decoded 640x480 loading picture so a fade from a full width
  frame clamps to black (loading.cpp:200-206), called from `sub_00051673` when `g_edges && Widescreen()`
  (loading.cpp:381-390); `g_edges` is set in `BeforeShowScreen` (loading.cpp:400-412).

## Browser today

- Kernel: `KernelVideoFlags = 0x00080000` and `KernelVideoPack = 4` (src/browser/kernel-provider.h:9-10). Ordinal 2
  writes `KernelVideoPack | 0x100 | 0x400000 | KernelVideoFlags`, the same value as native for 4:3
  (kernel-provider.cpp:223-224). Ordinal 24 answers only indices 7 to 9 from `{1, KernelVideoFlags, 0}`, captured by
  value at install (kernel-provider.cpp:229-250, 566); there is no 0xFFFF EEPROM path. platform-profile.cpp:154
  publishes the AV pack (ordinal 356).
- CreateDevice: in StateCapture and Rendering modes the presentation parameters must equal `{640, 480, 18, 1, 0, 1, 0,
  0, 1, 46, 320, 0, 0x80000002}` exactly (src/browser/diagnostic-graphics.cpp:874-877). 320 is 0x140; in the XDK
  headers 0x40 is PROGRESSIVE, 0x100 is 10X11PIXELASPECTRATIO and 0x10 is WIDESCREEN (verify against the XDK).
  Reporting the widescreen flag may change this word and fault here.
- Present always passes `pillarbox = false` (diagnostic-graphics.cpp:1283, 1354); there is no `g_splash` tracking in
  the browser draw paths.
- `MenuBackend::Init` sends Configure with the back buffer size (src/browser/menu-backend.cpp:1030-1036); `Present`
  forwards the flag (menu-backend.cpp:1577-1583).
- GPU worker: Configure sets `canvas.width/height` and the surface (src/browser/webgpu-service.cpp:828-839); Present
  draws the full canvas, or a centred 4:3 viewport for pillarbox, through the gamma ramp
  (webgpu-service.cpp:1017-1047). The presentation shader point samples with `textureLoad`
  (webgpu-service.cpp:493-509).
- Front end: `FRAME = 640x480` (src/browser/menu-shell.mjs:8); `layout()` fits, integer scales or stretches that 4:3
  box in CSS (menu-shell.mjs:79-93); Picture and Upscaling rows (menu-shell.mjs:12-13); settings `scaling` and
  `filter` (src/browser/settings.mjs:2-3); `#game` CSS and `image-rendering: pixelated` for Sharp
  (src/browser/menu.html:32-33, 129). Frame capture refuses `w * h * 4 > 4 MiB` (src/browser/webgpu-service.mjs:103).
- No address hooked by fov.cpp, hud.cpp or movie_aspect.cpp is overridden in src/browser today. The `ViewScale`
  equivalent is `DiagnosticGraphics::UpdateViewportConstants` (diagnostic-graphics.cpp:573-578).

## Tasks

1. Effective shape helper (pure JS, failing unit test first): `frameShape({ aspect, scaling, width, height, ratio })`
   returns the effective aspect (Auto: window width / height, at least 4/3) and the CSS box for fit, integer and
   stretch. Cases: 4:3 window, 16:9, 21:9, portrait window (stays 4:3, letterboxed), fixed 16:9 on a 21:9 window.
2. Publish `aspect` (as a float and as the CSS box), `fov` and `fmvScale` through the C01 host-settings channel. In
   the C01 app::Settings shim implement `Aspect()`, `Narrow()`, `Widescreen()`, `FovScale()` and
   `Get().fov/fmv_scale`; latch the aspect once per Present so one frame's flushes, quads and the presentation agree.
3. Kernel flag: let `InstallKernelProvider` take the boot-time widescreen bit from host settings and use it for
   ordinal 24 index 8 and ordinal 2. In a debug build with the bit on, log the CreateDevice parameters the game
   passes, then widen the contract at diagnostic-graphics.cpp:875 to exactly the observed widescreen variant. If the
   game boots and fits movies correctly with the bit off (hooks alone), record that and keep the bit off.
4. Build fov.cpp through the C01 mechanism with game/unreal.cpp. Provide `d3d::ViewScale(float)` (multiplies `vsc[58]`
   x and y in `DiagnosticGraphics` and reapplies on viewport changes) and `d3d::State()` / `d3d::Lock()` shims backed
   by `DiagnosticGraphics::state_` and `lock_`. Keep `game::ProjectModel` exported for whoever ports keypad.cpp.
5. Build hud.cpp. Until the chunks owning prompts.cpp and options_menu.cpp land, link pass-through versions
   (`ComboElement`, `HintElement`, `WordingElement`, `OptionsElement` return false, `EventPrompts` does nothing) and
   stub `app::DrawWatermark`, `app::DrawFps`, `app::g_hide_hud`. Verify the scope mask hook's `vsc` and alpha writes
   reach the draw: `DiagnosticGraphics` recaptures state from guest memory (`CaptureState`,
   diagnostic-graphics.cpp:518-571), so the write must land after that capture and before the draw, or go through the
   guest copies the capture reads.
6. Build movie_aspect.cpp's three aspect hooks. Leave the `BinkWait` hook to whichever chunk owns menu pacing; split
   the file if C01's build list cannot take half of it.
7. Loading picture edges: if the loading-screen chunk ports loading.cpp, check `PaintEdges` runs; otherwise compile a
   minimal browser file with `sub_00051673` and the `g_edges` bookkeeping, and have hud.cpp's
   `BeforeShowScreen/AfterShowScreen/BothBuffers` calls resolve to it.
8. Pillarbox state machine in diagnostic-graphics.cpp: clear `gpu::g_splash` on each draw when `g_in_splash == 0`,
   latch `g_splash_next` and `g_shown_splash` at Swap, and pass `g_shown_splash` to `Present` at lines 1283 and 1354
   instead of `false`.
9. Canvas shape: Configure carries the canvas pixel size `round(H * aspect) x H`, with `H = 480` until C07 supplies
   its own height. `MenuBackend::Present` compares the latched size with the configured one and sends Configure before
   the Present packet when it changed (one ordered path, no main-thread to GPU-worker side channel). Debounce window
   resizes by about 150 ms in menu-shell.mjs.
10. Presentation pass: the full width case stretches the 4:3 source over the whole canvas (already so), pillarbox
    keeps the centred 4:3 viewport (already so). Add a linear sampler variant chosen by the `filter` setting, because
    point sampling 640 columns onto 853 gives uneven column widths; Sharp keeps `textureLoad`.
11. menu-shell.mjs `layout()`: replace the fixed `FRAME` aspect with the effective aspect from task 1; integer mode
    steps the height in multiples of 480 device pixels and derives the width.
12. Capture: keep `capture()` within its 4 MiB readback (854x480x4 is 1.6 MiB, fine); C07 owns larger canvases.
13. Options rows (see below) and a restart notice for the kernel bit if task 3 keeps it boot-time only.

## Settings and UI

New entries in src/browser/settings.mjs:
- `aspect`: options `auto`, `4:3`, `16:10`, `16:9`, `21:9`; initial `auto` (native follows the output shape). Display
  tab, directly under Picture, label "Aspect ratio" (Auto: match window).
- `fov`: 45 to 140, step 1, initial 75. Display tab, label "Field of view", shown as "75 (default)".
- `fmvScale`: options `fill`, `4:3`; initial `fill`. Display tab, label "Full screen movies".

Interplay with Picture (`scaling`): with `aspect: auto`, Fit and Stretch give the same box (the frame already has the
window's shape, at least 4:3); Pixel perfect picks the largest whole multiple of 480 device rows that fits and centres
it. With a fixed aspect, Fit letterboxes or pillarboxes with black, Stretch distorts to the window as today. Upscaling
(`filter`) picks the presentation sampler (task 10) as well as the CSS `image-rendering`.

Path to the guest and renderer: settings.mjs to menu-shell.mjs (computes the effective aspect and box) to the C01
host-settings channel; the app::Settings shim serves the hooks; `MenuBackend::Present` reads the same latched values
for Configure. The kernel bit is read once at InstallKernelProvider.

## Acceptance criteria

- 4:3 window: pixel identical captures to today at the menu and mission start (hooks inactive, `Widescreen()` false).
- 16:9 and 21:9 windows: 3D not stretched (round objects stay round), HUD elements at the screen sides per `kHud`,
  menus centred 4:3 with pulled-out backdrops, OPSAT with black side bars, sniper scope ring round with the blur
  filling the screen, loading pictures pillarboxed with black edges during fades, ESRB notice 4:3, 16:9 cinematics
  full width, menu background movie no wider than 16:9.
- Resizing the window in Auto mode reshapes the next frames without a reload, a GPU validation error or a watchdog
  trip.
- FOV 90 widens gameplay only; scopes, cameras, cutscenes and keypad, lock pick and detonator models keep their size.
- No regression in the perf harness beyond noise (the hooks are CPU only; the presentation pass cost is unchanged
  apart from the sampler).

## Testing

- Unit first: `frameShape` (task 1) with node test; a C++ test for the shim's `Narrow/Widescreen/FovScale` against the
  native formulas (FovScale(75) is 1, Narrow(16/9) is 0.75); a test that the browser kernel answers index 8 with
  0x00090000 when the bit is set.
- Contract test for the CreateDevice parameter acceptance with the observed widescreen word, and that anything else
  still faults.
- agent-browser (muted with out/front-end/mute.js, a separate build dir, never out/boot-m5): window sizes 1280x960,
  1280x720, 1680x720; screenshots of title, Options, mission loading picture, first mission HUD, OPSAT, sniper scope,
  a cinematic. Compare against the native PC port's captures of the same screens where available.
- Frame captures via the existing capture path at 854x480 to diff consecutive runs.
- Perf: `node out/perf-60/bench.mjs <webdir> <port> out/perf-60/c06-wide.json c06` at 16:9 versus a 4:3 baseline;
  report mean and p95 frame time.

## Risks and open questions

- Does the game read the widescreen bit anywhere the hooks do not cover (beyond D3D mode lists and `ri + 0x4D04`)?
  Task 3 decides whether the kernel bit is needed.
- Which presentation flags word does the game pass with the bit on? Unknown until logged.
- `DiagnosticGraphics` captures device state from guest memory per draw; the native hooks write the HLE state directly
  (`d3d::State()`), so the scope mask and ViewScale writes may be overwritten before the draw.
- hud.cpp pulls in prompts.cpp and options_menu.cpp symbols; stubbing them must not change behaviour those chunks
  later restore.
- Aspect changing mid-frame (resize during a flush) could split a frame; the per-Present latch is meant to prevent
  that.
- Very wide windows (32:9): native has no upper clamp; HUD Shade/Extend strips were tuned up to 21:9 at most. Consider
  clamping Auto to 21:9 if artifacts appear.
- Canvas width `round(480 * aspect)` may be odd; check WebGPU and CSS scaling accept it without a half pixel shift.

## Out of scope

- Versus widescreen (src/versus/widescreen.cpp, projection.cpp); the browser runs the story image only.
- Render scale and output-matched canvas size (C07).
- The menu cursor's aspect handling (src/app/cursor.cpp:133) and the in-game options rows (`app::rows`).
- HDR output, brightness/contrast rows.

## Implementation notes

What C06 built.

### Shape and settings

- `src/browser/frame-shape.mjs`: `frameShape` (Auto is the window's shape, at least 4:3; a fixed ratio letterboxes or
  pillarboxes; Pixel perfect steps whole multiples of 480 device rows) and `canvasSize`. Tests in
  `tests/browser/frame-shape.test.mjs`.
- settings.mjs: `aspect` (auto, 4:3, 16:10, 16:9, 21:9), `fov` (45 to 140, 75) and `fmvScale` (fill, 4:3); Display tab
  rows Aspect ratio, Field of view and Full screen movies; in-game RECOMP SETTINGS > DISPLAY rows ASPECT RATIO, FIELD
  OF VIEW and FMV SCALE.
- Host settings words 7 (`fov`), 8 (`fmv_scale`, from the option's index) and 9 (the effective aspect, a value the page
  computes: `createHostSettingsWriter({ computed })`, rewritten by `refresh()` when the window settles 150 ms after a
  resize).
- `app::Aspect()` returns the shape latched at the last Present (`browser::host_settings::LatchAspect`, called by
  MenuBackend's output source), so one frame's hooks and its presentation agree. `Narrow`, `Widescreen` and
  `FovScale` are the native inline formulas over it. Checked in `tests/browser/host-settings.cpp` (Narrow(16/9) is
  3/4, FovScale(90), a portrait shape stays 4:3).

### Renderer

- `MenuBackend::SetOutputSource` asks once a frame for the canvas size (`round(480 * aspect)` x 480 until C07) and the
  upscaling filter; a changed size is configured before that Present's packet, on the same ordered path.
- The Present packet's flags word carries `PresentPillarbox` and `PresentLinear`; the presentation pass has a
  bilinear entry point (`fs_linear`, four loads, then the gamma ramp) beside the point sampled one. Sharp keeps the
  point sampler.
- Pillarbox state machine: `HookGraphics::SplashDraw` before every draw and `SplashPresent` at Swap, as d3d8.cpp keeps
  `gpu::g_splash`, `g_splash_next` and `g_shown_splash`; Present gets the latched flag.
- `AvGetSavedDataAddress` and the hooks' `kernel::SavedDataAddress()` now return the kept display only until the
  first Present, as native's Swap clears it (before, the browser returned it for the whole session).

### Hooks

- `fov`, `hud` and `movie_aspect` are in the default hook list (movie_aspect's BinkWait menu pacing fix included, as
  native links it). `gpu/backend.h` and `game/loading.h` are on the include allowlist.
- `src/browser/loading-screen.cpp`, built with the hooks when `hud` is selected, is the part of loading.cpp hud.cpp
  needs: `BeforeShowScreen` notes the ESRB notice, `PaintEdges` blacks columns 0 and 639 of a 4:3 loading picture
  (through a sized span, so every page is uploaded again), and `BothBuffers` runs the game's own ShowScreen.
- prompts.cpp's element draws, `app::DrawFps` and `app::DrawWatermark` are weak no-ops in game-stubs.cpp.
- Hook helpers in flat memory builds (`G`, `HostPtr`) translate through the flat page table without a backing lease,
  as generated code does: hooks also run under host paths that hold none (fov.cpp's FCameraSceneNode hook under the
  mission tick faulted with "host span requires a backing lease"). Checked builds still require the lease.

### Kernel flag

Kept off (task 3's alternative). The hooks keep `ri + 0x4D04` equal to `Widescreen()` and refit movies, so the
CreateDevice contract stays unchanged.

### Checked in the browser (out/parity, development start)

- 1280x720: canvas 853 x 480, round objects stay round, HUD on the right edge, subtitles on the left, the pause menu
  centred 4:3 with its backdrop full width.
- 1680x720: canvas 1120 x 480, same placement.
- 1280x960: canvas 640 x 480 as before.
- No WebGPU validation errors in any of the three.

### Not checked

Loading pictures with black edges during fades, the ESRB notice, 16:9 cinematics, the menu background movie, the
OPSAT and sniper scope at 16:9, keypad and lock pick model scale with FOV 90, and live window resizing are left for
the user's playtest. The perf harness was not run.

### Static lifetime

| Static | Restart safe because |
| --- | --- |
| fov.cpp `g_view`, `g_scale`, `g_world_to_screen` | Set by the next frame view |
| hud.cpp page and mask state | Set per draw; the mask restores only values it wrote |
| movie_aspect thread locals | Per draw |
| loading-screen `g_edges` | Set by the next ShowScreen |
| host-settings latched aspect | Latched again at every Present |
