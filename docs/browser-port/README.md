# Single player browser port roadmap

## Starting point

The XBE recompilation has already finished. Matching generated C++ and headers are available in `generated/game/`. Generating those files, building the recompiler and supporting `sc4_adv.xbe` are outside this plan.

During development, these C++ inputs still need the Wasm compilation and runtime work below. For the player, Wasm is already built: the webpage offers Select ISO, validates the selected local image, then reads all game assets from that image. No player side recompilation, extracted folder selection or asset download is part of the required workflow.

M1 is accepted for diagnostic compilation; see [M1 evidence](m1-evidence.md). M2 is accepted for representative CPU execution with complete retained story metadata, compact backing and bounded workers; see [M2 acceptance and limits](m2-acceptance.md). **M3 is accepted for bounded diagnostic boot.** The integrated player shell authenticates the selected ISO, executes the genuine parent and child, reads real assets through guest handles, writes and reads its owned cache, and reaches the normal renderer's first device request with networking unavailable. Trusted generation provenance, matching compiled identity, immutable binding and joined ownership remain mandatory. The explicit `GRAPHICS_CHECKPOINT` stops before device completion or rendered frames; it is not a fatal error display, a menu or a running game. The frozen suite passes 248/248 tests, and browser lifecycle QA retires all 40 created workers. See [M3 acceptance and limits](03-game-boots.md#current-cache-and-startup-continuation) and [evidence](m3-evidence.md#current-accepted-evidence). The local test image location is recorded only in ignored [GAMEFILES.md](../../GAMEFILES.md). **M4 is accepted for the real menu and verified single player launch boundary.** The genuine menu renders through WebGPU; keyboard navigation, temporary profile creation, Solo and New Game confirmation work. The host stops at the original verified launch request, with no mission runtime started. See [M4 acceptance evidence](m4-evidence.md#current-accepted-evidence). **M5 is accepted for the controllable first mission area.** The normal menu route, original world reload, firmware replacement, transition cancellation and ten minute traversal pass with bounded resources and intended simulation speed. Display performance is approximately 6.17 FPS on the tested host. See [M5 evidence and M6 limits](m5-evidence.md). **M6 is active.** User playtesting confirms working audio. Rendering, stable 60 FPS, full mission completion and recovery remain under investigation. Night and thermal vision render, and one original quick-save/load cycle restores player state; broader route acceptance is pending. See [M6 progress and reported defects](06-playable-mission.md#current-implementation-and-reported-defects). **M7 is active.** Profiles, settings and original saves persist in the browser's private file system across closed pages and fresh browser sessions, with export and import; campaign transitions and mission coverage are not started. See [M7 current implementation](07-campaign-and-saves.md#current-implementation). M8 remains not started. Initial entry return alone is not a complete game boot.

The developer uses an Apple Silicon Mac. The initial browser target is a pinned Chromium version on that Mac with WebGPU and shared WebAssembly memory. No Windows VM, Wine or Rosetta is required to build the browser prototype. Windows reference captures are useful if available, but are not a build dependency.

## Scope

Deliver a locally executing browser version of the story campaign using WebAssembly, WebGPU and browser audio. Keep the existing translated game logic and Xbox API replacements where their behavior is portable.

Included:

* Story campaign, including profiles, mission transitions, checkpoints, saves and loads.
* Keyboard and mouse as the required input path, with gamepad support after the first playable scene.
* Original gameplay rendering, including the shadows needed for stealth.
* Audio, required movies, browser lifecycle handling and recoverable errors.
* A required Select ISO workflow using the user's own local disc image, plus local save export.

Excluded:

* Co op, split screen, Versus, LAN, online sessions, voice and Xbox Live.
* Browser compilation of C++ on the player's device.
* Streaming the Windows executable from another machine.
* Every browser and every GPU at the first release. Phones and tablets run the same page with touch controls; see [Phones and tablets](mobile.md).
* HDR output, enhanced shader model effects, ambient occlusion, SMAA and desktop display management unless later justified.
* Distribution of game assets or generated game code without a separate rights review.

Some shared code contains multiplayer branches even in `default.xbe`. Excluding multiplayer means rejecting those paths explicitly, not deleting arbitrary shared functions or pretending network initialization succeeded.

## Milestones

1. [M1: It compiles](01-compiles.md). All story generated C++ compiles to Wasm objects; the retained function map links into a diagnostic browser module.
2. [M2: CPU code runs correctly](02-cpu-runtime.md). Real generated functions execute with tested guest memory, dispatch, atomics and continuations.
3. [M3: The game boots from an ISO](03-game-boots.md). Select ISO validates a local disc, starts guest threads, reads its assets and reaches graphics initialization under a diagnostic host.
4. [M4: The real menu works](04-menu-works.md). WebGPU draws the actual game menu and keyboard input navigates it.
5. [M5: A controllable 3D mission runs](05-first-3d.md). New Game reaches a real scene with movement, camera, geometry, textures and baseline lighting.
6. [M6: A mission is playable](06-playable-mission.md). A representative mission works with stealth, required shadows, audio, movies, gadgets and checkpoint recovery.
7. [M7: Campaign and persistence work](07-campaign-and-saves.md). Supported campaign paths, mission changes and durable saves survive fresh browser sessions.
8. [M8: A supported build is deliverable](08-validation-and-delivery.md). Reproducible local delivery, compatibility evidence, performance budgets and failure recovery are documented.

A linked module is not a running game. A clear color is not a menu. A menu is not a playable mission. A playable mission is not evidence that the campaign works.

## Execution rules

* Complete one package at a time. Each package should produce a reviewable artifact and a focused validation result.
* If a package combines several unresolved design choices, split it before implementation. Package size is a planning boundary, not a promise about elapsed time.
* Write meaningful tests for complex memory, synchronization, scheduling, storage and translation behavior before changing it. Verify the intended failure first, then the passing implementation.
* Keep the Windows target separate and preserve its behavior. Shared changes need regression coverage; do not claim a Windows run when only its build definition was inspected.
* Keep original generated output untouched. Apply browser adaptations through shared abstractions or reproducible build generated adapters, never manual edits to thousands of generated functions.
* No stage depends on a fresh recompiler run. Any future generator change needs an explicit migration decision and retained input provenance.
* Use explicit source lists for browser builds. Do not import all of `src/*/*.cpp` and then chase Windows errors.
* Use trapping placeholders only at documented bring up boundaries. A critical operation must never silently report success.
* Keep test credentials, private paths, copyrighted fixtures and proprietary captures out of public artifacts.
* For browser automation use the project's agent-browser workflow. Node smoke tests supplement real browser tests; they do not replace them.

## Architecture boundaries

### Generated code and linking

The native project separates host and game code using PE imports, exports and linker aliases. The browser target should initially statically link generated code and portable host overrides into one Wasm program.

Preserve both the overridable function name and its original `__imp__` body. Hooks using `GUEST_ORIG` must still reach the original body. `X86Resume` is executable control flow, not optional error reporting.

### Guest memory

Do not allocate a 4 GiB buffer to imitate native virtual memory. Guest addresses remain 32 bit values; translate them to compact host backing regions. The two physical windows at `0x80000000` and `0xF0000000` must alias the same 64 MiB storage.

Ordinary loads, stores, pointer helpers, bulk transfers, atomics and GPU resource access must agree on this translation. Passing `base + guest_address` through an overlooked code path is a correctness bug.

### Threads and browser services

Keep browser UI work separate from blocking guest execution. Use pthread workers for guest threads and a browser service context that can yield for GPU, file and audio callbacks.

An operation may block a guest worker only if an independent context can complete it. Never wait on a worker for a callback that can execute only on that same blocked worker.

### Graphics

Implement `gpu::Backend` for WebGPU. Reuse Xbox state interpretation and texture handling where possible, but audit DXGI formats and HLSL generation rather than treating those helpers as platform neutral.

Do not assume synchronous GPU readback, desktop pipeline binaries or every Xbox blend and depth mode maps directly to WebGPU.

### Assets and persistence

The required asset source is a user selected local ISO. Follow the [ISO loading contract](iso-loading.md). The page must not fetch game assets from a server, upload ISO bytes, require extraction or copy the full ISO into browser storage.

Keep immutable game assets outside the Wasm heap and writable game state in a separate persistent namespace. Index the disc filesystem and read bounded byte ranges on demand rather than loading the entire image into MEMFS.

Keep the game's synchronous file contract behind a worker compatible bridge. Retain the selected File in the session controller so mission runtime restarts do not open the picker again. After a page reload, selecting the ISO again is an acceptable baseline.

### Reboots

Native mission changes start a fresh process, as documented in `src/gpu/pipeline_cache.h` and implemented through `src/kernel/boot.cpp`. Replacing this is required before the first mission, not an optional release feature.

A browser session controller owns assets, launch state and persistence, and replaces the game runtime when a guest reboot requests it. All pthreads belonging to the old runtime must stop or be terminated with their owning runtime.

## Assumptions and open decisions

These are proposed directions, not silently settled implementation choices:

* Pin a tested Emscripten version during M1; record exact browser and toolchain versions in evidence.
* Start with compact region translation and measure its cost in M2. Change the layout if real access patterns require it.
* Prefer direct WGSL emission using the existing shader semantics. M4 must evaluate a small working sample before choosing direct emission versus a translation toolchain.
* M3 must prove the local ISO reader and its worker bridge. WORKERFS, WasmFS and OPFS are possible implementation components, not alternative player workflows; no full disc import is required.
* M6 must choose an audio transport after measuring consumption and allocation behavior.
* A published distribution model is a separate M8 gate. A successful private build does not authorize hosting generated game code.

Record each decision with alternatives, a reason, a small proof and reversal cost in the relevant milestone evidence.

## Input requirements by stage

* M1 and initial M2: matching generated C++, mapping tables, shared headers and configuration metadata.
* Real game function tests in M2: matching XBE section bytes when the selected function reads image data.
* M3 onward: a user selected local ISO containing the exact XBE used for generation and the required story and shared media. Read `default.xbe` and image section data from the ISO. Do not require Versus initialization merely because the disc also contains `adv/`.
* M6 onward: a legally acquired reference installation or manually verified gameplay expectations where visual and behavioral comparisons are needed.

Record the XBE hash and generated source manifest before runtime work. Reject a mismatched XBE rather than accepting it as a harmless dump difference during bring up.

## Evidence and status

Each package completion record should contain:

* Package ID and status: not started, active, blocked or accepted.
* Toolchain, browser, machine and input manifest identifiers.
* Commands or a repeatable local procedure.
* Expected result and observed result.
* Relevant tests, diagnostic counters and bounded logs.
* Known failures and the next blocking dependency.

Use a local ignored evidence directory for proprietary captures. Commit only sanitized reports and synthetic fixtures. No debug dump should serialize full game memory by default.

Accept a milestone only when its exit checklist passes in a real browser. Keep blockers visible; do not relabel an incomplete stage as complete because a later experiment works.

## Reference implementation map

* Build and host linkage: `CMakeLists.txt`, `src/app/game_code.cpp`, `src/cpu/x86_ops.h`, `tools/recomp/codegen_emit.cpp`.
* Guest CPU and dispatch: `src/cpu/x86_context.h`, `src/cpu/dispatch.cpp`, `tools/recomp/codegen_int.cpp`, `tools/recomp/codegen_fp.cpp`.
* Kernel and launch: `src/kernel/memory.cpp`, `src/kernel/kernel.h`, `src/kernel/threads.cpp`, `src/kernel/objects.cpp`, `src/kernel/boot.cpp`.
* Files: `src/kernel/files.cpp`, `src/kernel/disc.cpp`, `src/app/game_files.cpp`. The existing disc parser probes XISO partition offset zero and full original Xbox disc offset `0x18300000`; the browser must verify and bounds check these layouts rather than assuming arbitrary `.iso` files work.
* Graphics: `src/xdk/d3d8.cpp`, `src/gpu/backend.h`, `src/gpu/state.h`, `src/gpu/d3d12.cpp`, `src/gpu/render_thread.cpp`, `src/gpu/shaders.cpp`, `src/gpu/vertex.cpp`, `src/gpu/texture.cpp`.
* Audio: `src/xdk/dsound.cpp`, `src/audio/output.h`, `src/audio/xaudio2.cpp`, `src/xdk/bink.cpp`, `src/game/bink_io.cpp`.
* Input and campaign: `src/core/input.cpp`, `src/xdk/xapi.cpp`, `src/game/quick_save.cpp`, `src/game/loading.cpp`.

The existing `RECOMP_ONLY` build is useful before this roadmap but is not the browser target.

## Platform documentation

Recheck these against the pinned toolchain before implementing APIs:

* [Emscripten downloads](https://emscripten.org/docs/getting_started/downloads.html)
* [Emscripten pthreads](https://emscripten.org/docs/porting/pthreads.html)
* [Emscripten exceptions](https://emscripten.org/docs/porting/exceptions.html)
* [Emscripten filesystem](https://emscripten.org/docs/api_reference/Filesystem-API.html)
* [Emscripten WebGPU support](https://emscripten.org/docs/porting/multimedia_and_graphics/WebGPU-support.html)
