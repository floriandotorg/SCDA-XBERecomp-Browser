# M6: A mission is playable

Status: not started.

Previous: [M5: First 3D mission](05-first-3d.md). Next: [M7: Campaign and saves](07-campaign-and-saves.md).

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
