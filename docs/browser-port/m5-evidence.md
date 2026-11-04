# M5 implementation evidence

Status: accepted for P5.1 through P5.4 on the recorded first-area route. Parent: [M5](05-first-3d.md).

## Reproducible artifact and route

The frozen production identity is `396cdbe256ec6f4378056b4fdb272bc119d820b2497554493c3147016d41833f`. Its story Wasm is 60,878,163 bytes. The production build and subsequent no-op build pass. Matching XBE SHA-256 is `c742d61c9b8d8c1c4f886a4a4a69ffb65ab3961e0283dcad33bfcfa963885143`; trusted reproduction verification passes for all 134 retained generated outputs. Original generated files are unchanged.

Select the local ISO, press Boot, wait for the original title prompt, then follow Start, Solo, temporary profile, New Game, Normal and confirmation. The first campaign map is `x01_Iceland_a`, loaded from its 13,681,301-byte LIN archive. The original opening sequence finishes and returns control to Sam in the ice cave with the original HUD. No developer travel command, scene shortcut or injected objective state is used.

Tested host: Apple M1 Max, 10 CPU cores, 64 GiB memory, macOS 27.2, Chromium 151 with WebGPU Apple `metal-3`, cross-origin isolation and shared Wasm memory. Emscripten is 6.0.13-git; the pinned Binaryen 133 optimizer SHA-256 is `02590be425ed6e0fb7d7a4a31cf9e62c66a46a3b54e4822889478c295d19d438`. The module explicitly requires standard SIMD and actual tail calls. Capability checks reject unsupported browsers before instantiation. Test automation uses the agent-browser headless WebGPU session. Settings retain the original 640 by 480 presentation, sensitivity 1 and normal first-campaign defaults.

## Runtime and streaming contracts

The original `XLaunchNewImageA` constructs the real launch page and calls `HalReturnToFirmware(2)`. The browser captures an owned 4 KiB launch page and starts a fresh C++ `Runtime` and `PreparedImage` inside the same Wasm host. All old guest workers join; handles, waiters, timers, audio, translated backing and renderer resources retire before replacement. GPU retirement must be acknowledged before a new guest starts. Unsupported executables, multiplayer and firmware modes fail explicitly.

The selected File, ISO bridge, bounded idle pthread pool, GPU service, temporary profile and raw cache belong to the outer session. Device identity, copied launch bytes and persisted display pixels survive replacement. Old guest pointers and handles do not transfer. Input producers retire before their shared transport is rebound. Stop releases the complete outer session and its temporary state. Full return-to-menu and New Game use this same restart contract.

The original pause menu Restart Mission restores a serialized world in process. It resets the real level clock, reloads the opening scene and preserves the original native contract. It does not call firmware reboot. World epochs record a changed level or clock reset; runtime restart counters separately record firmware transitions. Writable read/write OpenIf access to the original autosave temporary file is supported with sharing checks, rather than reporting false success or inventing a reboot.

Mission reads use one reusable 4 MiB staging buffer, assembled through ISO mailbox slices of at most 1 MiB. Defaults outside mission mode remain 1 MiB. A canceled or partially failed logical read publishes neither partial guest bytes nor a successful completion or stale event. Named failures include logical path, offset and slice size. The required mission is validated before boot; optional package probes retain native missing-file behavior. Loading progress counts actual ISO reads. The original Bink work-query guard preserves held/completed read scheduling and delegates active work. There is no full disc preload, asset upload or game asset download.

Hard limits remain explicit:

| Storage or resource | Limit |
| --- | ---: |
| Main / pthread stack | 1 MiB / 256 KiB |
| Wasm heap / translated backing | 384 MiB / 192 MiB |
| Guest virtual allocation arena | 64 MiB |
| Temporary profile / raw cache | 32 MiB / 64 MiB |
| Mission logical read / ISO slice | 4 MiB / 1 MiB |
| GPU command staging | 4 MiB, 256 draws |
| Renderer residency / resources / pipelines | 128 MiB / 256 / 256 |
| GPU texture storage / service resources | 256 MiB / 4,096 |
| Audio voices / arena | 64 / 4 MiB |
| Simulation steps per presentation / backlog | 8 / 2 seconds |

Checked page translation allocates 8 MiB of metadata lazily and indexes committed owned backing. It does not allocate a 4 GiB guest image. Physical aliases share write versions, cross-page access retains full validation, and foreign memory retains locking. Bounded straight instruction blocks release leases before calls, loops, atomics and timer reads; original exception-handling functions remain unchanged. Actual guest tail transfers keep the existing stack limits. The mutex lifetime correction destroys unreachable closed objects while preserving live aliases and pending waits. Indefinite waits sleep; critical-section release wakes real handoffs.

## Completed lifecycle checks

Two confirmed native Restart Mission operations increment world epochs from 1 to 2 to 3 while runtime restarts remain 1. Both retain 24 active guest workers, 23 child-thread handles, 227 renderer resources, 214 pipelines, 69,905,596 renderer-resident bytes and 297,795,584 Wasm heap bytes. The captures are `final-native-cycle-1` and `final-native-cycle-2`.

