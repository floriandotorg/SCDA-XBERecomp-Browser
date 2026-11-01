# M5: A controllable 3D mission runs

Status: not started.

Previous: [M4: Menu](04-menu-works.md). Next: [M6: Playable mission](06-playable-mission.md).

## Observable milestone

Starting a new single player game from the real menu loads an actual mission. The player can move and control the camera inside a real textured environment with baseline lighting and a visible HUD.

A static scene, developer free camera, replayed frame or isolated model viewer is not this milestone. Full stealth correctness and mission completion belong to M6.

## Entry requirements

* M4 accepted.
* Required story assets are accessible without a full game preload.
* A specific first mission and reproducible start procedure are selected. Prefer the first normal campaign path rather than an undocumented developer jump.

## P5.1: Guest reboot and mission launch

Tasks:

* Trace New Game through `XLaunchNewImageA`, `HalReturnToFirmware`, launch data and any loading hooks to establish the actual transition contract.
* Replace `CreateProcessW` and executable based image selection with a browser runtime restart for the same story module.
* Transfer the launch page and required settings or profile state to the session controller before tearing down the old guest runtime.
* Define the complete shutdown boundary: guest pthread workers, file requests, GPU submissions, audio sink, event waiters and detached loops must not survive as a second live game instance.
* Preserve assets and temporary writable state across the restart without treating process local pointers as transferable state.
* Reject Versus and unsupported executable paths with a safe single player return path.
* Make repeated return to menu and mission start transitions reuse the same contract rather than adding special cases.

Deliverable: a tested session controller that can replace the menu runtime with the requested mission runtime.

Acceptance:

* Real menu launch data is consumed by a fresh runtime and the expected mission load begins.
* A repeated restart stress test does not grow active workers, handles or GPU resources without bound.
* Old asynchronous completions cannot write into the new runtime's guest memory.
* Cancellation during transition leaves a recoverable host shell rather than two running guests.

## P5.2: Mission loading and sustained streaming

Tasks:

* Trace mission archive and media reads, decompression helpers, asynchronous completions and loading thread waits.
* Remove browser file adapter assumptions based only on the startup working set.
* Add bounded streaming caches, staging buffer reuse and observable loading progress derived from real work.
* Verify CPU pointer spans used by decompression and package loaders across translated memory regions.
* Retain the game's scheduling and background copy behavior. Audit `src/game/bink_io.cpp` and related loading hooks for worker starvation.
* Handle missing or corrupt mission files with a named failure and retry or restart path.

Deliverable: the selected mission reaches game world initialization through normal loading.

Acceptance:

* Loading completes without a full asset preload, busy spin or unbounded memory growth.
* Reads canceled by a restart cannot signal stale event objects.
* Loading failures identify the missing file or failed operation rather than presenting an infinite spinner.

## P5.3: Baseline 3D graphics

Tasks:

* Inventory shader instructions, vertex programs, material states and texture types used by the selected scene.
* Add skinned and static vertex paths, transforms, lighting, texture stages and other operations actually encountered.
* Verify perspective divide, depth range, culling, alpha testing or discard, blend state, stencil and coordinate conventions using focused scenes or synthetic fixtures.
* Implement the render target, depth, copy, resolve, resource reuse and sampled feedback paths used during loading and gameplay.
* Finish readback behavior exercised by gameplay. Preserve which completed version `SyncLate` delivers and when new results become visible to guest code.
* Map unsupported WebGPU fixed state through an explicit emulation strategy or a blocking diagnostic, not a visually plausible silent substitution.
* Begin at conservative internal resolution and original style settings. Disable only optional enhancements, not gameplay state.

Deliverable: a correctly oriented textured scene with player model, baseline lighting and HUD.

Acceptance:

* No missing geometry, exploding skinning, inverted depth or systematically swapped texture channels in the selected test area.
* Required shader variants compile and the WebGPU validation log is clean.
* Camera movement does not cause stale buffer reuse or intermittent corrupted uploads.
* Current visual omissions are enumerated; missing stealth shadows may remain only as an explicit M6 blocker.

## P5.4: Movement, mouse look and simulation timing

Tasks:

* Map keyboard actions and relative mouse motion into the existing game input layer.
* Acquire Pointer Lock only from a user gesture and provide visible unlock and resume behavior.
* Implement sensitivity, action press and release, pause and focus loss behavior. Avoid binding browser reserved shortcuts as the only way to perform a required action.
* Verify that the chosen game frame cap and timer behavior advance simulation at the intended speed instead of scaling with browser presentation cadence.
* Measure guest execution, render submission, frame delivery, readback stalls and asset waits separately.
* Establish an initial device specific frame time and memory baseline. Publish the tested settings rather than claiming universal performance.

Deliverable: the player can move, look, pause and resume in the real scene.

Acceptance:

* Keyboard and mouse control the game, not only a host camera.
* Pointer Lock denial and focus changes recover without stuck movement.
* Pausing or backgrounding according to the chosen policy does not inject a giant simulation step on return.
* A documented ten minute traversal remains responsive without resource growth or repeated invalid state diagnostics.

## Exit checklist

* [ ] P5.1 through P5.4 accepted.
* [ ] New Game from the menu reaches the selected mission.
* [ ] Mission restart is a real runtime lifecycle operation.
* [ ] Real geometry, textures, player and HUD render.
* [ ] Keyboard movement and mouse look work.
* [ ] A repeatable traversal produces performance and resource evidence.
* [ ] Remaining gameplay and visual blockers are listed for M6.

## Evidence

Record the mission, exact route, settings, runtime restart counts, peak memory, file cache budget and frame time distribution. Keep screenshots and game traces local unless distribution rights are clear.

## Stop conditions

Do not call the result playable if it reaches 3D only through a hardcoded scene shortcut, skips the real launch data contract, has broken gameplay speed or loses state on every mission restart.
