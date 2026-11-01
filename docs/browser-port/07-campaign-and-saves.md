# M7: Campaign and persistence work

Status: not started.

Previous: [M6: Playable mission](06-playable-mission.md). Next: [M8: Validation and delivery](08-validation-and-delivery.md).

## Observable milestone

The supported story campaign can be played through its required mission sequence. Profiles, unlocked progress, settings and supported saves persist across a closed page and a fresh browser session.

After reopening the page, the player selects the ISO again, loads saved progress and continues. They do not extract files, recompile code, upload the disc or select it again for every mission.

## Entry requirements

* M6 accepted for a representative mission.
* Browser session controller already supports guest runtime restart.
* Current writable namespaces and save dependencies are documented.
* The supported campaign route and mission inventory are known from the game, not guessed from filenames alone.

## P7.1: Durable writable filesystem

Tasks:

* Inventory actual files and operations for profiles, settings, progress, saves, thumbnails, temporary writes and renames.
* Choose one verified browser persistence backend for this target, such as an OPFS based file store. Prove its completion and flush behavior before connecting the game.
* Keep persistence outside the guest runtime so replacing the runtime cannot erase or race the save service.
* Namespace data by supported game identity and save schema, independent of the ISO filename or local selection handle.
* Preserve file operation ordering and publish a complete save only after its required files are durable. Use staging and a tested commit or recovery protocol rather than promising transactionality from unrelated writes.
* Request available persistence protection where supported and handle denial, quota exhaustion and storage eviction truthfully.
* Define when the UI may report Saved. Do not rely only on asynchronous work started during page unload.

Deliverable: persistent writable mapping with a documented durability and recovery contract.

Acceptance:

* Profile, settings and a real save survive a closed page and reopened session.
* A failed write cannot replace a previously accepted save with an incomplete new one.
* Quota or permission failures produce a visible not saved state and preserve the previous accepted data where possible.
* Selecting the same supported ISO under a different filename still finds the same progress.

## P7.2: Save export, import and corruption recovery

Tasks:

* Export supported writable progress and settings into a documented portable package. Do not include ISO sectors, game assets, full memory or caches by default.
* Validate imported package sizes, paths, schema and game identity before staging changes.
* Provide an explicit user confirmation and backup or rollback path before replacing existing progress.
* Detect incomplete save sets, corrupt metadata and mismatched versions without preventing the browser shell from starting.
* Test failure injection using dedicated fixtures and isolated test storage, never the user's real saves.
* Make storage and temporary versus durable state visible in diagnostics without exposing private filesystem paths.

Deliverable: working export and import with safe recovery and validation tests.

Acceptance:

* Export, clear isolated test storage, import and load reproduces the expected progress.
* Malformed packages cannot escape the save namespace or allocate unbounded buffers.
* Failed import leaves the previous accepted save usable.
* Loss of the local ISO selection does not delete progress.

## P7.3: Mission transition and restart matrix

Tasks:

* List each real single player transition type: new campaign, normal mission completion, checkpoint load, manual load, restart, return to menu and any supported mission selection path.
* Record launch data, persistent files and state ownership for each transition.
* Exercise transitions involving movie completion, asynchronous file reads, pending GPU readbacks and audio queues.
* Verify that only durable or explicitly transferred state crosses the runtime restart boundary. Never retain guest pointers from the previous instance.
* Reuse the selected ISO and asset index while invalidating old request generations and transient caches as required.
* Add restart stress tests that count all runtime owned workers and resources, not only one top level worker.

Deliverable: a transition coverage matrix and focused regression scenarios.

Acceptance:

* Every supported transition type passes from a normal game path.
* Pending old operations cannot signal new handles or overwrite replacement guest memory.
* No transition asks the player to select the ISO again within the same page session.
* Repeated transitions remain within the recorded memory and resource budgets.

## P7.4: Campaign coverage, one mission package at a time

Do not treat campaign validation as one unbounded task. Repeat the following package for each remaining mission and required campaign branch.

Tasks for each mission:

* Record entry conditions, expected exit, special rendering modes, required gadgets, media and save restrictions.
* Reach it through supported game flow or a validated ordinary save, not by patching guest state.
* Play the required objectives through to completion and perform at least one supported recovery or save and load cycle.
* Add newly encountered shader, texture, kernel or file behavior to the compatibility ledger.
* Fix each new behavior in a focused implementation package with a synthetic regression test where feasible.
* Recheck earlier milestone routes if shared memory, rendering or scheduling behavior changes.

Deliverable per mission: an accepted route report or a named blocking issue with a minimal reproduction.

Acceptance per mission:

* The mission's required objectives and completion transition work.
* Required visibility, shadows, audio, movies and gadget paths have no critical omissions.
* Recovery and transition leave the next runtime in a valid state.

Campaign acceptance requires all required missions and identified campaign branches in the declared support matrix, not an arbitrary sample. Optional gameplay permutations can be documented separately.

## P7.5: Fresh session continuation test

Tasks:

* Start from the public facing shell with no retained ISO permission assumption.
* Select a supported ISO, create or select a profile, play, save and close the page only after the save acknowledgment.
* Open a fresh session, select the ISO again and continue the saved mission.
* Repeat using an exported and imported save in isolated storage.
* Verify settings, controls and campaign progress remain consistent after a Wasm build update supported by the chosen schema policy.

Deliverable: an end to end persistence procedure suitable for release regression testing.

Acceptance:

* Continued gameplay reflects the accepted saved state rather than merely showing a save slot name.
* Updated builds either load compatible data or explain incompatibility without overwriting it.
* The process never copies the entire ISO into persistent storage.

## Exit checklist

* [ ] P7.1 through P7.3 accepted.
* [ ] Every required mission package in P7.4 accepted.
* [ ] P7.5 passes from a fresh browser session.
* [ ] Durable save acknowledgment and recovery semantics are documented.
* [ ] Save export and import work without bundling assets.
* [ ] Campaign support claims match the coverage matrix.

## Evidence and stop conditions

Keep a mission and transition matrix with tested builds, input image identity, settings, pass or fail results and known deviations. Record failures as blockers rather than removing hard missions from scope silently.

Do not proceed to delivery with intermittent save loss, stale runtime writes, unexplained campaign softlocks or unsupported version data silently accepted as valid.