Two full returns to the real Solo main menu and the intervening New Game advance firmware runtime counts through 1, 2, 3 and 4. The latest menu has 13 renderer resources and four pipelines; the Wasm heap remains 297,795,584 bytes. The fixed browser pool peaks at 132 workers, including idle Emscripten workers. Stages differ in resource counts as loading and menus initialize; these are bounded lifecycle observations rather than identical frame snapshots.

Cancellation is armed for the next original firmware request. The `restarting` notification triggers Stop before a fifth guest runtime can execute. `final-cancelled-transition.json` records a stopped shell, zero tracked workers, zero GPU resources in the retirement acknowledgement and an enabled Boot control. A fresh Boot without reselecting the ISO restores the original campaign, controllable player and trusted Pointer Lock; see the private `recovery-player` and `recovery-controlled` records. Native and JavaScript cancellation tests additionally verify stale result suppression, owned memory boundaries and failed-replacement behavior.

## Graphics coverage and limits

The selected area renders original static and skinned geometry, textures, lighting, player, subtitles and HUD. Movement and camera changes retain geometry, depth orientation and channel ordering without intermittent upload corruption. Required shader variants compile and actual game WebGPU validation remains clean.

Encountered paths include indexed vertex conversion, transforms and skinning; reciprocal/RSQ signed-zero behavior; texture-stage signed dot and reflection; register combiners; linear and swizzled A8/X8, DXT1/3/5 and six-face cube mip chains; independent min/mag/mip filters, LOD and border addressing; depth/stencil import, masks, alpha discard, depth-only draws, shadow comparison and bias; clipped clears and partial copies; sampled render-target feedback and completed-version readback. Unsupported fixed state fails with a named diagnostic. The original renderer's occlusion effect, shadow filter and shadow application hooks remain explicit M6 boundaries if encountered.

Immutable resource IDs and versions preserve GPU contents across delayed completion. Batches reuse three aligned arenas and consecutive compatible passes. Owned view and sampler caches have fixed limits. Deferred validation preserves command IDs and propagates terminal failures; retirement suppresses stale callbacks. Both direct pthread mailbox and main-thread fallback pass the public GPU fixture with 742 commands, four deliberate errors and zero resources after shutdown. All 123 shader fixtures compile and pass independent numeric GPU checks.

Private image evidence uses the bounded completed-presentation GPU readback because automation screenshots time out on this host. It serializes with guest commands and uses the existing Readback operation. The PNG retains RGB and uses opaque alpha to match the WebGPU canvas configuration. A public 4 by 4 exact-pixel fixture verifies ordering, ownership, source retirement and zero-resource cleanup. No proprietary captures are published.

## Input and timing

WASD reaches original game actions. Relative mouse motion reaches the original engine MouseX/MouseY axis handler at the viewport update boundary. Capture requires a trusted gesture; denial has a visible retry. Sensitivity, Escape unlock, key release and focus loss use the owned shared input transport. Enter pauses the original game. Backgrounding clears movement, releases capture and excludes inactive duration from the guest timer.

The final focus test holds forward before backgrounding. All eight hidden samples retain the exact same original world time, `16.7593307495` seconds; the input mask is zero and the inactive gate is set in every sample. Return records 8,351 inactive milliseconds, clears the gate and retains zero simulation backlog. Earlier native-pause samples likewise hold world time constant.

The original engine clamps a single tick to approximately 66.666 ms. Presentation on this host exceeds that interval. Mission scheduling now divides actual active elapsed time into bounded native engine ticks and suppresses only intermediate primary viewport draws. Every update still runs the original engine; secondary viewports retain their draws. It never changes TimeDilation, the native maximum delta, world time or objective state. It carries bounded time debt and fails explicitly if unable to keep up. Pause, level change, clock reset, cancellation and exceptions restore the scheduling boundary.

Read-only telemetry separates native update work, native drawing, completed presentation intervals, GPU command/readback/upload/draw waits and ISO waits. Wait categories overlap native drawing and must not be added to it. Update and draw work exclude inactive host time. Counters describe this device and route; they do not establish universal performance.

## Accepted ten minute traversal

The final recovered session completes 59 circuits in 601.569 real seconds. Each circuit alternates relative mouse counts of +24 and -24, then holds W, D, S and A for two seconds each, with 500 ms released between legs. Before starting, the player moves forward for two seconds with +40 relative mouse counts from the normal opening camera. This stays in the opening ice field near the submarine and crates. All 236 recorded legs change the original Pawn Location; total travel is 114,943 game units. Sampling reads the original reflection properties and never writes player position or world time. Captures at roughly one minute intervals show the scene and HUD throughout. Close camera placement against ice can obscure Sam in intermediate captures; precise camera collision parity remains unverified.

