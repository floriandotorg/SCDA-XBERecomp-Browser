# Required ISO loading contract

Status: active. Bounded File reads, an independent completion bridge, checked Wasm copies, authenticated image preparation and a build owned executable identity feed the integrated `story_boot` target. Its player shell selects the local ISO, executes genuine parent and child startup and owns complete disposable worker teardown. Trusted retained generation evidence and the initial story metadata policy are available. Actual startup now loads and unloads authenticated embedded resource sections and writes temporary files. Continuation is blocked on an approved raw cache device profile after a real Partition0 open is refused, not on another ISO or executable. Guest asset file streaming, a graphics checkpoint and full boot acceptance remain open. The detailed foundation sections below record earlier implementation slices; current status and execution evidence are in [M3](03-game-boots.md#authenticated-section-lifecycle-and-raw-cache-profile-gate).

Parent: [Roadmap](README.md). Implementation milestone: [M3: Game boot](03-game-boots.md).

## Player experience

The webpage and prebuilt Wasm contain the browser runtime, not the game's levels, textures, movies or audio. The player provides those from a local ISO.

Required flow:

1. Open the webpage.
2. See a Select ISO button and a short explanation that the file remains local.
3. Select an ISO through the browser's ordinary file picker.
4. Validate disc structure, locate `default.xbe`, verify the supported image identity and confirm required story data is present.
5. Start the game once validation completes. A separate explicit Start action is acceptable if needed for browser media permissions.
6. Read assets from the same selected ISO as the game requests them.

Selecting a file must not upload it. No extracted directory, companion local server, native installer or manual mount is required for the player.

A plain file input is the baseline. Do not require a browser specific persistent filesystem handle. After a page reload, the user may select the ISO again; previously saved progress remains independent of that selection.

## Ownership boundaries

* Browser session controller: owns the selected File, asset reader, disc index and runtime generation counter.
* Dedicated I/O worker: performs bounded file range reads and disc parsing as needed. This is not a Service Worker.
* Game runtime: owns guest memory, kernel handles, guest workers and graphics resources for one boot generation.
* Save storage: owns writable game files, settings, profile and launch transfer state in a namespace separate from the ISO.

A mission reboot replaces the game runtime but keeps the ISO reader available. A page reload destroys the selection unless an optional, later permission based persistence feature is explicitly added.

## Disc formats

Initial support should match the layouts already recognized in `src/kernel/disc.cpp`:

* Xbox filesystem starting at image offset zero, commonly called XISO.
* Full original Xbox disc images with the game partition at `0x18300000`.

Verify both signatures in the volume descriptor at partition sector 32. The file extension and MIME type are picker hints, not format validation.

Document accepted formats precisely. Other image formats, archives, encrypted containers, Xbox 360 images and compressed disc formats are unsupported unless deliberately implemented and tested.

## Range reader

Expose an implementation independent read operation equivalent to:

```text
readAt(offset, length, requestId, runtimeGeneration) -> bytes or explicit error
```

Requirements:

* Read only the requested File range. `File.slice` and a bounded ArrayBuffer read are a practical baseline.
* Do not read the full ISO with one `arrayBuffer()` call, preload it into MEMFS or duplicate it into IndexedDB or OPFS.
* Preserve large offsets above 4 GiB end to end. Avoid signed 32 bit bitwise coercion of file positions, sector arithmetic or request lengths.
* Validate offset, length, multiplication and addition against the selected file size before allocation or I/O.
* Bound request size, concurrent reads, staging buffers and cache size. Reuse buffers where ownership allows.
* Return correct short reads and end of file results through the guest file layer.
* Preserve correctness if requests finish out of order.
* Tag every request with its runtime generation. Discard old completions before they access a replacement runtime's memory or handles.
* A guest worker may wait only if the reader and completion service can continue independently.

The ISO File can be passed to a worker without eagerly copying its entire contents. Copying the bounded chunks required by Wasm is expected and should be measured.

## Filesystem index

Extract the portable Xbox filesystem parsing from the existing Windows backed disc reader. Retain known semantics but strengthen validation at the untrusted file boundary.

Required behavior:

* Case insensitive Xbox path lookup and directory enumeration.
* Binary directory tree parsing, subdirectory traversal and file extent lookup.
* Explicit checks for truncated tables, invalid names, out of bounds links, cyclic entries, recursive directory cycles and overlapping or invalid extents.
* Bounded recursion, total directory count, index bytes and per table allocation.
* Clear errors instead of interpreting malformed entries as a valid empty directory.
* Stable metadata used by the existing kernel file layer.
* Confinement of `..` and separator normalization to the disc namespace.

Use synthetic disc images with invented bytes for parser tests. Never commit copyrighted sectors as test fixtures.

`src/kernel/disc_index.cpp` now implements this parsing independently of platform handles. The Windows `Disc` adapter delegates to it. The index accepts a file size and synchronous bounded range callback, validates both supported partition signatures and publishes entries only after the complete mount succeeds. Synthetic browser tests now mount a File through the Wasm mailbox bridge and copy indexed subfiles into compact guest memory. This is not yet connected to kernel file handles or real startup callers.

Current ceilings are 32 directory levels below the root, 8,192 directories including the root, 131,072 entries, 16 MiB per directory table and a conservative 64 MiB cumulative index allocation budget. Physical requests are at most 1 MiB. `Load` is separately limited to 64 MiB per file; larger assets must use bounded subfile reads. ASCII case folding is deterministic and independent of host locale. Non ASCII name bytes are preserved, not Unicode case folded.

## Build and disc identity

The generated code must execute with the image it was translated from.

* Record the expected `default.xbe` hash in the Wasm build manifest.
* Read that file from the selected ISO and compare its hash before starting guest code.
* Reject an unsupported release with the expected and observed image identifiers and a concise explanation.
* Validate required story paths or a versioned asset manifest without demanding a full ISO hash scan at every launch.
* Read image sections, TLS data and other original executable bytes from the selected ISO when the runtime needs them.

`src/browser/iso-validation.mjs` now implements the identity comparison contract over an indexed `find` and bounded `read` source. It requires schema 1, `image: "default.xbe"`, a trusted lowercase `xbeSha256`, exact `xbeSize` and a nonempty `requiredStoryFiles` list of `{ path, minBytes }`. Missing identity blocks disc access. The inventory emits null identity and story fields until retained provenance is supplied; the selected disc must never populate them as its own authority. Synthetic fixtures exercise this contract in Wasm and Chromium, not real executable validation. See the evidence for budgets, cancellation, source ownership and parser limits.

`src/xbe/xbe.h` validates original image metadata without platform handles. It rejects invalid header, certificate, section and library spans, overlapping sections, invalid executable entry points, unbounded kernel import lists and invalid TLS templates or callback lists. A failed parse exposes an empty image. Parsing is not authentication.

`src/browser/iso-image.mjs` now passes only the validator's returned bytes into `src/browser/image-wasm.cpp` and `src/browser/image.cpp`. Preparation checks the base, image end and entry against generated metadata, loads headers and preloaded sections into fresh compact backing, clears section reference counts for unloaded sections and retains bounded import and TLS metadata. This preparation alone leaves import slots unresolved and callbacks unexecuted. A separate `BindImageImports` step can bind functions registered on the stopped CPU runtime and trusted kernel data addresses after validating the complete table. Missing exports reject binding without patching any slot. Its ledger records each slot, ordinal, kind and target. Production kernel exports and startup wiring are absent; the ISO harness still stops at preparation. `PrepareImageThread` can now prepare at most two isolated thread environments on that same bound stopped runtime. It allocates guarded compact stacks and guest thread blocks, copies the TLS template and publishes the image's negative TLS slot index only after preflight. This is cold bootstrap storage, not a thread scheduler or kernel handle implementation. TLS callbacks and guest entry remain unexecuted. This preparation is exercised with invented fixtures and a real selected local ISO under a development manifest; it does not supply missing generation provenance or establish a boot.

Different ISO packaging may contain the same supported executable. Validate filesystem structure and executable identity rather than requiring every byte of the entire disc to match one dump.

## Writable state

The ISO is always read only. Guest writes to cache drives, profile paths, settings, launch data and save slots go to a separate writable mapping.

A profile operation must not attempt to modify the chosen disc. Temporary state is acceptable during bring up if clearly labelled; M7 makes supported saves durable and exportable.

Progress is associated with the supported game build and a save schema, not with an unsafe filename alone. Never discard saves just because the same disc is selected under another filename.

## UI states and errors

Use explicit states:

```text
Awaiting ISO
    → Validating ISO
    → Ready to start
    → Booting
    → Running
    → Restarting mission
```

Canceled selection returns to Awaiting ISO without an error. Invalid selection offers Select another ISO. Read failure during play pauses or stops safely with retry or restart options; do not leave an infinite loading screen.

Distinguish at least:

* No supported Xbox filesystem found.
* Truncated or corrupt disc structure.
* Missing story executable or required assets.
* Executable version does not match the build.
* File could not be read or is no longer accessible.
* Browser capability is missing.

Replacing the ISO during play must first stop the runtime and invalidate outstanding requests. Never switch backing bytes underneath a running guest.

## Privacy and distribution

The local test image location is recorded in [GAMEFILES.md](../../GAMEFILES.md), which is ignored by Git. Keep machine specific game paths there, not in tracked documentation or test fixtures.

ISO contents, paths and hashes remain local unless a user explicitly exports a diagnostic containing selected metadata. No automatic upload, remote sector service, analytics attachment or full memory dump is allowed.

The local ISO workflow avoids bundling game assets. It does not establish that distributing translated game code is permitted. Public Wasm delivery remains a separate rights review gate in M8.

## Contract tests

* Valid synthetic XISO and full image layout probe.
* Signature mismatch, truncated descriptor, invalid directory tree and recursive cycle.
* Large sparse fixture or reader mock with offsets above 4 GiB.
* Case insensitive file lookup, end of file and bounded subfile reads.
* Two concurrent reads completing in reverse order.
* Stop or mission restart while reads are outstanding.
* Repeated menu to mission transitions without selecting the ISO again.
* Fresh page session: select ISO again, restore saved progress and boot normally.
* Asset network requests remain zero during real loading and traversal.
* Resident buffers remain bounded independently of ISO size.
