# M4: The real menu works

Status: not started.

Previous: [M3: Game boot](03-game-boots.md). Next: [M5: First 3D mission](05-first-3d.md).

## Observable milestone

The actual game menu renders into a browser canvas through WebGPU. Text and menu imagery are readable, keyboard selection and confirmation work, and the user can enter the single player flow.

A browser drawn replacement menu, prerecorded video, rendered triangle or clear color is not this milestone.

## Entry requirements

* M3 accepted with a trace of the real graphics boundary.
* The chosen browser exposes WebGPU under the local serving configuration.
* A portable snapshot of required Xbox render state exists or is extracted during the first package.

## P4.1: WebGPU backend and callback topology

Tasks:

* Add a WebGPU implementation of `gpu::Backend` using the pinned Emscripten WebGPU integration.
* Create device, queue, canvas configuration, resize handling, validation reporting and explicit device loss handling.
* Choose where GPU API work runs and how guest commands reach it. Verify this context can yield while guest threads wait.
* Audit `src/gpu/render_thread.cpp` for SSE streaming copies and assumptions about blocking waits. Use portable copies initially; optimize only after measurement.
* Define resource IDs, command ownership, state snapshot lifetime and shutdown order. Do not send borrowed guest pointers across asynchronous boundaries without a lifetime contract.
* Exercise a synthetic draw, texture upload, resize and readback to prove the topology independently of game shaders.

Deliverable: a small backend test scene using the same queue and resource machinery as the game.

Acceptance:

* Validation errors reach the diagnostic UI with the responsible command or resource.
* GPU completion callbacks execute while guest workers are blocked.
* Resizing and destroying the backend release resources without leaving waits unresolved.

## P4.2: Menu shader and texture path

Tasks:

* Capture the menu's actual state combinations and required vertex and pixel operations from `src/gpu/shaders.cpp` and `src/xdk/d3d8.cpp`.
* Build a small prototype of direct WGSL generation or a verified HLSL translation path. Choose based on semantic coverage, runtime cost and integration effort, not source language preference alone.
* Preserve coordinate conventions, clip depth, matrix ordering, register constants, interpolation and shader precision where observable.
* Map texture formats through an explicit WebGPU format layer. Native texture helpers return host formats and require auditing rather than direct reuse.
* Support menu image and font formats, including swizzling, palettes, channel order, alpha and compressed textures. If BC support is unavailable, use a tested decode path or report the required capability.
* Build deterministic synthetic shader fixtures with known pixel results. Store generated WGSL and concise state descriptions on compilation failure.

Deliverable: shader and texture support for the menu's observed state set.

Acceptance:

* Synthetic pixel checks catch channel swaps, incorrect UV orientation, alpha errors and wrong constant layout.
* Real menu shader variants compile without substituting one generic shader for unsupported states.
* Required adapter limits and optional texture compression capabilities are recorded.

## P4.3: Menu draw state, presentation and readback

Tasks:

* Implement the menu's vertex decoding, indexed draws, topology conversions, viewport, scissor, culling, blending, depth and stencil state.
* Support the resource creation, copies, resolves and front buffer behavior observed on this path.
* Build a pipeline key that includes all shader and fixed state affecting output. Keep state cache behavior deterministic.
* Implement the exercised `Sync`, `SyncLate` and delivery paths honestly. Track source version, completion and ownership of readback staging memory.
* If a readback path is not reached, retain a named unsupported diagnostic until M5 rather than claiming it is implemented.
* Present the actual game front buffer to the canvas with correct aspect and resize behavior.

Deliverable: real menu frames rendered by the WebGPU backend.

Acceptance:

* Text is readable, imagery has correct orientation and alpha, and selection highlights are visible.
* Synthetic state changes cannot reuse an incompatible cached pipeline.
* No WebGPU validation errors remain on the menu path.
* Idle menu operation stays within a recorded resource budget and shows no continuing GPU resource growth.

## P4.4: Keyboard menu input and single player route

Tasks:

* Bridge browser key events to the existing guest input and controller state expected by menu code.
* Implement press, release, repeat, focus loss and reconnect or reactivation behavior without stuck buttons.
* Navigate real profile, confirmation and single player selection screens, using temporary profile data if durable storage is not ready.
* Explicitly reject multiplayer and unsupported dashboard navigation. Do not require `sc4_adv.xbe` merely to display the shared menu.
* Provide browser Start, Stop, diagnostics and an explanation of temporary storage. Keep the game menu separate from this host UI.

Deliverable: a real navigable menu with an identified New Game or mission start boundary.

Acceptance:

* Keyboard input changes the real selected item, confirms and returns from submenus.
* Losing focus clears held input and does not queue an uncontrolled burst of repeat events.
* Reaching the mission start boundary produces a documented launch request or loading transition.
* Refreshing or restarting still reproduces the menu from a clean session.

## Exit checklist

* [ ] P4.1 through P4.4 accepted.
* [ ] Real menu is visible and navigable.
* [ ] Shader, texture and state handling pass focused synthetic tests.
* [ ] Every exercised readback operation is real, not a successful stub.
* [ ] No menu path WebGPU validation errors remain.
* [ ] Single player selection reaches the next stage's boundary.

## Evidence

Record shader variant count, pipeline count, GPU resource counts, first visible frame timing, canvas size and adapter details. Compare private captures with the reference where available; use public synthetic fixtures for automated pixel tests. Browser automation must use agent-browser.

## Deliberately deferred

Full 3D material coverage, gameplay mouse capture, sound output, optional enhancements and durable saves are not required here. Do not defer an operation the actual menu requires merely because its name sounds like a later feature.

## Stop conditions

Do not progress with replacement UI masquerading as the game menu, generalized shader fallbacks that hide unsupported operations or synchronous waits that stall GPU callbacks.