| Measurement | Final result |
| --- | ---: |
| Real / original world time | 601.569 / 601.644 seconds |
| World-to-real time ratio | 1.000125 |
| Completed frames | 3,712 |
| Mean / median frame time | 162.04 / 161 ms |
| p95 / p99 / maximum frame time | 187 / 197 / 265.29 ms |
| Wasm heap / translated backing | 297,795,584 / 156,792,190 bytes |
| Renderer residency / resources / pipelines | 68,072,812 bytes / 219 / 188 |
| GPU service heap / peak resources | 24,379,392 bytes / 656 |
| Active guest workers / child-thread handles | 24 / 23 |
| Browser workers including idle pool | 132 |
| Maximum simulation backlog | 0 microseconds |
| Validation errors | 0 |

Heap, translated backing, renderer residency, renderer resources, pipelines, worker and handle peaks stay constant throughout the circuit samples. GPU service heap and resource peaks are already at their final values in the first circuit and remain unchanged. Audio voices warm from 43 to the fixed limit of 64; 4,037 recoverable creation rejections are recorded. They do not terminate the guest or produce GPU validation errors. Audible output and audio voice lifetime parity remain explicit P6.2 blockers.

The interval executes 11,151 native simulation ticks and skips 7,439 intermediate primary viewport draws. Measured native update work is 312.907 seconds and drawing is 288.540 seconds. GPU command wall waits are 57.970 seconds, including draw waits of 42.970 seconds and readback waits of 3.797 seconds; upload waits are zero during this interval. These waits overlap drawing. Streaming reads another 4,329,884 bytes through 702 operations, with 0.680 seconds of asset waits. Actual world-time endpoints and bounded presentation histograms are recorded independently. The approximately 6.17 FPS display cadence is a device-specific limitation, even though simulation progresses at the intended speed.

The separate 15.150-second gameplay sample delivers 99 frames, mean 154.60 ms and p95 161 ms. Its sampled world clock advances 14.998 seconds, within a presentation interval of the wall window; the longer traversal establishes the sustained timing result. No CPU profiler or build runs concurrently with the timed traversal.

`traversal-final.json`, `traversal-summary.json`, `traversal-samples.jsonl`, `traversal.log` and the `traversal-*.png` captures retain the private evidence. A further post-traversal movement/release check reports changed player position, input mask zero, Pointer Lock retained and no errors. Final Stop then reports a stopped shell, zero tracked browser workers, GPU retirement with zero resources and an enabled Boot control in `final-clean-stop.json`.

## Validation artifacts

All captures, raw traces, private ISO paths and test logs remain in ignored `out/m5-prerequisites/`. Public tests contain synthetic inputs only. Complex memory, ABI, timing, streaming, synchronization, translation and lifecycle changes have red/green checks.

The final timing/input suite passes 15 tests, and the final integration suite passes 90 tests. The earlier focused suites cover CPU configuration, memory blocks and pages, file providers, audio bounds, shader translation, render snapshots and GPU transport. Counts from overlapping runs are not summed into a full-suite claim. `git diff --check` passes. No Windows execution is claimed.

## Reproduction

```sh
mkdir -p out/test-tmp
TMPDIR="$PWD/out/test-tmp" \
PATH="/opt/homebrew/opt/emscripten/bin:/opt/homebrew/bin:$PATH" \
  emcmake cmake -S . -B out/boot-m5 -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBROWSER_BUILD=ON -DBROWSER_BOOT=ON -DBROWSER_STORY=OFF \
  -DBROWSER_MENU=ON -DBROWSER_MISSION=ON -DBROWSER_ACTUAL_TAIL_CALLS=ON \
  -DBROWSER_WASM_SIMD=ON -DBROWSER_MENU_TRACE_ABORTS=ON \
  -DBROWSER_GENERATION_RECORD="$PWD/out/provenance-audit/generation-record.json" \
  -DBROWSER_WASM_OPT=/opt/homebrew/opt/emscripten/libexec/binaryen/bin/wasm-opt
TMPDIR="$PWD/out/test-tmp" \
PATH="/opt/homebrew/opt/emscripten/bin:/opt/homebrew/bin:$PATH" \
  BINARYEN_CORES=4 cmake --build out/boot-m5 -j2
node tools/browser/serve.mjs out/boot-m5/boot-web 8125
```

Follow the original menu route above. In automation, hold Enter for 350 ms when pausing gameplay, since a 100 ms press can fall between frames. In menus use 100 ms presses and allow the original response before the next action. Inspect confirmation dialogs; Restart Mission defaults to Cancel and requires selecting OK.

## M6 work remaining

The first area is controllable, with a low display rate on the tested host. Broader gameplay remains unaccepted. M6 must improve and remeasure performance, replace the timed silent audio sink with audible output, validate stealth visibility and required shadow parity, and exercise vision modes, gadgets, combat, scripted interactions, death/checkpoints, quick save/load and mission completion. Voice exhaustion returns a recoverable HRESULT within the unchanged 64-voice budget and remains observable; audible mixing and lifetime parity need P6.2 validation. P6.1 owns stealth shadows and vision modes, P6.3 owns interactions and required media, P6.4 owns checkpoint and save/load recovery, and P6.5 owns full completion and broader performance. Durable saves across page lifetimes belong to M7. These limitations do not establish a completed or fully playable mission.
