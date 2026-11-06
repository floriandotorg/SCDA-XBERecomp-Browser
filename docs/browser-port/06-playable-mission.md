# M6: A mission is playable

Status: active. User playtesting confirms audio works correctly after the streaming fixes. Rendering, performance, full mission completion and recovery remain under investigation. M5 is accepted; P6.1 through P6.5 remain unaccepted.

The requested performance target is stable 60 FPS. On the M1 Max / Chromium host, the default mission build now presents at display rate on the tested route: 16.0 ms mean in the opening window, 16.4 ms in the control window and 16.1 ms over a 60 second walk, with no interval above 25.5 ms. See [the 60 FPS work](#60-fps-mission-rendering) below. Deeper areas and other hosts are not yet measured.

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

Native and Wasm regressions cover night-vision blend factors and constants, thermal dependent texture modes 15 and 16, bounded upload identity history and pipeline descriptor deduplication. Both vision modes have rendered in the original mission. All 125 synthetic shaders compile. Both GPU transport topologies pass 758 commands, retain only four intentional negative diagnostics and retire to zero resources. This includes exact D24/stencil and D16 readback, partial and delayed replies, CPU row-padding preservation and depth upload boundary values. No general visual-parity acceptance is claimed.

The dark models and black sky came from lost GPU contents. The level builds its 64x64 normalization cube maps face by face with `CopyRects` and then releases each face surface. The copies only existed in GPU face resources, and `ReleaseResource` destroyed them, so the cube textures were later uploaded from zeroed guest memory. Every dot3 bump-lit pass (characters, the boat, crates) then sampled black light vectors, and the sky lost its texture too. A released resource that still holds unwritten GPU contents is now read back into guest memory before it is destroyed (`tests/browser/released-surface.test.mjs`). The opening now renders as dusk with a lit sky. Frame times are unchanged: 16.0 ms opening, 16.4 ms control and 16.1 ms over the 60 second walk, max 25.2 ms. Back-lit characters still look dark; without a reference this is unverified.

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

The GPU-resident buffer candidate retains raw guest vertex ranges from the lowest referenced vertex through the last used attribute. Physical ranges use canonical addresses and `PhysicalWriteVersion`; virtual memory without write versions re-uploads on every draw. Generated scalar and block stores, `Memory::Write`, repeated copies/fills, file-read delivery and host writers through leased `Span` were reviewed. A mutable physical span now suppresses version reuse until its outer backing lease ends, preventing a later write through an already acquired pointer from reusing stale geometry. Native and Wasm tests cover aliases, invalidation, unchanged-buffer hits, unversioned fallback and eviction under a four-resource limit. Raw ranges are packed into shared GPU buffers, so cached ranges do not each consume a GPU resource slot. Pools share the renderer's existing count and byte limits with textures and feedback copies, including unused capacity and allocation padding. View metadata is separately bounded to at most 32 entries per configured resource, capped at 16,384 entries. Eviction coalesces free ranges, and a pool can be retired under texture pressure. Staged draws retain their previous bytes when a cached range changes; disjoint uploads can stay ahead of the same draw batch.

Pipelines describe each guest stream's stride, attribute offsets and native WebGPU formats. Generated WGSL expands missing components and decodes packed 11:11:10 normals, BGRA colors, normalized and unnormalized shorts, three-component byte/short attributes and type `0x72`. Layouts exceeding mailbox capacity or WebGPU stride, alignment, stream or attribute limits retain the existing checked CPU path. All 148 synthetic shaders compile; GPU float readback matches the CPU reference exactly for the 19 tested guest-format cases and exhaustive signed-short, byte and packed-normal component values. Integer-rounded normalization avoids the one-ULP errors found with direct WGSL division. Index data arrives at the renderer as an owned host copy, so expanded index buffers use exact normalized 16-bit content keys rather than assuming a physical write version still describes that copy. Shifting every input index by the same base reuses the same cached expansion. Cache hits avoid primitive expansion and the guest-memory lease; only a miss constructs expanded 32-bit GPU indices. Quads, fans and line loops retain their original expansion and rebase against the uploaded vertex range. Both real-GPU transport topologies cover resident draws, batches and buffer updates. Dirty uploads and their consuming draws share a posted command, updates retain their GPU allocation, and failures keep the existing deferred validation and next-exchange reporting behavior. Uploads run before the ordered draw phase; overwriting a range used by an earlier staged draw first submits that draw. Cancelling an unsubmitted frame discards its planned allocations without sending invalid destroy commands.

The final build succeeds with the prescribed Emscripten command. Focused native/Wasm tests cover cache hits and invalidation, normalized-index reuse, all ten primitive expansion counts, layout fallback, mutable spans, pooled allocation, staged version ordering and cancellation. Native/Wasm shader and pixel-shader suites and posted-command tests pass. Both real-GPU transport topologies complete 894 commands and retire to zero resources; upload-batch failures retain deferred reporting. The existing general menu-backend suite still has its unrelated eager-submission fence assertions, so it is not used to mask or gate these independently passing residency tests.

The preserved baseline is `out/perf-60/resident-baseline-web`, with game Wasm SHA-256 `7bc7fc6883488649696d4041ac47228c24177c52646981b83c2662f8fb87b0b7`. The final candidate is `out/perf-60/resident-index-cache-web`, with game Wasm SHA-256 `ee91bdb8cf41f6b75b4820082725995202f54ec46b763f558d0409a46d9721fa` and GPU Wasm SHA-256 `9ee98b74187cf92d82ac8a332d365b3a0600d9d32fa37113bda48f3902204747`. Earlier candidates with synchronous replacement or separate per-upload commands were rejected. Two baseline/candidate rounds ran back to back with stale agent-browser sessions closed and no builds or tests running. The route uses development start and ten-second windows at world times 30 and 60. Raw results are `out/perf-60/resident-index-final-{0,1}-{base,opt}.json`.

All intervals and wait counters below are milliseconds; waits are per presented frame. Command wait includes its draw/readback/upload subsets and must not be summed with them. Dirty uploads bundled with draws are charged to draw wait, so zero standalone upload wait does not mean zero uploads.

| Round | Window | Build | Mean / p95 | Command wait | Draw wait | Readback wait | Upload wait |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 1 | Opening | Before | 67.7 / 84 | 15.88 | 8.47 | 1.39 | 0.08 |
| 1 | Opening | After | 47.7 / 60 | 5.60 | 0.23 | 1.51 | 0.03 |
| 1 | Control | Before | 90.6 / 94 | 11.45 | 1.10 | 1.23 | 0.00 |
| 1 | Control | After | 93.8 / 89 | 16.52 | 10.16 | 1.53 | 0.00 |
| 2 | Opening | Before | 58.3 / 82 | 9.01 | 1.61 | 1.48 | 0.05 |
| 2 | Opening | After | 54.2 / 59 | 11.08 | 5.92 | 1.46 | 0.03 |
| 2 | Control | Before | 90.2 / 95 | 11.30 | 1.11 | 1.11 | 0.00 |
| 2 | Control | After | 109.3 / 139 | 28.81 | 22.46 | 1.55 | 0.00 |

Round one has no detected heavy competing transform/compiler/VM load. Round two is qualified: the VM exceeded one CPU core twice during baseline loading and three times during the candidate's later visual-capture hold, outside the timed windows. The per-run `-load.json` files preserve the monitor evidence; these are not two unqualified clean pairs. Earlier trials affected by sustained transform jobs and VM load are excluded. Approximately 1.2-second presentation intervals remain included: round-one baseline opening and candidate control, and round-two candidate opening and control. Opening means improve in both pairs, but control means regress and the second candidate has higher draw waits. The requested 60 FPS and a repeatable control-view improvement are not established. More profiling and a fully idle-host repeat are needed before accepting this as a performance improvement.

Private final-artifact captures `out/perf-60/resident-index-final-1-opt-ice.png` and `resident-index-final-cave.png` show no obvious new missing or garbled geometry compared with `resident-visual-base-ice.png` and `resident-baseline-cave.png`. Both final captures report no validation errors. The cameras differ, so this is not a pixel-parity claim. Dark models and black sky remain visible in both builds. No P6 performance acceptance follows from the component checks.

A temporary counter build shows the cache itself works: about 95% of opening and 92% of control-view vertex ranges hit, every index range hits, and uploads stay near 30 KiB per frame. About 41 control-view draws per frame bind a new vertex range every frame. These entries fill the 16,384-entry view table during the opening. After that, every new entry evicted through a linear scan of the whole table. That explains why opening improves while the control view regresses. Eviction now uses an ordered recency index, and the resident cache suites pass on native and Wasm. Under concurrent VM load of about 365% CPU, the control view measured 103.1 ms mean with 65.7 ms render work, against 106.3 ms and 70.7 ms for the same build before the fix. Simulation work rose to 30 to 38 ms per frame in both runs, so these runs are not a valid comparison with earlier results. Draw waits stay near 0.3 ms in both runs, so the earlier 10 to 22 ms draw waits did not reproduce. Posted command data is about 2.3 MiB per control-view frame, mostly per-draw shader constants. Ranges that change every frame should bypass the persistent table. A clean repeat on an idle host is still required.

The 2026-11-05 guest-memory investigation preserves the starting web artifact in `out/perf-60/memory-baseline-web`, game Wasm SHA-256 `9d54ec4a705ab05a6a47b1e87ae7820b713552d729f25e4688998c120652d8d4`. The retained runtime-only artifact is `out/perf-60/memory-runtime-web`, SHA-256 `49f920597166ee53a734c8465481d69aaede7ea1c35883af28c5f9af65a5a022`, restored into `out/boot-m5/boot-web`. The prescribed Emscripten build succeeded for that implementation and both subsequent, rejected cache experiments. No generated source was edited. `Invoke` delegates its checkpoint and session validation to `PollLoop` or `Poll` for generated and host-thunk targets; kernel and invalid high targets retain explicit checks. `Poll` loses its second identical session check. Cancellation and stack-budget checks still run on every invocation path, with the existing bounded execution yield, timer polling and host-boundary pause behavior. No stack checks were sampled away. Comparison of all 32,615 matched generated Wasm functions finds identical local counts and initial linear-stack allocations in the retained artifact.

Two baseline/candidate rounds ran back to back with no local builds or tests running, using the existing development-start route and ten-second windows at world times 30 and 60. An additional pair collected traces and extended gameplay after the timed windows. All numbers below are milliseconds; update is the harness's `updateMicroseconds` delta divided by 1,000 and by runtime frames. Presentation mean/p95 and simulation update describe different counters.

| Round | Window | Before mean / p95 | After mean / p95 | Before update / frame | After update / frame |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | Opening | 50.3 / 63 | 49.5 / 61 | 13.85 | 13.47 |
| 1 | Control | 80.3 / 87 | 79.7 / 84 | 28.03 | 27.53 |
| 2 | Opening | 55.6 / 62 | 48.7 / 60 | 14.77 | 13.67 |
| 2 | Control | 80.2 / 86 | 80.2 / 86 | 27.93 | 27.93 |
| 3 | Opening | 62.0 / 87 | 63.9 / 86 | 21.31 | 21.35 |
| 3 | Control | 97.0 / 101 | 96.6 / 103 | 34.01 | 34.27 |

Round one detected unrelated TypeScript compilers during baseline loading, before its timed windows. Round two detected VM activity during loading and opening in both artifacts. Round three detected VM activity during candidate loading, the opening boundary and later holds; baseline VM activity appeared during its later hold. Thus these are not two unqualified idle-host pairs. Per-run `-load.json` files retain the evidence. Raw measurements are `out/perf-60/memory-runtime-pair-{0,1}-{base,opt}.json` and `memory-runtime-final-0-{base,opt}.json`. The early opening improvement does not reproduce in round three. Control update improves by 0.50 ms once, ties once and regresses by 0.26 ms once. Control p95 regresses from 101 to 103 ms in round three. No repeatable control-view benefit or 60 FPS claim follows from this change.

Ten-second runtime-evidence samples report world deltas from 9.34 to 10.03 seconds across the retained comparisons; they do not establish exact ten-second progression in every individual window. Longer final holds advance world time 191.96 seconds over 191.74 seconds of page-clock time before and 136.90 over 136.49 seconds after. The retained candidate reaches world time 209.28 seconds, approximately 280 seconds of page uptime, without `WASM_ABORT`, displayed fault detail or validation errors. The measured candidate hold is shorter than its requested three-minute host timer, so the recorded page-clock duration is reported here. This is sustained stationary gameplay, not mission completion or a new route acceptance. Final captures, traces, console logs and `.json.soak.json` records use the `memory-runtime-final-0-{base,opt}` prefix.

Separate 20-second Chrome traces, collected after the timed windows, give generated-code self time of 27.6% before and 27.6% after, memory 15.5% and 16.3%, and runtime bookkeeping 7.5% and 7.4%. Inclusive `Checkpoint` samples fall from about 645 to 557 ms over traces of 20.37 and 20.23 seconds; inclusive times overlap and must not be added. The `threads.mjs`, `analyze.mjs` and `parents.mjs` outputs are retained in `out/perf-60/memory-runtime-{base,opt}-*.txt`. These profiles support a small bookkeeping reduction, not a measured simulation speedup.

Page-table caching was tested in two layouts, both reverted. A two-pointer scope caches the table returned by out-of-line `BeginBlock`; its single exploratory gameplay run gives opening mean/p95 46.9/58 ms with 12.82 ms update and control 76.9/81 ms with 25.86 ms update. However, 874 generated functions allocate more linear stack and 15,523 declare more Wasm locals. A revised heap table carries an owner reference so each scope remains one pointer. Its single exploratory run gives opening 51.7/57 ms with 13.77 ms update and control 74.9/79 ms with 25.12 ms update. Even this layout grows `sub_003655A0` from 32 to 128 bytes of initial linear stack, and matched generated local declarations rise from 244,574 to 261,869. These unpaired trials are not accepted speedup measurements. Neither brief trial aborted, but neither satisfies the no-frame-growth constraint. Sources, artifacts, frame comparisons and trial results remain under `out/perf-60/memory-cache-two-pointer*` and `memory-table*`.

Raising both adapter limits from 64 to 256 reduces static scopes only from 166,103 to 164,724, or 0.83%; this was evaluated without changing the adapter and was not gameplay-benchmarked. Existing call, label, wait, atomic and loop boundaries remain intact. Physical scalar version marking already takes no additional lock inside an execution/block lease, so no new non-locking marker was implemented or timed. Epoch coalescing was not substituted for the current per-write contract. A direct physical or image mapping remains deferred, without a performance measurement: it must retain per-page commitment checks, both physical aliases, checked cross-page and wraparound fallback, and shared physical write versions. These unimplemented directions have no claimed benefit.

The heap page-table layout is now retained. Each block scope holds one reference to a table that carries its owning memory, so block loads and stores index the table without reloading or emptiness-checking the page vector. Two interleaved candidate/current pairs on the same host measured opening update of 13.09 / 12.59 ms after against 14.32 / 12.84 ms before, and control update of 25.77 / 26.24 ms after against 27.09 ms before. The second current-build control window had a 20.9 ms draw wait and is excluded. Control means were 76.4 / 76.1 ms after against 78.3 ms before. The same unexplained draw-wait spikes therefore also occur on builds without resident buffers.

Generated frame growth was the reason this layout was first rejected. Native stack headroom is now measured directly. Chrome ignores `--js-flags=--stack-size` for workers, so the probe patches only the JavaScript glue of an artifact copy, recursing a fixed number of JavaScript frames before each pthread entry, and boots the development start until world time 20. The guest recursion through `sub_00060541` / `sub_000658A8` decides every failure. The previous build survives 150 reserved frames and fails at 187 in two runs. The page-table build survives 187 and fails at 250. A worker holds about 5,400 such frames, so both builds run within a few percent of a native stack overflow, and the page-table layout does not reduce that margin. This margin is a standing crash risk for deeper scenes, other browser versions and any future change that enlarges frames in that recursion. Changes to generated-code frames should be checked with this probe rather than by counting locals. The probe is `out/perf-60/stack-probe.mjs` with `make-reserve.sh`.

The retained runtime implementation passes all 48 focused memory, block, execution, invocation-observer, timer-clock and dispatcher tests. Eight CPU fixture tests pass with the cache experiments; the full `BROWSER_FULL_CPU` contract executable was not rebuilt or run in this investigation. Cache test runs under concurrent linking had transient generic execution-ownership assertion failures in two Wasm variants. The index-source scratch copy passes all four execution variants on an idle run, and each cache implementation also passes all four on an idle repeat. The failing assertion was not localized, so no proven pre-existing-failure attribution is made. Final retained-source memory-pages and execution reruns pass all six tests. A new pointer-sized-scope assertion was confirmed failing against the two-pointer experiment before the revised layout passed it; it also passes on the restored implementation. Execution assertions now report their source line for future diagnosis. No failing contract was weakened, and neither the known timer-driver failure nor the flaky synchronization-provider suite was used as a performance gate.

### 60 FPS mission rendering

Mission builds now default to `BROWSER_FLAT_MEMORY`. Generated code translates guest addresses through a per-page table owned by `Memory` instead of checked block scopes: `base` points at the table, each committed page holds its host offset and unmapped or uncommitted pages translate into the trap window above the Wasm heap. Stores mark the written host page; `PhysicalWriteVersion` folds these marks into the existing per-page versions, with 8-page block maxima for wide texture ranges. Physical aliases share storage, so both windows reach the same pages. Host `Read`, `Write` and `CheckRange` take the same unlocked path for committed contiguous ranges. The trade-off is that accesses straddling two unrelated regions are no longer faulted. Set `-DBROWSER_FLAT_MEMORY=OFF` for the checked layout. Flat generated frames also no longer carry block scopes. The `sub_00060541` recursion now survives 1,000 reserved JavaScript frames (it failed at 187 before) and overflows at 2,500.

Runtime polling runs on a thread-local budget: loop back edges and indirect calls count down 2,048 steps, kernel and SDK entries cost 16. Indirect calls use a dense guest-address table. The timer source reads the worker clock instead of proxying to the main thread, and clamps cross-worker skew instead of faulting. Dispatcher signals wake waiters only when the signaled object has one, which removes the thundering herd on guest mutex releases.

GPU commands that need no reply go through a 16 MiB shared command ring, so posting never waits for the service to consume the previous batch. Clear and copy are batched with draws. Present returns with one frame in flight, and fences and busy queries retire against the previous Present. Late surface readbacks start asynchronously and arrive in four shared readback slots without a mailbox exchange. A lock that finds its readback still in flight republishes the previous snapshot. The mailbox, ring and slots share one SharedArrayBuffer; `webgpu-pthreads.js` checks its exact size.

Per-draw host work avoids copying the 40 KiB render state, keys pipelines by a shader serial rather than WGSL text, measures snapshot wire size arithmetically, hashes the shader, pipeline and resident buffer caches, keys index buffers by guest address and physical version, and journals push-buffer state instead of copying the state twice.

Measured with `out/perf-60/bench.mjs` (`PERF_SOAK=<seconds>` adds a scripted walk, `PERF_TRACE=<path>` records a CPU profile, `out/perf-60/top.sh` summarizes it). All values are milliseconds.

| Window | Before mean | After mean / p95 / max | After update / render per frame |
| --- | ---: | ---: | ---: |
| Opening | 46.6 | 16.0 / 19 / 22.4 | 3.13 / 7.43 |
| Control | 75.2 | 16.4 / 19 / 23.3 | 3.53 / 10.53 |
| 60 s walk | | 16.1 / 19 / 25.5 | 4.01 / 7.36 |

Captures `out/perf-60/flat16cap-ice.png` and `headcap-ice.png` show the same geometry, fog, lighting and HUD at the opening. `flat18soak2-ice.png` shows the walked-to area rendering normally. The general menu-backend suite still stops at its pre-existing eager-fence assertions, so its later Present and readback checks were updated but not run to completion.

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
