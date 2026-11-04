# M6: A mission is playable

Status: active. User playtesting confirms audio works correctly after the streaming fixes. Rendering, performance, full mission completion and recovery remain under investigation. M5 is accepted; P6.1 through P6.5 remain unaccepted.

The requested performance target is stable 60 FPS. The latest clean ten-second control-view sample presents 79 frames, with 127.1 ms mean and 143 ms p95 intervals. This does not meet the 16.7 ms frame budget.

Validation uses normal browser gameplay and user playtesting. An independent reference installation is unavailable. Record observed behavior and reproducible defects, and keep visual parity unverified wherever there is no independent comparison. This does not waive the completion, recovery, audio or rendering acceptance checks below.

## Selected route and acceptance script

The selected mission is the opening Iceland story mission, beginning at `x01_Iceland_a`. Enter through Start, Solo, the temporary profile flow, New Game, Normal and confirmation. Select Continue when loading completes. Let required sequences finish without using optional intro skipping as a workaround.

The route is provisional until a full normal completion establishes its objective order. Record these checks as the route progresses:

1. Gain player control after the original opening sequence. Verify movement, camera, crouching and interaction prompts using the game's controls.
2. Progress beyond the opening ice field through required doors, traversal, gadgets and scripted objectives. Record the actual controls and objective order rather than guessing them from another platform's version.
3. Exercise required light and shadow conditions and vision modes. Record surfaces, shader states and stealth feedback encountered at each location.
4. Check speech, music, positional effects and required media, including sequence skip and return to control.
5. Reach a genuine checkpoint, exercise death and reload, then pause menu restart and permitted quick save/load. Keep the ISO selected throughout recovery.
6. Complete the mission normally, retaining the completion screen or transition evidence. Repeat from a clean session without developer state.

Known omissions entering M6 are owned by their implementation packages: original shadow and vision parity by P6.1, silent output and voice exhaustion by P6.2, unverified interactions and required media by P6.3, death/checkpoint and quick save/load coverage by P6.4, and full completion and the approximately 6.17 FPS M5 display rate by P6.5. Optional graphics settings must not hide required rendering defects.

## Current implementation and reported defects

Audio now works correctly in the user's physical-browser playtest. The implementation decodes PCM and Xbox ADPCM, retains independent guest packet timing and mixes through a bounded six-channel AudioWorklet transport. Stereo retains the native 0.707 centre/back coefficients and excludes LFE. Explicit activation, suspended or stopped device clocks, underruns, runtime replacement and retirement have regression coverage. The earlier automation-only device-clock failure is not an external prerequisite. Route-specific pause, recovery and movie synchronization checks remain pending.

Streaming fixes support the original asynchronous unbuffered file modes, explicit overlapped offsets, completion events, opaque completion contexts and the event handle's low-bit flag. Physical contiguous allocations accept the observed noncached and write-combined hints on the same coherent backing. Story sessions select a 512 MiB temporary cache, 256 shared file/link handles and a 1 GiB Wasm heap ceiling. Default non-story limits remain unchanged. Native and Wasm tests cover the original 175,345,664-byte bank size, quota rollback, handle reuse, partial failures and retirement. Production gameplay opens the speech banks, supplies continuous compressed streams and writes its initial autosave through the original cache fallback.

The optional development checkbox starts Iceland from a cold runtime using the original launch-page protocol and skips movies. It defaults off and is disabled during a session. The original utility drive receives its first format request before mission loading; later firmware reboots retain session storage. Browser validation reaches player control and the cave without the title/profile flow. The controller Continue prompt and the original scripted opening still occur. This shortcut is for investigation and does not count as normal-entry or required-media acceptance.

