# M7: Campaign and persistence work

Status: active. P7.1 and P7.2 are implemented and pass in Chrome; P7.3 to P7.5 have not started. See [Current implementation](#current-implementation).

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

## Current implementation

### Writable inventory

The game writes only to `\Device\Harddisk0\Partition1` (E:). `Partition3` (Z:) is the utility cache, which the game formats at boot and uses as scratch; it is not persisted. Observed on the story image (title ID `5553005E`):

| When | Files |
| --- | --- |
| First boot | `TDATA\5553005E\`, `UDATA\5553005E\TITLEMETA.XBX` (58 bytes), `TITLEIMAGE.XBX`, `SAVEIMAGE.XBX` |
| New profile | `UDATA\5553005E\<profile>\PROFILE.SAV` (9,804 bytes), `SAVEMETA.XBX`, and twelve preallocated slots: `SINGLE0..5.SAV` and `MULTI0..5.SAV` (2,500,000 bytes each) with `*IMAGE.SAV` thumbnails (32,896 bytes) |
| Quick save | `SINGLE0.SAV`, `SINGLE0IMAGE.SAV` and `PROFILE.SAV`, rewritten within 3 ms of each other |

A profile allocates about 30.4 MB, almost all zeros. The 32 MiB profile volume therefore holds exactly one profile; a second one fails with the game's own "not enough free storage" message. Loading a save and starting a mission write nothing to E:.

Saves are signed with the virtual hard disk key (kernel export 323), and the profile also depends on the drive serial. Both were random per page load. They now travel with the saved files, so a restored profile verifies in a later session.

### Architecture

* `TemporaryFilesystem` (E:) counts a revision on every successful mutation and writer holds for every open handle with write, delete or attribute access. `Serialize` writes a canonical image; file bytes are chunks of a zero run and a literal run, so a fresh profile is about 18 KB instead of 30 MB. `Restore` validates the whole image before replacing anything: canonical upper case paths inside the namespace, sorted order, existing parent directories, attributes, sizes, reservations, capacity and entry limits, chunk bounds and no trailing bytes.
* `StoragePublisher` (`src/browser/storage-publisher.h`) is a host thread owned by `StoryState`, not by the guest runtime, so it survives mission reboots. Every 100 ms it reports a pending change once and publishes a snapshot when E: has no open writer and has not changed for 750 ms. The snapshot carries the console identity. It goes through a proxied call to the boot worker, which posts it to the page; retired runtime generations are dropped there.
* `save-store-worker.mjs` owns the origin private file system under `scda/<namespace>/<xbeSha256>`. The namespace is `progress`; `?progress=<name>` selects an isolated one for tests. The XBE hash is the executable the selected ISO was checked against, so a renamed ISO finds the same progress, and losing the ISO selection does not touch it. A Web Lock makes one tab the writer; another tab starts without saved progress and says so.
* `save-store.mjs` keeps two slots. A commit writes the older slot through a sync access handle, calls `flush()`, closes, reads the file back and compares every byte. Only then is the commit accepted. A torn, failed or silently dropped write leaves the newer slot intact. Loading picks the newest slot that passes the container hash and the image validation.
* `save-service.mjs` on the page coalesces images that arrive during a write (only the newest is written) and tracks unsaved state. The next start in the same page boots from the newest accepted image, after any write in flight has finished.

### Durability and recovery contract

* **Saved** appears only after the page has received a published image, written it, flushed it and read it back intact. The top of the screen shows "Saving" while the guest has unpublished changes or a write is in flight, then "Progress saved".
* While anything is unsaved, closing the page asks for confirmation (`beforeunload`). Nothing relies on work started during unload.
* Quit to main menu waits up to 4 seconds for pending changes to be written before the runtime stops. If a runtime stops with unpublished changes, the page reports "The game stopped before its last changes were saved."
* A failed write (quota, I/O, another tab) shows "Progress not saved: <reason>" and keeps the previous accepted progress. The next successful write clears it.
* If every stored slot is damaged, each is copied to `damaged-<time>-<slot>` (at most eight copies, never deleted). The game starts without progress, and the notice stays in Options, Progress.
* Progress written by a newer schema or container version is reported, never loaded and never overwritten; saving is off for that session.
* After the first accepted write the page asks for persistent storage (`navigator.storage.persist()`). Options, Progress shows the usage and whether the browser may clear it.

### Export and import

Options, Progress has Export and Import. The `.scdasave` file is the same container as a slot: magic, version, JSON header (game identity, schema, sequence, time, page settings), the storage image and a SHA-256 over everything before it. It holds no ISO data, assets, guest memory or caches. Import checks the size limit before reading the file, then the hash, game identity, schema and every image bound before it asks for confirmation. The current progress is copied to `backup.scdasave` before the import commits, and Restore in the same tab undoes the import. Import and Restore are unavailable while a game runs.

### Evidence

Tests: `temporary-filesystem` (persistence and sparse modes), `storage-publisher` (images, polling and thread) and `file-provider` (writer holds) on native ASan/UBSan and Wasm; `save-store` and `save-service` in Node with injected torn, failed, dropped and locked writes, damaged and newer slots, foreign and malformed packages and path escapes.

Chrome 2026-11-07, build of this branch, isolated namespaces and a dedicated Chrome profile:

1. Fresh browser: Start, Solo, New Profile (SAMFISHER) committed. Quit through the pause menu, browser closed.
2. New browser process, ISO selected again: the store loaded sequence 2, the game listed SAMFISHER under Profiles and opened its Solo menu.
3. New Game, Normal, Iceland. After a short walk, quick save (F5) committed within a second. Browser closed while the mission ran.
4. New browser process: Load Game, Progress listed "Iceland (Quick Save)" with its thumbnail. Loading it restored the exact position and view of the save.
5. Export from that session (473 KB), import into an empty namespace, reload: the store loaded it, and the game listed the same save.
6. Slots written by an earlier image format loaded as damaged, were copied aside, and the page showed the notice.

### Not yet covered

* P7.3 transition matrix, P7.4 mission packages and P7.5 beyond the run above, including checkpoints, death reload, restart and every other campaign transition.
* A real quota failure and storage eviction in a browser (covered only by injected Node failures), the `beforeunload` prompt, and the persistence grant.
* Safari and phones.
* More than one profile, which needs a larger profile volume. A larger volume costs Wasm heap, because E: is held in memory.
* Schema migration: version 1 is the only one. A future change must either read version 1 or keep reporting it as incompatible.
