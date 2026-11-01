# M8: A supported build is deliverable

Status: not started.

Previous: [M7: Campaign and saves](07-campaign-and-saves.md). Parent: [Roadmap](README.md).

## Observable milestone

A reproducible build can be served under a documented configuration. On the supported Mac and browser matrix, a player selects a local ISO, starts single player, saves, closes the page and later continues.

The supported campaign, limitations, hardware budgets and failure recovery are backed by evidence. Public hosting is permitted only if the separate distribution review passes.

## Entry requirements

* M7 accepted, including campaign and durable save evidence.
* No exercised service is still a successful bring up stub.
* Selected ISO remains the sole game asset source.
* A support matrix and named release candidate are proposed.

## P8.1: Performance budgets and focused optimization

Tasks:

* Define measurable budgets for target resolution, frame time distribution, peak Wasm memory, total process memory where measurable, startup, loading, GPU resources and audio underruns.
* Choose budgets from measured hardware and a stated usability target. Do not advertise a frame rate based on CPU microbenchmarks.
* Profile representative heavy scenes, loading transitions, effects, gadget views and sustained streaming.
* Prioritize the dominant measured costs: memory translation, guest dispatch, shader variants, pipeline creation, uploads, synchronization, readbacks or ISO reads.
* Optimize one mechanism per package and retain before and after correctness and performance evidence.
* Keep caches bounded and versioned. Cache shader sources or keys where useful; do not assume native DX12 pipeline binaries are portable browser artifacts.
* Repeat key tests with cold and warm runtime caches. Warm caches must not hide a broken first run.

Deliverable: target specific budgets and a profiling report with regression checks.

Acceptance:

* Selected scenes meet the declared budget or the support claim and default settings are revised explicitly.
* Performance fixes preserve CPU correctness, rendering and saves.
* Long sessions show no monotonic unbounded growth in workers, buffers, index storage or GPU resources.

## P8.2: Browser lifecycle and failure recovery

Tasks:

* Test Pointer Lock denial, keyboard focus changes, fullscreen exit, audio suspension and background throttling under the chosen pause policy.
* Test canvas resize, device loss, failed pipeline creation, allocation failure and runtime worker termination.
* Test selection cancel, unsupported ISO, unreadable ranges, corrupt disc structures and unsupported XBE versions.
* Test changing ISO while stopped and attempting to change it during play. Require an orderly stop before replacement.
* Test save quota failure, unavailable persistence and malformed import using isolated fixtures.
* Report actionable states and recovery choices; avoid frozen canvases and indefinite loading indicators.
* Verify that diagnostics remain local unless the user explicitly exports them.

Deliverable: failure matrix with reproducible tests and expected UI behavior.

Acceptance:

* A recoverable error leaves a usable host shell and does not start a second guest instance.
* No stale read, GPU completion or audio message can target a replacement runtime.
* Storage failure is not reported as a successful save.

## P8.3: Supported platform matrix

Tasks:

* Pin the initial supported Chromium build, macOS and Apple Silicon configurations and record required WebGPU limits and features.
* Test the actual required limits, compression support, thread capacity and memory budget at startup instead of relying on the browser name.
* Validate another available Mac or GPU configuration before making a broader claim; record any access limitation honestly.
* Test additional browsers as separate compatibility packages only after the primary target is stable.
* Verify offline gameplay after the shell is loaded where the deployment supports it. Do not claim a fully offline first visit without implementing a shell cache policy.
* Check that local ISO read throughput and supported layout validation do not vary unexpectedly across the support matrix.

Deliverable: a support statement tied to tested configurations and capability checks.

Acceptance:

* The player can tell why their environment is unsupported before guest execution starts.
* Unverified browsers and mobile devices are described as unverified, not silently supported.
* The first run Select ISO flow and the fresh session save continuation test pass on every declared supported configuration.

## P8.4: Reproducible build and serving package

Tasks:

* Pin toolchain and dependency versions and document the generated input manifest required by the build.
* Make a clean browser build work from existing generated C++ without a Windows toolchain, PE libraries or automatic recompilation.
* Serve Wasm, JavaScript and workers with correct MIME types, caching and COOP and COEP headers. Check all required resources satisfy the isolation policy.
* Version Wasm, host code, worker code, generated adapters, cache keys and save schema coherently.
* Document a Mac developer build and local serving procedure, supported settings and evidence collection workflow.
* Separate distributable runtime artifacts from proprietary generated code, assets, test captures and local evidence according to the distribution decision.
* Preserve third party notices and applicable license requirements.

Deliverable: reproducible local build instructions and a clearly classified output package.

Acceptance:

* A clean local build reproduces the expected input manifest and supported runtime behavior.
* No player side C++ compilation is needed.
* There is no game asset endpoint, extracted folder dependency or ISO upload endpoint.
* No secrets, local absolute paths, proprietary sectors or game memory dumps leak into distributable diagnostics or source maps.

## P8.5: Distribution and player documentation gate

Tasks:

* Review rights for publishing translated game code separately from rights for the repository source and assets.
* Do not assume requiring an ISO makes a hosted precompiled Wasm lawful. Document the selected public, private or local preparation model and obtain appropriate review before public delivery.
* Explain that the player supplies a supported legally acquired ISO and that its contents remain on their device.
* Document supported ISO layouts, executable versions, first launch, audio activation, Pointer Lock, save durability, export and import, storage limits and reselecting the ISO after reload.
* List unsupported multiplayer modes and optional graphics features clearly.
* Provide a concise issue template containing build ID, browser, capability report and reproduction steps without requesting the game ISO or credentials.

Deliverable: player guide, known limitations and a release distribution decision.

Acceptance:

* Public deployment has an explicit approved rights basis; otherwise output remains a private or local build.
* Documentation matches the real Select ISO experience and tested save behavior.
* No claim implies the project supplies or licenses the original game assets.

## Exit checklist

* [ ] P8.1 through P8.5 accepted for the intended delivery model.
* [ ] Budgets are measured on the declared support matrix.
* [ ] Lifecycle and failure tests pass.
* [ ] Clean build and local serving instructions work.
* [ ] Required campaign, transition and save regression routes pass on the release candidate.
* [ ] Select ISO reads locally with bounded memory and zero game asset network requests.
* [ ] Distribution classification and limitations are explicit.

## Final smoke procedure

1. Open a clean supported browser session against the candidate build.
2. Select a supported local ISO using the visible button.
3. Validate and reach the real game menu.
4. Start single player, move and interact in a mission, enable audio and exercise a required vision mode.
5. Save, wait for durable acknowledgment, close and reopen the page.
6. Select the ISO again, load the save and continue actual gameplay.
7. Return to menu and start a mission again without another picker prompt.
8. Export progress and inspect the bounded local diagnostics for validation errors and unsupported operations.

Use agent-browser for automated page interaction and supplement with manual gameplay where required. A successful smoke run supplements, but does not replace, the campaign coverage matrix.

## Stop conditions

Do not publish when the legal distribution model is unresolved, saved progress can be lost without warning, asset reads leave the device, the support matrix is aspirational or critical services remain fake successes.