Native and Wasm regressions cover night-vision blend factors and constants, thermal dependent texture modes 15 and 16, bounded upload identity history and pipeline descriptor deduplication. Both vision modes have rendered in the original mission. All 125 synthetic shaders compile. Both GPU transport topologies pass 758 commands, retain only four intentional negative diagnostics and retire to zero resources. This includes exact D24/stencil and D16 readback, partial and delayed replies, CPU row-padding preservation and depth upload boundary values. The reported dark models and sky remain rendering defects under investigation; no general visual-parity acceptance is claimed.

The original cave tutorial and menu background render after supporting observed read-only movie opens and atomic reads of at most 32 MiB through unchanged 1 MiB ISO slices. Native and Wasm tests cover a large successful request, failed slices, cancellation and untouched destination bytes on failure. Tutorial playback produces PCM and advances through its original prompts. Full ending, skip and synchronization checks remain pending. All game bytes still come from the selected local ISO, without extraction or upload.

A later normal-entry cave run reproduces a separate `menu backend feedback resource count budget exceeded` fault. Temporary render-target feedback copies were retained for each historical target. The bounded feedback cache now retires the least recently used unused copy under either count or byte pressure. Original targets survive, queued draws flush before destruction, and feedback already bound by the current draw stays pinned. New original resources can reclaim unused feedback capacity. Native and Wasm regressions cycle through targets, verify current GPU contents after recreation, enforce the unchanged limits and retire to zero. The production build contains this fix. A deeper OPSAT run then reaches the 256-resource count limit after reclaimable copies are exhausted. Mission sessions now use a bounded 512-resource profile with the existing 128 MiB resident byte limit; menu defaults remain 256. OPSAT opens at the cave entrance, but the full deeper route recheck remains pending.

Performance remains far below the user's stable 60 FPS target. On the M1 Max / Chromium 151 host, scoped instruction blocks reduce the control-view mean interval from 220.7 to 176.3 ms. Sharing checked memory protection across generated execution reduces it to 126.2 ms, with 147 ms p95 and 157 ms p99, on artifact `f3b04666f0e8fc354a7aeb4b9fc43b7af65b56563dce663bdf0034e7e910f06e`. Protection releases at native calls, waits and repeated-memory operations, and yields at bounded checkpoints. Regression tests caught and corrected worker starvation, unrelated-runtime joining, a movie-worker boundary and indirect tail-call stack growth. Native, Wasm and actual-tail-call tests cover ownership, aliases, physical write versions, faults, cancellation, callbacks and 100,000 indirect transfers. The same ownership and tail-transfer contracts pass with full link-time optimization and SIMD; a full mission build using that compiler mode remains under investigation.

The next profile identifies GPU-service waits and CPU draw preparation alongside remaining memory overhead. Combined clear/draw/copy submission passes native/Wasm staging, fence, retirement and feedback-cache tests. Both real GPU transport topologies pass 776 commands, including copies between draws, partial clears and opaque-alpha conversion, retain eight intentional negative diagnostics and retire to zero resources. However, artifact `1406bc8329a820f6d023138ccfd1d51f8cf80147150b7024e237ec41baf65e30` presents 79 frames in ten seconds, with 127.1 ms mean, 143 ms p95 and 206 ms p99. This does not improve the mean, so combined submission is disabled in the next candidate. No synthetic or component result establishes 60 FPS.

A later profile shows the game thread spending about 30% of its time in synchronous GPU replies and contended locks, while the GPU service is busy only 10% of the time. Draw, batch, clear and copy commands are now posted to the GPU mailbox and collected at the next exchange, so the game thread prepares the next batch while the service records the previous one. Posted failures still surface on the next command. Guest waits with deadlines sleep until the earliest guest deadline or timer, at most 16 ms per clock sample, instead of polling every millisecond; arming a timer wakes its waiters. Mission sessions reuse one guest timer sample per millisecond without the timer lock. The expiry driver writes the tick count and feeds the audio clock only when time advances. The menu invocation observer filters by return address before releasing guest execution. Vertex conversion runs per attribute instead of per vertex and stays bit-identical for all supported formats. Inlining memory block entry into generated code overflowed the V8 worker stack in a deep guest recursion and was reverted.

Two back-to-back pairs on the same host and route, using the development start and ten-second windows at world times 30 and 60, measured these mean presentation intervals:

| Window | Before | After |
| --- | ---: | ---: |
| Opening | 81.6 / 81.3 ms | 59.2 / 58.2 ms |
| Control view | 115.8 / 116.3 ms | 104.3 / 89.9 ms |

Draw waits fall from 10.6 to 17.8 ms per frame to about 1.6 ms, except one control window at 11.5 ms. Native update work in the opening window falls from 26.4 to 14.0 ms per frame. One control window contains a single 1.2 s presentation interval that did not recur. World time advances at the intended rate in every window. This remains far from the 16.7 ms budget. The remaining cost is CPU vertex expansion and per-draw snapshots of guest vertex buffers, generated code with checked memory access, and command waits. Reaching 60 FPS needs GPU-resident vertex buffers keyed by physical write versions, conversion in the vertex shader, and cheaper guest memory access.

The original quick-save/load path has restored the saved player position and rotation within the same selected-ISO session. The load menu renders its real thumbnail and reloads without a page rebuild. This establishes one supported quick-save/load cycle, not death/checkpoint, pause restart or repeated recovery acceptance.

Keyboard controls include original Back and stick-click actions: Tab opens OPSAT, F presses the left stick and 1 the right stick. C and Shift alias crouch and jump; 2/3/4/5 alias vision/gadget directions. Native and Wasm tests verify guest button offsets and release behavior. Full route interaction checks and a gamepad path remain pending.

Private captures, profiles, red/green logs and local game diagnostics remain in ignored `out/m6-audio-fixes/`. The public audio fixture is `tests/browser/audio-output.html`; its `?sink=none` mode is explicitly non-audible. No P6 package is accepted by component tests. Full mission completion twice from normal entry, required rendering, interactions, media and recovery remain outstanding. There is no unresolved external prerequisite.

## Observable milestone

A selected representative story mission can be played from normal menu entry to its completion boundary. Movement, interaction, stealth, required lighting and shadows, audio, mission critical media and checkpoint recovery work together.

One easy opening room is not sufficient evidence. Choose a route containing the mechanics and rendering states the browser port is most likely to mishandle.

## Entry requirements

* M5 accepted.
* Assets continue to stream from the selected local ISO without extraction or upload.
* A representative mission route and manual acceptance script are recorded.
* Known first scene omissions are listed with owners rather than hidden behind optional graphics settings.

## P6.1: Gameplay rendering and stealth parity

Tasks:

* Inventory shader, light, shadow, depth and stencil states encountered beyond the first scene.
* Implement original style shadows and lighting required for navigation, visibility and stealth feedback. Optional enhanced shadows remain excluded.
* Verify alpha tested foliage, fences, projectors, transparent materials, effects, render target sampling and surfaces used by the route.
* Test vision modes and gadget views that change render targets, projection, shader paths or readback behavior.
* Distinguish cosmetic differences from state that changes player visibility, interaction targets or enemy behavior.
* Add synthetic regression fixtures for each newly supported operation where possible. Use private reference captures for game specific comparison.

Deliverable: a rendering coverage ledger for the selected mission and its required modes.

Acceptance:

* The player can interpret shadows and vision modes well enough to execute the recorded stealth route.
* Required modes do not produce missing surfaces, invalid depth, persistent stale frames or validation errors.
* No unsupported shader operation is hidden by a generic success fallback.

## P6.2: Audible browser output

Tasks:

* Replace the timed silent sink behind `src/audio/output.h` with Web Audio output while retaining the existing mixer where practical.
* Choose a bounded AudioWorklet transport after a small latency and underrun experiment. Avoid allocation and blocking work in the audio rendering callback.
* Define channel conversion from the native six channel mixer, gain and clipping behavior. Start with a reliable stereo output if appropriate.
* Handle browser user activation, suspended audio contexts, pause, focus policy and resume.
* Keep consumption timing consistent with guest expectations and mission restarts. Old audio buffers must not play after the next runtime begins.
* Track underruns, queue depth and latency without logging every audio callback.

Deliverable: audible music, speech and positional effects through the real mixer.

Acceptance:

* Representative speech, music and effects are intelligible and spatial cues remain useful.
* Denied autoplay produces an explicit action to enable audio, not a stalled game.
* Pause, resume and mission restart do not cause a sustained click loop, runaway latency or stale sound playback.
* Mixer and transport tests cover wraparound, underrun and rate or channel conversion where implemented.

## P6.3: Movies, scripted sequences and interactions

Tasks:

* Trace real Bink and scripted sequence paths before assuming a browser video element is needed. The existing game code may perform its own decode and rendering.
* Implement required file streaming, decoded frame upload and sound synchronization based on the observed path.
* Test skip, end of movie, scene transition and a movie adjacent to a mission reboot.
* Verify interaction prompts, doors, inventory, required gadgets, camera modes and scripted objectives along the selected route.
* Support the required keyboard actions without depending on browser reserved shortcuts. Add a tested gamepad path once the keyboard route works.
* Leave optional intro skipping as a setting, not a workaround that skips mission critical story or scripting.

Deliverable: the selected route's media, interactions and scripted objectives work.

Acceptance:

* Required sequences finish and return control correctly.
* Media reads remain bounded ISO reads rather than downloaded replacements.
* Audio and picture stay acceptably synchronized over the selected sequence, with measured drift where possible.
* Gamepad disconnect cannot leave a held movement or action state.

## P6.4: Checkpoint and recovery loop

Tasks:

* Exercise death, checkpoint reload, pause menu restart and return to menu.
* Preserve the writable state necessary for checkpoint and save logic within the session.
* Verify GPU readback required for save thumbnails or capture paths. A missing image must not corrupt save data or hang the save operation.
* Validate restart cancellation, read completion invalidation and input reset through repeated recovery loops.
* Test quick save and quick load where the game permits them, using `src/game/quick_save.cpp` as a behavioral reference rather than forcing saves at forbidden points.
* Keep temporary storage warnings until M7 proves persistence beyond the current page lifetime.

Deliverable: recovery works without rebuilding the page or selecting the ISO again.

Acceptance:

* Death returns to a valid playable checkpoint with expected state.
* Pause menu restart and quick load cannot leave old guest workers or pending GPU waits behind.
* Repeated recoveries remain within recorded memory and resource budgets.

## P6.5: Mission completion and baseline profiling

Tasks:

* Execute the full selected route with normal gameplay, required objectives, stealth, audio and checkpoint recovery.
* Reach the authentic mission completion screen or transition request. Do not inject objective completion to satisfy the milestone.
* Profile CPU execution, pipeline compilation, file reads, GPU waits and audio underruns separately.
* Fix correctness bugs before tuning. Optimize only the measured dominant path and retain the relevant regression test.
* Record settings, exact hardware, browser, frame time distribution and known visual deviations.

Deliverable: repeatable manual completion evidence and a bounded performance report.

Acceptance:

* The selected mission can be completed from the normal single player entry path.
* No critical gameplay blocker or recurring GPU validation error remains on the route.
* No game asset network requests or ISO uploads occur during the run.
* A second clean run reproduces the result without hidden developer state.

## Exit checklist

* [ ] P6.1 through P6.5 accepted.
* [ ] Representative mission completes normally.
* [ ] Required shadows, lighting and vision modes work.
* [ ] Speech, music, effects and required media work.
* [ ] Checkpoint recovery and supported quick save or load paths work within the session.
* [ ] Remaining campaign and persistence risks are listed for M7.

## Stop conditions

Do not call the mission playable while stealth feedback is misleading, required objectives cannot trigger, audio timing stalls the simulation or recovery corrupts runtime state. Do not infer full campaign support from this one route.
