# M3 diagnostic boot acceptance and historical I/O evidence

Current integration: **M3 is accepted for diagnostic boot to the normal renderer's first device request.** The [current M3 continuation](03-game-boots.md#current-cache-and-startup-continuation) supersedes every historical slice status below. The integrated `story_boot` target authenticates retained executable provenance and matching compiled identity, publishes immutable session owned bindings, executes the genuine parent and child, delivers actual source bytes through guest handles, creates and reads the owned cache, and reaches `GRAPHICS_CHECKPOINT` from normal renderer initialization rather than a fatal display callback. It does not return device success, publish device outputs or claim rendered frames. Initial entry return alone was not sufficient.

## Current accepted evidence

M5 lifetime correction: an owned mutex is now retired after its final handle and operation pin disappear. Live aliases and blocked waits continue to retain it; ownership alone cannot retain an unreachable closed object. This supersedes the historical mutant-handle retention claims below and follows [Microsoft's documented mutex lifetime](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createmutexw). Running thread completion tokens still survive final handle closure until the thread completes. Native and Wasm ownership, duplication, object ABI and semaphore ABI checks cover the corrected boundary. See [M5 integration evidence](m5-evidence.md).

The frozen suite passes **248/248 tests**, including native ASan/UBSan and threaded Wasm. Final verification matches **231 manifest inputs** and all **134 retained generation outputs**. Source bytes, generated bodies and story metadata authority are unchanged. Three disposable Node generations assert the exact normal graphics caller and presentation metadata, positive guest asset delivery counters distinct from executable authentication, coherent silent audio and graphics reports, zero active guest workers and drained I/O staging.

The same packaged artifact runs in agent-browser Chromium with three boots of the same selected File, Stop after genuine execution progress, restart, empty selection, invalid ISO, unsupported executable size, missing shared memory and successful recovery. All **40 created workers retire**, with five peak live workers and none remaining. Empty selection is a DOM selection contract check rather than native picker automation. The browser reports no page or worker errors; network inspection contains local static GET requests only. Guest faults and unsupported executable hashes also have explicit regression coverage rather than being relabeled as a running game.

Diagnostic outcomes for the initial three browser boots take approximately **2.48, 2.40 and 2.41 seconds**, while independent UI callbacks continue. Production Wasm starts at **64 MiB** with a **256 MiB** maximum, observed compact backing is approximately **95.36 MiB**, and reader staging peaks at **1 MiB** with zero reader cache. An inclusive warmed browser process tree RSS sample peaks at approximately **2.02 GiB**, from **1.23 GiB** before the scenarios to **1.47 GiB** immediately afterward; this includes browser and daemon overhead and may double count shared mappings. It is not exclusive Wasm usage or a first frame measurement.

The timed silent sink is owned by the same session and driven by the serialized kernel clock, not a detached timer. It has a one second queue at 48,000 Hz and six float channels, with tested fractional timing, pause and concurrent clock publication. The actual checkpoint has zero submitted and consumed audio frames. No DSP initialization, movie skip, decoder completion, hardware register value, fence or GPU readback is fabricated. Movie playback is not reached before this device request; subsequent movie and rendering integration belongs to M4.

Reproduce the complete regression suite with repository local temporary files:

```sh
mkdir -p out/test-tmp
PATH="/opt/homebrew/opt/emscripten/bin:$PATH" TMPDIR="$PWD/out/test-tmp" \
  node --test --test-concurrency=4 tests/browser/*.test.mjs
```

Configure `BROWSER_BOOT=ON`, `BROWSER_STORY=OFF` and the trusted local `BROWSER_GENERATION_RECORD`, then build `story_boot` serially before selecting the real local ISO in its served player shell. Do not mix manifests and binaries or use a selected executable as build authority. Current local evidence is `out/provenance-audit/current-request-final-*`, including the exact reproduction harness `run-current-acceptance.mjs`, final suite, identity verification, three-generation results, browser scenarios, network inspection and qualified memory observations. Proprietary paths, identities, asset counts and bytes, detailed caller evidence and raw traces remain ignored. All sections below are historical foundation evidence.

Historical slice status: active. The bounded reader, independent completion bridge, checked Wasm adapter, disc index and manifest validator now connect to original XBE preparation with private compact backing. Synthetic contracts and a real selected ISO exercise this path in Wasm across three worker generations. P3.2 and P3.3 are not accepted for game integration. The developer picker and Stop controls are not the P3.1 player shell; P3.4 through P3.6 remain pending. A later native and Wasm contract slice adds checked import binding against registered functions and bounded kernel data, with no production export catalog or bootstrap wiring. A subsequent synthetic slice prepares isolated thread TLS and guarded bootstrap storage on the same bound stopped runtime. Production thread lifecycle and TLS callback integration are absent. Further P3.5 synthetic native and threaded Wasm slices add bounded events, semaphores, recursive owner tracked mutants with explicit abandonment and cancellation aware single object waits plus atomic wait any or wait all sets, without guest export, handle or production thread exit integration. A further synthetic slice adds notification and synchronization timers with an explicitly advanced, pausable virtual clock and no timer workers. A further synthetic native and threaded Wasm slice adds checked signed 100 ns guest wait deadlines, an explicit absolute time origin, indefinite waits and a distinct bounded host watchdog outcome. A further synthetic native and threaded Wasm slice snapshots guest timeout pointers and maps completed waits to indexed NTSTATUS values, retaining explicit faults for unsupported alertable waits, internal retirement and the independent host watchdog. A further synthetic native and threaded Wasm slice adds a bounded event and semaphore handle table whose reference pinned waits survive final handle closure. A further synthetic native and threaded Wasm slice checks guest stack frames and handle arrays for the three native handle wait ordinals, retaining snapshots through blocking and publishing stdcall results only after completion. A subsequent synthetic native and threaded Wasm slice adds notification and synchronization timer handles with shared schedules and operation pinned lifetime after final closure. Armed schedules alone do not retain closed timers. A subsequent synthetic native and threaded Wasm slice snapshots signed guest timer due time pointers and checks virtual relative or absolute 100 ns deadlines and millisecond periods before atomic schedule replacement. A subsequent synthetic native and threaded Wasm slice adds mutant handles with context identified recursive ownership retained beyond final handle closure until explicit owner abandonment. A subsequent synthetic native and threaded Wasm slice adds checked stdcall decoding for unnamed event and mutant creation, event set and clear, mutant release and handle close, with stable preflighted output spans across nonblocking mutations. A subsequent synthetic native and threaded Wasm slice extends that checked object ABI to unnamed semaphore creation and release, preserving signed counts, optional previous count outputs and preflight before mutation. A subsequent synthetic native and threaded Wasm slice adds checked unnamed timer creation, APC free scheduling and cancellation, with single byte signal state outputs distinct from internal schedule membership. A subsequent synthetic native and threaded Wasm slice adds checked handle duplication and atomic close source transfer with stable output preflight, shared state and retained pending waits. A subsequent synthetic native and threaded Wasm slice adds checked event, semaphore and mutant information queries with coherent reference pinned snapshots, without consuming signals or acknowledging abandonment. The production clock driver, production handles and production wait export registration remain absent. Trusted generation provenance and an accepted startup story manifest are still absent. No game entry point has executed.

Parent: [M3 game boot](03-game-boots.md). Contract: [ISO loading](iso-loading.md).

## Scope and order

The first slice implements the P3.2 dependency before the player shell. It establishes independent local completion. A second slice extracts and hardens the P3.3 disc index independently of Windows I/O. A third slice mounts that index through independent File reads in Wasm and copies bounded subfiles into compact guest memory. A fourth slice authenticates bounded executable bytes and checks story file metadata against an injected trusted manifest. A fifth slice hardens the shared original XBE parser using invented native and Wasm fixtures. It does not connect authenticated bytes to a runtime loader. These slices do not establish the missing generation provenance or boot. The existing M1 page and M2 Wasm targets are unchanged. The ISO page is a synthetic contract harness, not Select ISO for players.

The first five slices below used synthetic data only. The later [real ISO preparation slice](#authenticated-image-preparation-and-real-iso-slice) uses the test image referenced in ignored [GAMEFILES.md](../../GAMEFILES.md). The trusted generation XBE hash remains absent. Supported build identity must come from retained generation provenance, not from trusting whichever executable a player selects.

## Implementation

* `src/browser/iso-reader.mjs`: File byte ranges, checked safe integer positions, explicit errors, FIFO admission and resource counters.
* `src/browser/iso-mailbox.mjs`: fixed shared staging slots, monotonically increasing request tags, bounded synchronous guest waits and completion publication.
* `src/browser/iso-io-service.mjs`: mounted File lifetime, runtime generations, two guest MessagePort connections and independent completion.
* `src/browser/iso-io-worker.mjs`: dedicated browser I/O worker entry point.

The main controller mounts a File once. A runtime receives a private MessagePort and fixed SharedArrayBuffer mailbox. The guest posts its range directly to I/O, then uses `Atomics.wait` on its mailbox. I/O performs `File.slice(offset, offset + length).arrayBuffer()`, checks the runtime and request tags, copies into shared staging and notifies the waiter. No UI message callback is required to finish a guest read.

The mailbox is not guest memory. `readIsoRange` returns a shared staging view that remains valid only until the next request on that mailbox. The Wasm adapter now validates its destination span, checks shared generation ownership before and after waiting, reacquires the current heap view and copies only a complete requested range. The separate C++ guest adapter validates compact guest destinations before I/O and again during the final write, with no backing lease held across the wait. See the Wasm integration slice below.

### Control protocol

The owning controller sends these messages to the dedicated worker. Each reply echoes the optional `id` and contains either `ok: true` with `stats`, or `ok: false` with a bounded code and message. File names and original exception messages are not reported.

```text
mount       { file, generation }
generation  { generation }
connect     { port, mailbox, generation }
stats       {}
close       {}
```

Transfer the MessagePort in `connect`; share, rather than transfer, the mailbox. The guest calls `readIsoRange(port, mailbox, offset, length, requestId, generation, timeoutMs)` from a blocking worker only. Calls on one mailbox are sequential. Request identifiers increase within a generation. Generations increase within the signed 32 bit range and disconnect old guest ports before replacement workers attach. A new generation retains the mounted File. Replacing the File requires closing and replacing the I/O service.

A timeout invalidates a pending slot and posts cancellation. If a writer has already acquired the slot, the waiter can time out but cannot reuse that slot until publication finishes. Old tags cannot publish into a newer request. An unavailable service cannot make a guest wait beyond its configured deadline.

### Budgets and accounting

* Maximum individual range: 1 MiB.
* Maximum active reads: two.
* Maximum admitted reader jobs, including queued jobs and cancelled active reads: sixteen.
* Active read reservation ceiling: 4 MiB. With the current concurrency and request ceilings, active reservations reach at most 2 MiB.
* Two attached shared mailboxes, each at most 1 MiB plus a twenty byte header.
* Cache bytes: zero. No persistent storage, MEMFS, asset fetch or upload path exists.
* Wait deadline: one through 30,000 milliseconds; default 5,000 milliseconds.

`stagingBytes` counts active read reservations, not total JavaScript heap or browser Blob storage. Promise results awaiting their completion copy can coexist briefly with subsequent reads. With two sequential guest ports, at most two completed range buffers await handoff alongside two active reads and two fixed mailboxes. This is bounded independently of disc size. The standalone asynchronous reader requires its consumer to release returned buffers; it cannot bound buffers a caller deliberately retains.

Cancellation rejects active and queued promises immediately. Browser Blob reads cannot be aborted through this API, so cancelled active reads retain their reservations until the underlying promise settles. A later generation cannot evade the concurrency or memory ceiling by repeatedly cancelling. The controller watchdog and recovery from permanently stalled browser I/O remain P3.1 work.

Short physical reads are errors, not successful zero filled reads. The later guest file layer must clamp subfile ranges at end of file before requesting physical ISO bytes.

## Tests and reproduction

Write tests first, observe missing module failures, then implement. A later request tag regression test also failed with `TIMEOUT` instead of the expected `REQUEST` before enforcing monotonic mailbox tags.

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
node --test tests/browser/iso-reader.test.mjs tests/browser/iso-bridge.test.mjs
node --test tests/browser/*.test.mjs
node tools/browser/prepare-iso-tests.mjs
node tools/browser/serve.mjs out/iso-tests 8091
```

Open `/tests/browser/iso.html` under that isolated server using agent-browser. Wait for `globalThis.isoResults`, inspect the result, browser errors and network requests. The preparation script copies only the listed runtime modules and synthetic harness into the ignored output directory. No Wasm build or game assets are required by this page.

Observed on Apple Silicon macOS, Node `v26.5.0`, agent-browser Chromium `151.0.0.0`:

* All fourteen new tests pass. The complete existing suite passes all thirty three tests, including native and Wasm CPU contracts with complete retained metadata.
* Large positions use a synthetic six GiB file interface. A read above four GiB preserves both endpoints and allocates twelve result bytes. This is not an operating system sparse File or real multi GB browser resident memory measurement.
* Reader tests cover reversed completions, overflow and invalid ranges, queue saturation, duplicate requests, staging pressure, short reads, access failure, cancellation and retained reservations across generation replacement.
* Worker tests block an independent guest in `Atomics.wait`, complete through I/O, and wake it on read failure, short read and cancellation. A deliberately late completion cannot change a replacement slot or its data. Close wakes pending slots; excess guest connections are rejected.
* The browser reads 50,331,648 verified bytes through two guest workers per generation across three generations, reusing one synthetic 1 MiB File. Each generation terminates its guest workers and closes old ports before replacement.
* The final browser run records 57 UI timer callbacks during guest work, two peak active reads and 131,072 peak active reservation bytes. All three generation checkpoints have zero active reads, pending jobs and reserved bytes. Cache bytes remain zero.
* Browser error inspection is empty. Observed network requests contain only local HTML, JavaScript and a rejected favicon request. No ISO upload or asset request appears. This is synthetic I/O evidence, not a real game network capture.

Ignored local evidence: `out/iso-tests/all-tests.log`, `out/iso-tests/browser-results.json` and the staged browser harness. M2 compilation outputs remain in their existing ignored directories.

## Portable disc index slice

`src/kernel/disc_index.h` and `src/kernel/disc_index.cpp` extract directory parsing, path lookup, enumeration and subfile reads behind a file size and synchronous range callback. `src/kernel/disc.cpp` retains the Windows handle and overlapped I/O adapter, obtains the physical size with `GetFileSizeEx` and delegates to the portable index. `Disc::Entry` remains an alias with the existing kernel metadata shape. The host CMake source glob includes the new implementation automatically.

The parser checks both volume signatures at partition zero and `0x18300000`. All extents are bounded against the physical size before I/O or allocation. An ordered interval index rejects overlapping files, directories, descriptor data and recursive directory aliases. Reachable table links must address complete, nonoverlapping records; cycles and repeated links are errors rather than silently skipped entries. Empty tables must have zero length or contain only `0xff`. Invalid names and duplicate case insensitive names are rejected. Mount failure clears the previous index and never exposes partial entries.

Lookup normalizes slash and backslash separators, dot components and parent traversal confined to the disc root. Traversal through a file is rejected. Entries are sorted using deterministic ASCII case folding. Subfile reads clamp at EOF and preserve positions above four GiB. Directory reads and file loads split physical requests at one MiB. Short physical reads fail rather than fabricating bytes. This retains the native count based API; richer guest completion and error semantics remain P3.4 work.

Budgets are enforced before allocation: 16 MiB per table, 32 directory levels below root, 8,192 total directories, 131,072 entries and 64 MiB of conservative cumulative index charges. Charges include both table scratch buffers, entry vector capacity, names and interval bookkeeping. They are not an observed process RSS measurement. Whole file `Load` has a separate 64 MiB ceiling. These limits can be lowered, not raised, through `DiscLimits`.

### Parser reproduction and observations

```sh
node --test tests/browser/disc-index.test.mjs
node --test tests/browser/*.test.mjs
```

The tests were written first. Both platforms initially failed because `src/kernel/disc_index.cpp` did not exist. Both now compile and execute the same synthetic C++ fixtures. Native execution enables AddressSanitizer and UndefinedBehaviorSanitizer; Wasm executes under Node without Emscripten filesystem support. No website change or browser parser execution is claimed.

* Both native and Wasm contract tests pass. The complete suite now passes all 35 tests.
* Sparse six GiB reader fixtures cover both supported partitions, repeated path lookup, sorted enumeration, EOF and subfile reads above four GiB. A valid small mount and read use fewer than 8,192 physical bytes, not a full image allocation.
* Corrupt cases cover either descriptor signature, truncated descriptors, short reads, invalid extents, directory and file overlaps, recursive aliases, repeated or cyclic tree links, embedded records, invalid names and duplicate case insensitive entries.
* Reduced allocation, depth, directory and entry budgets reject mounts. A deeply nested fixture reaches the default recursion ceiling. A file larger than one MiB loads through multiple bounded requests; an exceeded load budget and read failure return no loaded data.
* A deterministic 1,000 mutation sweep runs on each platform and checks failure isolation, accepted extent bounds and the physical request ceiling. It is regression coverage, not a claim of exhaustive fuzzing.
* Empty tables, empty files and failed remounts are covered. Native sanitizer runs report no failures.

Ignored evidence is in `out/disc-index-tests/`, including `all-tests.log` and per platform results. The Windows adapter was not compiled or run on this macOS host. No real ISO was provided. The later Wasm integration slice mounts an invented browser File through this index. Trusted executable identity, the real required story file list and original image loading remain absent. The identity comparison contract is implemented in the later validation slice.

## Wasm integration slice

`src/browser/iso-wasm.mjs` binds one port and mailbox to one Wasm worker and a shared generation ownership cell. It validates Wasm heap destinations before requesting bytes, preserves numeric offsets above four GiB, blocks only that worker and checks ownership again before copying. It reacquires the current heap after waiting rather than retaining a view across memory growth. Short completions and explicit I/O errors leave the destination unchanged. Request tags remain monotonic for the lifetime of the binding.

`src/browser/iso-disc.cpp` adds `ReadDiscToGuest` for the portable index and M2 compact memory. It limits each guest read to one MiB, clamps at subfile EOF and rejects calls made under a backing lease. `Memory::CheckRange` validates the entire destination without exporting a host pointer. The adapter releases that check before synchronous I/O, uses a bounded temporary buffer, checks runtime cancellation after I/O and relies on `Memory::Write` to revalidate every destination page under the memory lock. A failed read does not publish partially filled bytes into guest memory. This is a data path, not kernel file handle or pending operation semantics.

The temporary C++ guest buffer adds at most one MiB per active call to the existing mailbox and reader budgets. Parser scratch allocations remain subject to the index limits. No disc cache or full image import was added. The controller must invalidate ownership and stop old workers before attaching replacement workers; the shared cell is not a substitute for runtime shutdown. The fixture creates one Wasm instance per generation and terminates it before starting the next.

### Reproduction and observations

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
node --test tests/browser/iso-wasm.test.mjs
node --test tests/browser/*.test.mjs
node tools/browser/prepare-iso-wasm-tests.mjs
node tools/browser/serve.mjs out/iso-wasm-tests 8092
```

Open `/tests/browser/iso-wasm.html` with agent-browser under this isolated server and wait for `globalThis.isoWasmResults`. The preparation script builds a synthetic Wasm contract module using the real index, compact memory and guest copy adapter. It stages only the listed modules and invented fixture. The seven bytes named `default.xbe` are not a valid executable and never reach guest entry.

* Tests were added before implementation. JavaScript initially failed with `ERR_MODULE_NOT_FOUND` for the Wasm adapter; the native fixture failed with the missing `browser/iso-disc.h` header.
* All five new tests pass. The complete browser test suite passes all forty tests. Native guest adapter coverage runs with AddressSanitizer and UndefinedBehaviorSanitizer.
* JavaScript tests cover invalid destinations before I/O, unchanged adjacent bytes, heap replacement during the wait, stale ownership, short completions, cancellation and explicit read failure without copying into the destination.
* Native tests cover destination wrap and uncommitted pages, reads under a lease, the request ceiling, EOF clamping, physical failure, decommit during I/O and runtime cancellation before publication. The reader asserts that no guest backing lease is held.
* The Node integration runs the actual Wasm parser and guest copy path against a sparse six GiB reader interface. Each generation mounts through low offset descriptor reads and completes two subfile reads above four GiB, preserving offsets through C++, JavaScript and the independent service. This is not an operating system sparse File or resident memory measurement.
* Chromium mounts one invented 90,112 byte local File through a dedicated I/O worker across three Wasm worker generations. It reads 2,085 physical bytes per generation, 6,255 total, including descriptor, table and two EOF checked subfile reads. Invalid guest destinations and stale ownership are rejected.
* The final browser run records twenty one UI timer callbacks, one peak active read and 2,048 peak reader reservation bytes. Every generation ends with zero active reads, pending jobs and reserved bytes. Cache bytes remain zero. These counters exclude Wasm heap size and parser allocations; no resident memory bound is inferred from this small fixture.
* Browser error inspection is empty. Observed network requests contain only the local harness modules and a rejected favicon request. Worker module and Wasm loading remain runtime code delivery, not game asset traffic. No ISO upload is performed. This is synthetic evidence, not a real game network capture.

Ignored evidence: `out/iso-wasm-tests/all-tests.log`, `browser-results.json`, `browser-network.txt` and the staged Wasm harness. No real game code executes in this fixture, and the M1 diagnostic page and M2 build targets are unchanged.

## Manifest driven identity validation slice

`src/browser/iso-validation.mjs` implements `validateDiscImage({ disc, manifest, signal })`. The source supplies `find(path)` returning `{ size, dir }` or no entry, and `read(path, position, length)` returning a `Uint8Array`. Both operations may be synchronous or asynchronous. Paths use the mounted index's normalized case insensitive namespace. Source exceptions become bounded read errors without exposing host file paths.

The validator snapshots schema 1 manifest fields before the first disc operation:

* `image` must be `default.xbe`.
* `xbeSha256` must be a lowercase SHA256 digest from trusted generation provenance.
* `xbeSize` must be the exact expected executable length, at most 64 MiB.
* `requiredStoryFiles` must contain one through 256 `{ path, minBytes }` entries. Paths are relative ASCII names, at most 240 bytes, with case insensitive duplicate checks. Parent traversal, empty components, drive names and executable dependencies are rejected. No Versus image is required.

Absent or malformed identity blocks all disc access. The build inventory now emits explicit null size and story fields alongside the absent hash. It does not infer trusted identity from a selected File, an extracted executable or a successful synthetic test. Connecting retained generation provenance to the build remains a separate gate.

Image size and required story file metadata are checked before image allocation. Only the executable is read, in chunks at most one MiB, into one JavaScript buffer capped at 64 MiB. Web Crypto hashes that buffer locally. A mismatch returns expected and observed hashes through a local error, never an upload. Story checks verify indexed existence, regular file type and minimum length, not content integrity or successful decoding. There is no full story scan or disc preload.

The result contains the authenticated original bytes for the bounded XBE parser described below and a future runtime loader. Connecting these components remains pending. It does not parse sections, load TLS, initialize memory or execute an entry point. Callers must use those returned bytes rather than rereading an unverified executable, serialize validation per session, and abort the validation signal before retiring its runtime generation. Cancellation is checked before and after every asynchronous source operation and after hashing, so cancelled results cannot be published. Blob reads and Web Crypto hashing themselves cannot be interrupted by this signal; the future session controller still owns worker termination.

The 64 MiB image buffer is separate from Wasm and the bounded I/O staging. Web Crypto may make its own full input copy and retain internal hashing state. No total resident memory ceiling is inferred from these logical limits. Alternatives were a streaming hash implementation or a full ISO hash. Web Crypto avoids another cryptographic implementation; the fixed executable ceiling keeps its one shot input bounded. Streaming can replace hashing if real executable sizes or measurements justify it, without changing disc reads.

### Validation reproduction and observations

```sh
node --test tests/browser/iso-validation.test.mjs
node --test tests/browser/*.test.mjs
node tools/browser/prepare-iso-wasm-tests.mjs
node tools/browser/serve.mjs out/iso-wasm-tests 8093
```

Open `/tests/browser/iso-wasm.html` with agent-browser and wait for `globalThis.isoWasmResults`. The fixture now includes an invented three byte `story.dat`. Its seven bytes named `default.xbe` remain opaque invented data, not a valid XBE. The digest and required file list in the test worker are fixture constants only. Test only Wasm exports adapt the actual mounted `DiscIndex` to the validator; no production picker or runtime bootstrap is added.

* Tests were written first and failed with `ERR_MODULE_NOT_FOUND` for the validator. The mounted integration then failed because its validation result was absent. A later sparse manifest regression failed with `READ_FAILED` before manifest iteration was hardened.
* Nine new validator tests pass. The complete suite passes all 49 tests, including native sanitizer contracts, Wasm contracts and the complete retained CPU metadata target.
* Tests cover bounded chunk boundaries, missing provenance before I/O, oversized images, malformed story paths, duplicate names, missing or directory entries, wrong executable sizes and hashes, short reads, sanitized access failures, manifest mutation and cancellation before I/O, during a read and during hashing.
* The Node mounted integration authenticates fixture bytes and rejects a mismatched expected digest across three generations. Each generation performs four high offset subfile reads through the independent service, twelve total, preserving positions above four GiB. This uses the sparse six GiB reader interface, not a real large File.
* Chromium authenticates and rejects the mismatch across three worker generations using one 90,112 byte synthetic local File. It reads 2,131 physical bytes per generation, 6,393 total. All generations end with zero active reads, pending jobs and reserved staging bytes. Peak reader reservation is 2,048 bytes, peak concurrency one and cache bytes zero. The UI records 29 timer callbacks.
* Browser error inspection is empty. Observed network traffic contains only local harness code and a rejected favicon request. No ISO upload or game asset request appears in this synthetic run. Worker module and Wasm delivery are runtime code traffic, not assets.

Ignored evidence: `out/iso-wasm-tests/identity-all-tests.log`, `identity-browser-results.json`, `identity-browser-network.txt` and `identity-server.log`. The browser and this slice's local server were closed after inspection. No real ISO or proprietary XBE was read.

## Bounded original XBE parser slice

`src/xbe/xbe.h` remains the shared parser used by the native boot path and recompiler. It now validates complete spans before reading metadata or allocating section and library vectors. Integer range checks use widened arithmetic. Scalar reads decode little endian bytes explicitly and reject out of file positions; virtual scalar reads cannot cross a mapping into unrelated file bytes. File offsets exclude uninitialized section tails. Strings have explicit termination bounds.

Parsing builds a private candidate and publishes it only after all checks pass. Every attempt clears prior image state, including debug key selection, sections, libraries and TLS. Failure returns an empty image and a bounded diagnostic; success clears an earlier diagnostic.

Current parser ceilings:

* Original file: 64 MiB, matching the identity validator.
* Header: 16 MiB; virtual image: 256 MiB, without wrapping the 32 bit address space.
* Sections: 1 through 1,024; libraries: at most 256; section names: at most 255 bytes.
* Kernel thunk scan: at most 4,096 slots including the terminator, with ordinal imports only.
* TLS initialization plus zero fill: at most 1 MiB; callbacks: at most 256 plus a terminator.
* Stack commit: at most 16 MiB; heap reserve: at most 256 MiB, with commit no larger than reserve.

Certificate, section and library metadata must fit the declared headers. Raw section bytes must fit the file and virtual section span. Sections cannot overlap each other or the headers in either namespace; shared allocation pages without byte overlap are not rejected. Entry points and TLS callbacks must name file backed executable section bytes. Kernel thunk and callback scans require mapped original bytes and bounded termination. TLS descriptors must be complete, templates must have ordered and backed extents, and the TLS index must fit a writable section, including its zero filled tail. Optional TLS and library tables remain supported.

These are conservative supported parser limits, not evidence that a real supported release fits every constraint. No proprietary XBE was available. Authentication is still a separate prerequisite: `Image::Parse` does not hash its input. Browser startup must feed the identity validator's returned bytes to the parser and then explicitly load sections, initialize TLS and resolve exports in compact memory. None of that runtime wiring is added here. The shared native parser changes were exercised on macOS, not through the Windows boot adapter or the recompiler executable.

### Parser reproduction and observations

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
node --test tests/browser/xbe.test.mjs
node --test tests/browser/*.test.mjs
```

* Tests were written first. The native fixture initially failed with `corrupt XBE accepted` for a wrapping section size; Wasm also rejected the test execution before the fix.
* Both new contract tests pass. The complete suite passes all 51 tests. Native parsing enables AddressSanitizer and UndefinedBehaviorSanitizer; Wasm runs under Node with Emscripten filesystem support disabled. This slice did not change or test a webpage.
* Invented retail and debug images check independent entry, import, title, section, library and TLS values, repeated successful parses, optional metadata, failed reparses and zero filled index storage.
* Corrupt cases cover wrapping addresses, truncated metadata, oversized counts, raw and virtual overlap, invalid entry and import addresses, unbounded names and tables, invalid ordinals, TLS template reversal, inaccessible callbacks and allocation budget overflow.
* All 1,792 truncation lengths below the complete fixture are rejected. A deterministic 2,000 mutation sweep checks rejection isolation and accepted extent bounds on each platform. This is regression coverage, not exhaustive fuzzing.
* Boundary fixtures accept 4,095 imports followed by a terminator and 256 callbacks followed by a terminator, and reject one additional nonzero slot. No sanitizer failures were observed.

Ignored evidence: `out/xbe-tests/tests.log`, `all-tests.log`, per platform results and compiled fixtures. No ISO, proprietary executable, guest memory loader or real entry point was used.

## Authenticated image preparation and real ISO slice

`src/browser/iso-image.mjs` connects `validateDiscImage` to the Wasm image adapter. It snapshots and bounds the generated base, image end and entry before asynchronous reads. Validation failure or cancellation cannot allocate image backing. Only the authenticated returned executable buffer is copied into the current Wasm heap; the disc is not reread. Failed allocation, heap copying or preparation resets unpublished image state.

`src/browser/image.cpp` prepares an isolated `PreparedImage`. It parses those original bytes, rejects a layout different from the retained generated metadata and creates a fresh `Memory` only after metadata checks. The image span is rounded to pages and remains subject to the existing 112 MiB compact backing ceiling, including the shared physical windows. Headers and preloaded sections are copied; section tails, image gaps and unloaded sections remain zero. Guest section reference counts are explicitly one for preloaded sections and zero otherwise. Sections sharing an allocation page retain separate byte extents.

The loader collects at most 4,095 `{ slot, ordinal }` imports without modifying their original slots. It snapshots initialized TLS bytes plus zero fill and records the bounded callback list. Entry, import slots and required TLS metadata must be in preloaded sections; preparation rejects unsupported deferred startup metadata rather than reading uninitialized guest bytes. These are conservative loader constraints, not universal XBE support. The TLS index is not assigned, no per thread block is created, imports are not resolved and callbacks do not run. No `Runtime::Invoke` or guest entry call exists in this slice.

`src/browser/image-wasm.cpp` owns one unpublished or prepared image per Wasm instance. Allocation and reset retire prior image state; parser and layout failure publish no image. The adapter is not an authentication authority. Production startup must use the validation wrapper, a trusted build manifest and a fresh stopped instance. It must not expose these low level exports as a bypass around identity checks.

The explicit ISO contract build includes these sources. The native host source glob now excludes `src/browser/`, keeping Emscripten adapters and diagnostic entry points out of the Windows target. No Windows compilation or production boot target is claimed. M1 and M2 runtime source lists remain separate.

### Synthetic reproduction

```sh
node --test tests/browser/image.test.mjs tests/browser/iso-image.test.mjs
node --test tests/browser/iso-wasm.test.mjs
node --test tests/browser/*.test.mjs
node tools/browser/prepare-iso-wasm-tests.mjs
node tools/browser/serve.mjs out/iso-wasm-tests 8094
```

* The native and Wasm loader tests initially failed because `src/browser/image.cpp` was missing. The JavaScript bridge tests initially failed with `ERR_MODULE_NOT_FOUND` for `iso-image.mjs`.
* All 57 tests pass. Native image preparation enables AddressSanitizer and UndefinedBehaviorSanitizer; Wasm contracts run without Emscripten filesystem support.
* Loader fixtures cover original header and section bytes, zero tails, shared pages, unloaded sections and reference counts, untouched import slots, TLS template and callback snapshots, missing TLS, empty TLS, repeated isolated images, layout mismatches, unloaded startup metadata and backing budget rejection.
* Bridge tests check that no Wasm image operation occurs before successful authentication, that generated layout is snapshotted, that the heap is reacquired after allocation and that cancellation, failed copying and parser rejection cannot publish an image.
* Mounted Wasm integration uses an invented valid XBE across three generations: prepare, reject an expected digest mismatch, then prepare again. Its original bytes pass through independent I/O, hashing, the actual parser and compact backing. Every generation ends without pending reads or reserved staging.

### Real local ISO observations

The location is recorded only in [GAMEFILES.md](../../GAMEFILES.md). Do not copy its absolute path or copyrighted bytes into tracked files. Local executable bytes, hashes, full disc listings, manifests and browser results remain under ignored `out/m3-real/`.

Open `/tests/browser/iso-local.html` with agent-browser. The page explicitly identifies itself as a developer preparation contract, not a player boot shell. Choose a local expected manifest and the ISO using its ordinary file inputs. The manifest contains the validator's schema 1 fields plus generated `ranges.X86ImageBase`, `ranges.X86ImageEnd` and `ranges.X86EntryPoint`. The harness retains one selected File and one independent I/O worker across three disposable Wasm workers. Stop terminates both worker contexts; a 30 second controller watchdog bounds a stalled worker response.

For this local smoke test only, the expected digest and size were taken from the already retained `assets/default.xbe`, independently of the selected ISO. The disc executable matched those bytes. The layout came from the retained generated mapping. Two observed story paths were checked for existence and nonzero size. These observations do not prove that the retained executable produced the generated code or establish a complete startup story manifest. The production inventory deliberately keeps its trusted identity and story fields null; guest entry remains blocked.

Observed on the same Apple Silicon host with pinned Emscripten and agent-browser Chromium:

* A real 7,825,162,240 byte File mounts at the supported full image partition `0x18300000`. Native sanitizer parsing and preparation also pass with these original bytes.
* Each browser generation reads 6,463,488 physical bytes through bounded ranges, including index and executable reads; three generations read 19,390,464 bytes total. The largest request and peak reader reservation are both 1 MiB. Peak concurrency is one; cache bytes remain zero.
* Each prepared image contains 34 sections, 157 import records and a 144 byte TLS template. Compact backing, including the shared physical windows and commitment maps, is 74,213,058 bytes. This is image preparation, not thread initialization or executed startup.
* Each Wasm worker observes 161,087,488 bytes of linear memory. This harness also retains its earlier compact guest copy fixture, so that size includes a second physical backing allocation. No total browser RSS measurement or independent resident memory ceiling is inferred from these counters.
* All three generations finish with zero active reads, pending requests and reserved reader bytes. The controller records 56 UI timer callbacks during the run. Workers terminate before replacement; the selected File is reused without another picker operation.
* An incorrect expected digest produces `The executable version does not match this build.` Stop during another run produces a distinct cancelled `Stopped.` result. Browser error inspection is empty. Observed network requests contain local harness code and a rejected favicon request, with no ISO upload or game asset request.

This supplies real File and image preparation evidence, not real asset decoding, guest filesystem completion semantics, full process memory measurement or a game boot. The harness and development manifest must not become a player supplied trust override.

## Checked image import binding slice

`BindImageImports` in `src/browser/image.cpp` resolves a prepared image's import ledger against the same stopped `browser::Runtime` that will own execution. `Runtime::KernelTarget` returns a thunk address only for a registered nonzero ordinal in the existing 400 slot dispatch range. An unavailable ordinal is a bounded `Fault` carrying that ordinal, not a fabricated callable target. A trusted caller may additionally supply at most 399 `{ ordinal, address, size }` kernel data exports. This catalog is runtime configuration, never disc supplied metadata.

Binding has separate preflight and publication phases. A backing lease holds the image stable while checking all data spans, original import slots and the zero terminator. Data exports must have unique supported ordinals, nonzero committed contiguous storage, no function registration at the same ordinal and no backing overlap with the import table. Backing comparisons also reject aliasing through the two physical windows. This slice conservatively requires the import table and each data export to fit contiguous host backing; it does not allocate, initialize or invent kernel data values.

Only a complete preflight patches the slots and publishes `boundImports`, with `{ slot, ordinal, kind, target }` for every import. Original authenticated XBE bytes and the terminator remain unchanged. Missing or ambiguous exports, modified slots, uncommitted backing and a cancelled runtime leave all slots untouched and publish no ledger. Binding is permitted once, including an empty table. Repeated imports retain separate ledger entries with the same target.

The bootstrap must serialize binding with runtime launch and catalog changes, keep the prepared memory paired with this runtime and finish before any guest worker executes. The active worker check is a guard, not a concurrent launch barrier. The backing lease prevents memory transitions and holds no I/O wait. A failed preflight can be retried once its missing dependency is supplied; a successfully bound image cannot be rebound to another runtime.

### Reproduction and observations

```sh
node --test tests/browser/image.test.mjs
node --test tests/browser/*.test.mjs
```

* Tests were extended first. Native and Wasm initially failed compilation with missing `importsBound`, `boundImports` and `KernelTarget` members before implementation.
* Both image contracts pass. The full suite passes all 57 tests, including native AddressSanitizer and UndefinedBehaviorSanitizer coverage and Wasm execution under Node with Emscripten filesystem support disabled.
* Invented images bind mixed function and data exports, invoke a synthetic registered host service through the patched thunk and read the actual mapped data through its patched address. This is not a generated game function or real kernel service.
* Tests cover unavailable final imports and bounded ordinal diagnostics, retry after registration, duplicate and ambiguous data exports, null, empty, wrapping and uncommitted data ranges, direct and physical alias overlap with the import table, modified original slots and terminators, cancelled runtimes, repeated ordinals, empty tables and rejected rebinding.
* A 4,095 import fixture reaches the parser's accepted ceiling. Success retains every ledger entry. An unavailable final ordinal or a decommitted final page leaves the first slot unchanged and publishes no ledger, demonstrating complete preflight rather than incremental patching.
* No webpage changed, no browser run was performed for this slice and no real ISO or proprietary executable was read. Existing ISO harnesses still stop at image preparation. No new low level Wasm binding export or identity bypass was added.

Ignored evidence: `out/image-tests/import-tests.log`, `import-all-tests.log` and per platform result files. Production kernel services, initialized kernel data, thread TLS, trusted generation identity and real boot remain separate gates. This slice does not accept P3.3 or P3.5.

The choice is to reuse the runtime's existing kernel thunk namespace rather than create host thunks for missing services or patch unknown ordinals optimistically. Keeping binding separate from preparation preserves the authentication and layout gates while letting startup fail before guest execution. The reversal cost is one binding function and its ledger; the disc reader and original parser are unchanged.

## Isolated thread TLS and bootstrap storage slice

`PrepareImageThread` in `src/browser/image.cpp` prepares cold thread state only after import binding. Binding now retains the runtime identity. A different runtime, cancellation, active guest workers, duplicate or unaligned thread IDs and a third thread reject preparation. This retains the CPU runtime's two worker ceiling rather than implying a production scheduler. The caller must serialize preparation with launch, memory transitions, catalog changes and shutdown; the active worker check is not a concurrent launch barrier.

Each descriptor records an initial `X86Context`, KPCR, ETHREAD, stack bounds and TLS addresses. Storage belongs to the prepared image until that image is destroyed. Two fixed, disjoint 32 MiB guest address slots begin at `0xD0000000`. Actual backing covers only the requested stack, TLS and three bookkeeping or guard pages. Existing mappings reject allocation rather than being reused or overwritten. The first page and the page immediately below the stack remain uncommitted. KPCR and ETHREAD occupy separate byte extents in one committed system page. These are checked guest guards, not host operating system guard mappings.

Stack requests clamp to at least 64 KiB and reject values over 16 MiB. The bounded TLS template is rounded to sixteen bytes, with one additional pointer slot before the data. The image TLS index is a negative dword offset from `NtTib.StackBase` to that slot. Its value is the same across thread stacks. Each slot points to its private template copy, including zero fill; rounded padding starts zero. With no TLS descriptor, no slot or image index write occurs. An empty TLS descriptor still receives its pointer slot. KPCR and ETHREAD fields follow the existing native thread block layout. The initial context has a sixteen byte aligned stack below TLS, its FS base at KPCR, masked x87 and SSE defaults and the bound runtime as host. No return address, guest entry, kernel handle, suspend state or scheduler registration is invented.

The TLS index must remain committed and cannot overlap the import table. Later preparation rejects a modified published index. Allocation or preflight failure leaves the index and thread ledger unchanged. A pending mapping guard retires unpublished backing if preparation fails after allocation. Publication holds a backing lease, with no I/O or wait. The existing 112 MiB total compact backing ceiling still applies. The two descriptors do not imply two additional Wasm workers, and no storage reuse after thread exit is provided.

This host initialization does not execute or substitute for the game's XAPI startup or its TLS callbacks. Original template bytes and the callback snapshot remain unchanged. Production startup must establish callback ownership and ordering before this helper is used to launch game threads. The existing ISO harness and its low level Wasm exports are unchanged and still stop at image preparation.

### Reproduction and observations

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
node --test tests/browser/image.test.mjs
node --test tests/browser/*.test.mjs
```

* Tests were extended first. Both platforms initially failed compilation because `PrepareImageThread` and the thread ledger did not exist.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and Wasm contracts pass. The full suite passes all 57 tests with Emscripten filesystem support disabled for the image contract.
* Invented images verify KPCR and ETHREAD fields, guarded pages, context defaults, independent TLS copies, the shared negative index and unchanged original template bytes. A synthetic registered host thunk follows FS, StackBase and the index to modify only the selected thread's TLS. No generated game body executes.
* Contracts cover absent and empty TLS, the one MiB TLS template and sixteen MiB stack boundaries, unchanged neighboring thread data, duplicate and excess threads, malformed IDs, oversized stacks, changed indices, missing binding, wrong runtime identity, cancellation, uncommitted index storage, address collisions, backing budget exhaustion and preparation under a backing lease. Native coverage also rejects preparation while a runtime owned synthetic worker is active.
* No webpage changed, no browser run was performed and no real ISO or proprietary executable was read for this slice. Thread lifecycle, callback dispatch, production services and game startup remain blocked independently.

Ignored evidence: `out/image-tests/thread-tests.log`, `thread-all-tests.log` and per platform results. This slice does not accept P3.1, P3.3 or P3.5.

## Bounded event and semaphore dispatcher slice

`src/browser/dispatcher.h` and `src/browser/dispatcher.cpp` add a `Dispatcher` paired with one CPU `Runtime`. Notification events retain their signal until reset; synchronization events consume one signal; semaphore waits consume one permit. Set, reset and release return the prior signal count. Invalid object types, nonpositive releases and limit overflow reject without changing state. Semaphore arithmetic checks remaining capacity before addition, including at `INT32_MAX`.

One dispatcher holds at most 128 live objects and two concurrent waits, matching the current runtime worker ceiling rather than implying a production thread budget. Opaque nonzero identifiers increase by four and are never reused; exhaustion rejects instead of wrapping. Retired waiters retain private object references, so at most two retired objects can coexist with the live table. `Destroy` removes the identifier, marks its object retired and wakes pending waits with `Destroyed`. A replacement cannot satisfy the retired wait. This is explicit internal retirement, not the eventual Xbox close handle contract, where a pending operation may retain an object after handle closure. Identifiers are local to this dispatcher, not guest handles or guest pointers.

All predicate tests, consumption and mutation serialize under the dispatcher mutex. A waiter samples `Runtime::Sequence` while holding that mutex, then releases it before `Runtime::Wait`. A mutation publishes the predicate before incrementing the runtime sequence. Completion between predicate checking and blocking therefore cannot lose a wake. Wakes shared with unrelated runtime work recheck the predicate without renewing the original deadline. The current two waiter ceiling makes waking both acceptable; no fairness or FIFO guarantee is claimed.

Wait budgets are explicitly host monotonic milliseconds from zero through 30,000, not Xbox timeout values. Zero polls the predicate. Negative and excessive durations reject before deadline arithmetic. Positive remaining fractions round upward to avoid early expiry. This bounded internal API deliberately does not interpret NT absolute or relative 100 ns deadlines, guest pause time, indefinite waits or alertable APC waits. Guest clock and timeout translation remain integration gates rather than silent substitutions of browser time.

Every operation except retirement checks runtime cancellation. `Runtime::Stop` wakes a blocked dispatcher wait, unwinds its bookkeeping and joins the owned worker. Polls also reject under a guest backing lease. No guest pointers or host spans cross a wait. The owner must keep the dispatcher and its runtime alive until all callers exit, stopping and joining runtime workers before service destruction. The service does not create detached workers, initialize guest dispatcher headers or register kernel exports.

### Reproduction and observations

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
node --test tests/browser/dispatcher.test.mjs
node --test tests/browser/*.test.mjs
```

* Tests were written first. Native and Wasm initially failed compilation because `src/browser/dispatcher.cpp` was absent.
* Both contracts pass. The full suite passes all 59 tests. Native execution uses AddressSanitizer and UndefinedBehaviorSanitizer. Wasm runs under Node with real Emscripten pthreads, a bounded pool and filesystem support disabled.
* Tests cover notification persistence and reset, single consumption of synchronization events, semaphore contention and exact permit counts, invalid releases, maximum signed counts, wrong types and identifiers, zero polling, finite expiry, extreme duration rejection and unrelated wakes without deadline extension.
* Two concurrent waiters exercise event broadcast, one consumer per synchronization event signal and semaphore release. A 200 round request and response handoff exercises racing publication and wait admission without lost completions on both platforms. This is regression coverage, not an exhaustive scheduling proof or a fairness claim.
* Retirement wakes a pending waiter distinctly from signaling, rejects stale identifiers and cannot consume a fresh replacement. Capacity rejection and 1,000 create and retire cycles leave zero live objects. Cancellation through an actual runtime owned synthetic worker leaves zero active workers and waiters; retirement remains available for cleanup after cancellation.
* No webpage changed, no browser run was performed and no real ISO or proprietary executable was read for this slice. No game function, guest filesystem completion or production kernel wait caller is connected. Timers, mutants, wait multiple, APCs, NT timeout conversion and guest header synchronization remain absent.

Ignored evidence: `out/dispatcher-tests/tests.log`, `all-tests.log` and per platform result files. This slice does not accept P3.5 or any M3 exit item. It chooses a small runtime paired predicate service over porting the native process globals, detached timer threads or approximate pulse behavior. Existing image and ISO harnesses remain unchanged.

## Bounded multiple object wait slice

`Dispatcher::WaitMultiple` extends the same runtime paired predicate service to one through 64 distinct internal identifiers. `WaitMode::Any` consumes only the first ready object in input order and reports its zero based index. `WaitMode::All` checks every predicate and consumes every synchronization signal or semaphore permit in one mutex critical section, only when the whole set is ready. Notification events remain signaled in both modes. An incomplete all wait never reserves or consumes a subset, including on timeout, retirement or cancellation. Single object `Wait` now delegates to the same implementation.

Admission snapshots strong object references and the input order before publishing one active waiter. Empty, oversized, duplicate, unknown and malformed sets reject without consuming signals or leaking waiter admission. Duplicate identifiers are rejected in both modes as an explicit conservative internal contract, not a claim about Xbox wait behavior. The caller must keep the input span stable until admission; the wait does not retain the span or reread it after admission. The existing two concurrent wait ceiling applies to calls, not individual members. Admitted waits can pin up to 128 retired objects in addition to the 128 live table entries, replacing the earlier single wait slice's two retired object budget. No unbounded per object wait queue is allocated.

A retired member ends the entire pending wait with `Destroyed` and its original index, before any ready member is consumed. If multiple members have retired, the lowest input index wins. This is internal retirement behavior, not guest handle closure semantics. Successful all waits and timeouts have no selected index. Stale identifiers never bind a replacement. Runtime cancellation still takes precedence at the predicate checkpoint, and `Stop` joins owned workers blocked in either wait mode.

The shared runtime sequence and one original host monotonic deadline preserve the earlier lost wake and timeout rules. Predicates, retirement checks and all consumption remain under the dispatcher mutex; neither backing leases nor that mutex cross `Runtime::Wait`. The API still uses bounded host milliseconds, not NT 100 ns values, indefinite waits or guest pause time. No fairness, APC handling, mutant ownership, guest wait blocks or header synchronization is implied. This slice adds no exports or game startup wiring.

### Reproduction and observations

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
node --test tests/browser/dispatcher.test.mjs
node --test tests/browser/*.test.mjs
```

* Tests were extended first. Native and Wasm initially failed compilation with missing `WaitMode` and `WaitMultiple` members before implementation.
* Both dispatcher contracts pass with native AddressSanitizer and UndefinedBehaviorSanitizer and real Emscripten pthreads under Node. The complete suite passes all 59 tests, including the complete retained story CPU target. Emscripten filesystem support remains disabled for the dispatcher contract.
* Contracts cover deterministic any selection, atomic all consumption, notification persistence, no partial reservation during a pending all wait, finite all timeout despite unrelated wakes, invalid set preflight, unknown final members, duplicates, leases and the two caller admission ceiling.
* Contending any and all callers share a semaphore without losing or inventing permits. A 200 round request and response handoff exercises multiple wait publication racing admission. This is scheduling regression coverage, not exhaustive verification or a fairness guarantee.
* Both modes snapshot their identifiers before blocking and report retirement of the original member despite caller array changes and immediate object replacement. All wait retirement preserves an already signaled member. The 64 member boundary accepts and consumes the complete set, returns index 63 correctly and rejects 65 members without partial consumption.
* Actual runtime owned synthetic workers blocked in either mode cancel and join through `Stop`, leaving zero active waiters and workers. Cleanup remains available after cancellation. No webpage changed. No browser run was performed, no real ISO was read and no generated game code executes.

Ignored evidence: `out/dispatcher-tests/multiple-before.log`, `multiple-tests.log`, `multiple-all-tests.log` and per platform results. The missing trusted build identity, production exports, guest clock translation, kernel handles and real startup remain separate gates. No M3 exit item or P3.5 acceptance is claimed.

This chooses a bounded snapshot and one predicate mutex rather than acquiring permits incrementally or allocating guest wait blocks before their object lifetime contract exists. All or nothing consumption reuses the existing wake bridge without new workers, pollers or changes to image preparation and ISO loading.

## Bounded owner tracked mutant slice

`Dispatcher::CreateMutant` extends the same runtime paired service with an unowned mutant or initial ownership by a caller supplied nonzero token. Mutant waits require an explicit owner even when another member would satisfy an any wait first. Events and semaphores still accept the default zero token. These tokens are internal identities, not authenticated guest thread IDs, ETHREAD pointers or host thread IDs. The eventual lifecycle owner must assign unique tokens, prevent concurrent callers from impersonating one owner and never reuse a token while it can still own an object or participate in a wait. No fallback process global identity is used.

An unowned mutant has signal one; ownership depth is one minus its signal. The same owner may acquire recursively up to a conservative internal ceiling of 65,535 acquisitions. Other owners cannot consume the mutant until the final release. `ReleaseMutant` verifies type and ownership before changing state, returns the prior signal and wakes contenders only on the final release. Invalid release, wrong object type and excessive recursion leave state unchanged. This bound is a supported internal contract, not a claim about the Xbox recursion limit.

Any waits retain input order. All waits test availability and recursion capacity for the entire selected set before consuming anything. A satisfied all wait acquires or recursively increments every mutant and consumes other permits atomically. An incomplete set, invalid owner or recursion overflow cannot reserve a mutant or consume another object's signal. Existing object and waiter budgets, retirement precedence, backing lease rejection and the runtime sequence wake bridge remain unchanged. No new workers, pollers or per owner allocation are introduced.

`AbandonOwner` scans the bounded live table under the same predicate mutex, clears all recursive ownership for the specified token and marks those mutants abandoned before waking waiters. The next successful acquisition grants ownership but returns `Abandoned` once. Any returns the selected index; all acquires the complete set and reports the lowest abandoned input index while clearing every consumed abandoned flag. An incomplete all wait does not consume abandonment. Ordinary signaling, timeout, retirement and abandonment remain distinct outcomes. These internal outcomes are not yet mapped to guest NTSTATUS values.

Owner cleanup and retirement remain available after cancellation; new allocations, waits and releases still check runtime cancellation. Abandonment is an explicit lifecycle hook, not automatic cleanup from `Runtime::Stop` or a claim of thread exit integration. The caller must first prevent new operations under the exiting token, stop or join its wait callers and then abandon its ownership. Closing a handle is not owner exit; `Destroy` remains internal retirement and does not implement Xbox handle closure. Guest dispatcher headers, handles, exports, APCs and production thread lifetime are still absent. The native Windows object service is unchanged.

### Reproduction and observations

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
node --test tests/browser/dispatcher.test.mjs
node --test tests/browser/*.test.mjs
```

* Tests were extended first. Both platforms failed compilation with missing `CreateMutant` and owner aware wait members before implementation. Both now pass with native AddressSanitizer and UndefinedBehaviorSanitizer and real Emscripten pthreads under Node, with filesystem support disabled. The complete suite passes all 59 tests.
* Synthetic contracts verify initial ownership, recursive acquisition, prior release signals, denied nonowner releases, partial release isolation, independent final release wakeup, wrong types and null owner rejection before any signal consumption.
* Mixed event, semaphore and mutant all waits check atomic recursive consumption and preserve earlier and later signals when the recursion ceiling is reached. Exactly 65,535 releases restore availability after rejected excess acquisition, proving the failed preflight did not add ownership depth.
* Abandonment covers recursive ownership of multiple objects, repeated cleanup, incomplete all waits preserving flags, lowest index reporting for multiple abandoned members, any ordering and one time flag consumption. Two contending owners acquire sequentially after abandonment; only one receives `Abandoned`, and final release permits the other to acquire normally. This is scheduling regression coverage, not a fairness or exhaustive interleaving proof.
* Retirement wakes a blocked owner distinctly and cannot affect a replacement object. An actual runtime owned synthetic worker blocked on a held mutant cancels and joins through `Stop`, leaving zero active workers and waiters. Explicit owner cleanup then retires the held object after cancellation. All mutant contracts finish with zero live dispatcher objects and waiters.
* No webpage changed, no browser run was performed, no real ISO was read and no generated game function executes. Existing ISO and image preparation harnesses are unchanged. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/dispatcher-tests/mutant-before.log`, `mutant-tests.log`, `mutant-all-tests.log` and per platform results.

This chooses bounded explicit ownership and a teardown hook over native process globals or automatic identity inferred from host pthreads. Reusing the predicate mutex keeps mixed all waits atomic. Reversal cost is limited to mutant state and owner aware dispatcher calls; thread lifecycle and guest status translation remain separate integration gates.

## Bounded virtual clock timer slice

`Dispatcher::CreateTimer` adds notification and synchronization timers to the same 128 object budget. Notification timers retain their signal after a successful wait; synchronization timers consume one signal. Single and multiple object waits use the existing predicate mutex and runtime sequence bridge. All deadlines reached by one `AdvanceTime` call publish under that mutex before any waiter can consume them, including atomic all waits over several timers. There is no timer thread, detached poller, callback queue or per firing allocation.

`SetTimer(id, delayTicks, periodTicks)` uses an unsigned relative delay on a dispatcher local monotonic clock, initially zero. The internal tick unit is 100 ns; the caller supplies elapsed ticks explicitly. Zero delay fires immediately, including while the clock is paused. Zero period selects a one shot timer. Arming clears any old signal and returns whether a previous schedule was still pending. A replacement removes the old deadline; an overflowing deadline rejects before changing the previous signal or schedule. These are internal contracts, not an implementation of signed NT relative or absolute timeout translation.

`AdvanceTime` checks clock overflow before publication, scans the bounded live object table and wakes the runtime once if any timer expires. Periodic schedules stay anchored to the original due time. Missed firings coalesce into one signal, not accumulated permits or callbacks. The next deadline is computed using a remainder rather than iterating missed periods or multiplying an unbounded count. When no later periodic deadline is representable, the current expiry still signals and the timer disarms instead of wrapping. `TimerTime` exposes the logical clock for local diagnostics; it is separate from host monotonic wait deadlines and the CPU runtime's synthetic instruction clock.

`SetTimerPaused` freezes advancement without accumulating a later catchup interval. `AdvanceTime` discards supplied intervals while paused. The future clock driver must serialize pause boundaries and supply only the elapsed intervals intended for this clock; no browser timestamp is inferred or sampled here. Host wait budgets still expire while timers are paused. The service does not yet drive guest time, implement guest indefinite waits or keep a blocked worker progressing by itself. A production driver must run independently of blocked guest workers and be owned and stopped by the session controller.

`CancelTimer` removes the pending schedule and returns whether one existed. It preserves an already published signal and does not retire the object or complete an unsignaled wait. Cancellation and expiry serialize: whichever obtains the predicate mutex first determines whether a signal was published. Cancellation remains available for cleanup after runtime cancellation, as do object retirement and mutant abandonment. New timers, rearming, clock advancement and pause changes reject a cancelled runtime. Destroyed objects have no future schedule in the live table and cannot signal replacement identifiers.

### Reproduction and observations

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
node --test tests/browser/dispatcher.test.mjs
node --test tests/browser/*.test.mjs
```

* Tests were extended first. Both dispatcher platforms failed compilation with missing `CreateTimer`, `TimerTime` and `SetTimer` members before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete suite passes all 59 tests. Emscripten filesystem support remains disabled for the dispatcher contract.
* Invented timers cover exact expiry boundaries, notification persistence, single synchronization consumption, zero delay, rearm clearing a signal, replacement deadlines, cancellation before and after expiry, periodic coalescing and original phase preservation.
* Paused advancement leaves the clock unchanged, discards even an extreme supplied interval and preserves pending signals. A finite host wait still times out while the guest timer clock is paused. Resume permits later explicitly supplied time to expire the timer.
* Independently advancing time wakes a blocked mixed all wait and publishes equal deadlines for an all wait atomically. Notification expiry releases two waiters. Two immediate synchronization expiries release two contenders exactly once each. Cancelling an unsignaled timer leaves its waiters pending rather than inventing completion. This is scheduling regression coverage, not exhaustive verification or a fairness guarantee.
* Clock overflow and overflowing rearm preserve the prior time and schedule. Periodic expiry at the unsigned clock ceiling signals and disarms without wrapping. Timers share the live object capacity; retirement cannot signal a replacement. Stop cancels and joins an actual runtime owned synthetic timer waiter, and explicit timer cleanup leaves zero objects and waiters.
* No webpage changed, no browser run was performed, no real ISO was read and no generated game code executes. No DPC or APC callback is accepted or silently dropped. No guest timer header, handle, export, signed NT timeout or production clock driver is connected. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/dispatcher-tests/timer-before.log`, `timer-tests.log`, `timer-all-tests.log` and per platform result files.

This chooses an explicitly driven bounded timer table over native process globals, detached timer threads or host time masquerading as guest time. The clock driver, guest status translation and DPC ownership can be added independently without changing timer signal consumption or atomic wait behavior.

## Checked guest wait deadline slice

`Dispatcher::WaitGuest` and `WaitMultipleGuest` add a separate guest clock contract without changing the existing bounded host millisecond waits. They share admission, retirement, owner checks, atomic consumption and the runtime sequence bridge with host waits. Deadlines are snapshotted under the predicate mutex before waiter admission and never renewed by unrelated wakes. No guest pointer or backing lease crosses a wait.

A missing optional signed timeout means indefinite guest time, not an integer sentinel. Zero polls. Negative values specify relative 100 ns ticks on the same explicitly advanced monotonic clock as timers. The magnitude calculation supports `INT64_MIN` without signed negation overflow; addition rejects an unrepresentable deadline before consuming any signal. Positive values specify absolute system time in 100 ns units since 1601. These require a trusted system time origin supplied when constructing the dispatcher. Missing origin rejects before consumption. Absolute comparison subtracts the origin rather than adding elapsed ticks to it, avoiding overflow. `INT64_MAX` remains a finite absolute timeout, unlike the native service's current sentinel convention.

The origin is fixed for this internal contract. The future session controller must supply a coherent epoch for guest system time queries and clock advancement; neither the ISO nor an implicit host clock supplies it. Guest system time adjustment is unsupported. Relative waits and zero polls need no origin. `AdvanceTime` now wakes the runtime after every positive unpaused advance, even with no timers armed, so an independent driver can expire blocked guest waits. Paused intervals remain discarded. A predicate published by the same advance as a wait deadline wins before timeout testing, including atomic all waits. Incomplete all waits never consume partial signals on guest timeout or watchdog expiry.

Every guest wait also requires a positive host monotonic watchdog budget, at most 30 seconds. That budget continues while guest time is paused or not advancing. Expiry returns `WatchdogExpired`, never `TimedOut` or fake signaling, and has no selected index. The owning controller must treat this as an execution boundary or failure, not map it to a successful guest operation or retry forever. Indefinite guest time therefore does not imply an unbounded blocked worker in this foundation. Runtime cancellation and object retirement still wake these waits independently of clock progress. No new thread, poller, queue or allocation budget is introduced.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher.test.mjs
node --test tests/browser/*.test.mjs
```

* Tests were extended first. Native and Wasm compilation failed with missing `WaitGuest` and constructor overload members before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete suite passes all 59 tests. Emscripten filesystem support remains disabled for the dispatcher contract.
* Five additional executions of each compiled native and threaded Wasm fixture pass. These repetitions exercise scheduling variation but do not establish exhaustive interleaving coverage.
* Synthetic contracts cover zero polls, past and current absolute deadlines, exact relative and absolute expiry without an armed timer, explicit origin requirements and independent host watchdog expiry. Extreme finite and indefinite waits do not substitute host time for guest time.
* Two concurrent waits distinguish `INT64_MAX` as a finite absolute deadline from the exact `INT64_MIN` relative magnitude, expiring one virtual tick apart. Relative overflow rejects without consuming a ready signal, while the largest representable relative deadline is accepted. Absolute comparisons remain safe even with an unsigned origin and elapsed time near their ceiling.
* Pause prevents guest deadline progress and discards supplied intervals while the host watchdog remains active. Resume expires a relative deadline with subsequent supplied time. Mixed event and timer all waits cover coincident signal and timeout precedence, atomic consumption, timeout and watchdog isolation, and later any selection of a preserved signal.
* Invalid watchdog budgets, backing leases and absent absolute origin preserve signals and waiter admission. Retirement ends an indefinite guest wait distinctly. Stop cancels and joins an actual runtime owned synthetic worker in an indefinite guest wait, leaving no active workers or waiters without clock progress. This is scheduling regression coverage, not exhaustive interleaving verification or a fairness guarantee.
* No webpage changed, no browser run was performed, no real ISO was read and no generated game function executes. Guest timeout pointer decoding, NTSTATUS translation, handles, dispatcher headers, alertable APC waits, system time adjustment and the production independent clock driver remain absent. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/dispatcher-tests/guest-timeout-before.log`, `guest-timeout-tests.log`, `guest-timeout-all-tests.log`, `guest-timeout-repeat.log` and per platform results.

This chooses one predicate implementation with distinct clock domains and explicit watchdog failure over converting guest deadlines into host sleeps or copying the native unchecked timeout arithmetic. The reversal cost is limited to deadline translation and clock notification; timer scheduling, ISO loading, image preparation and production exports remain separate.

## Checked guest timeout pointer and wait status slice

`src/browser/guest-wait.h` and `src/browser/guest-wait.cpp` add `WaitGuestStatus` over the existing dispatcher guest deadline contract. The caller supplies already resolved internal object identifiers, their wait mode, an explicit owner token where required, a guest timeout address and a bounded host watchdog. These identifiers are not guest handles or dispatcher object pointers. There is no object lookup, guest argument stack decoding, dispatcher header synchronization or automatic production export registration in this adapter.

A zero timeout address means indefinite guest time. A nonzero address reads exactly eight bytes through checked compact memory, decodes little endian bits and preserves the signed value, including `INT64_MIN` and `INT64_MAX`. Unaligned byte spans are supported without host pointer casts. Unmapped, uncommitted, partial and wrapping spans fault before waiter admission or signal consumption. The timeout is copied before admission; subsequent guest mutation or decommit cannot change a pending deadline. No guest pointer or backing lease crosses the wait. The caller must pair the memory and dispatcher with the same runtime generation and serialize teardown until all callers have exited.

Completed any waits return `STATUS_WAIT_0 + index`; completed all waits return `STATUS_SUCCESS`. Abandoned acquisitions return `STATUS_ABANDONED_WAIT_0 + index`, retaining the dispatcher's lowest abandoned input index for all waits. Guest deadline expiry alone returns `STATUS_TIMEOUT`. These conventional NT encodings follow the [documented kernel multiple wait return values](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-kewaitformultipleobjects); they are a bounded adapter contract, not evidence of all Xbox edge cases or production compatibility. Predicate consumption and abandonment remain atomic in the dispatcher, before status translation.

Host watchdog expiry throws the distinct `WaitWatchdogExpired` fault rather than returning a guest timeout. Runtime cancellation propagates without translation. Internal retirement faults rather than inventing guest handle closure semantics. Alertable requests reject before consuming signals because APC delivery is not implemented; the adapter does not silently run them as nonalertable waits. Invalid sets, owner identities, deadlines and host budgets retain the dispatcher's existing preflight checks. No exception is caught and converted to successful guest completion. The future owning session must stop on watchdog or unsupported behavior, not retry indefinitely.

### Reproduction and observations

```sh
node --test tests/browser/guest-wait.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Native and Wasm initially failed compilation because `src/browser/guest-wait.cpp` was absent.
* Both new contracts pass with native AddressSanitizer and UndefinedBehaviorSanitizer and real Emscripten pthreads under Node. The serial full suite passes all 61 tests, including the complete retained story CPU target. Emscripten filesystem support remains disabled for the new contract.
* An initial concurrent full suite run passed 60 of 61 tests; the existing native image fixture returned no exit status within its bounded execution. No image test limit or runtime code was changed. The subsequent serial run passes every test. The logs retain both outcomes rather than claiming a clean concurrent run.
* Five further executions of each compiled native and threaded Wasm fixture pass. These runs exercise scheduling variation, not exhaustive interleaving coverage or fairness.
* Synthetic fixtures cover zero polling, signed relative and absolute timeout pointers, unaligned decoding, both signed extremes, null indefinite timeouts, independent watchdog faults and partial all waits preserving ready signals. Mutation and decommit after admission leave the original deadline intact.
* Unmapped, wrapping, partially uncommitted and out of range timeout spans, inherited backing leases, invalid watchdog budgets and unsupported alertable requests reject without consuming signals or leaking waiter admission. No host pointer is retained into guest memory.
* Status contracts cover any index 63, abandoned index 63, atomic all success, abandoned any and all results, lowest index selection with multiple abandoned mutants and one time clearing of every acquired abandonment flag. Internal retirement is an explicit fault, not a guest status.
* An explicitly registered synthetic thunk returns a real adapter status through the runtime's kernel dispatch path. An actual runtime owned synthetic worker in a null timeout wait cancels and joins through `Stop` without a fabricated guest return, leaving zero active workers and waiters. The test registration is not a production export implementation or game ABI adapter.
* No webpage changed, no browser run was performed, no real ISO was read and no generated game function executes. Production handles, guest object headers, real calling conventions, coherent system time queries, independent clock driving and APC delivery remain separate gates. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/guest-wait-tests/before.log`, `all-tests.log`, `serial-all-tests.log`, `repeat.log`, per platform results and compiled fixtures.

This chooses a small checked timeout and status boundary over borrowing guest memory across waits or registering partially implemented kernel exports. Existing ISO loading, image preparation, dispatcher predicates and timer scheduling are unchanged. A later handle and thread lifecycle adapter can call this boundary after resolving real objects and owner identity.

## Bounded dispatcher handle lifetime slice

`src/browser/dispatcher-handles.h` and `src/browser/dispatcher-handles.cpp` add `DispatcherHandles`, a table paired with one dispatcher and its runtime. It creates unnamed events and semaphores, duplicates their handles, closes handles and resolves event and semaphore operations. At most 128 handles are live. Nonzero identifiers start at `0x1000`, advance by four and never reuse a closed identifier within the table. Exhaustion rejects before wrapping. Duplicates share the same dispatcher object, not a copied signal count. The existing 128 live dispatcher object ceiling remains independent of the handle ceiling.

`WaitGuestStatus` resolves one through 64 handles under the table mutex into fixed arrays of strong object references and internal identifiers, then releases that mutex before entering the checked timeout and status adapter. Unknown final members reject without consuming earlier signals. Different handles for the same object remain duplicate wait members and reject before consumption in either mode. Input order and the underlying object identities are snapshotted before dispatcher admission. The caller must keep its input span stable during this snapshot; later mutation cannot redirect a pending wait. Guest handle array decoding is not implemented.

Closing a handle removes that lookup immediately. A pending operation retains its object even after every handle closes, so closure cannot produce the dispatcher's internal `Destroyed` result or bind a replacement. Signals through a surviving alias can complete the original wait. Final object retirement occurs only after all handles and operation references have released, including unwind on guest timeout, host watchdog, preflight failure or runtime cancellation. Pending all waits pin every member without reserving partial signals. Closed objects pinned by these operations still count against the dispatcher's object budget; a free handle slot does not bypass it. No retirement queue, worker, poller or unbounded container is added.

The table mutex protects lookup and publication; dispatcher predicates retain their existing separate mutex. No table mutex or guest backing lease crosses a wait. Object retirement from `Close` happens after releasing the table mutex. Creation preflights handle capacity before allocating an object and publishes only after dispatcher creation succeeds. New operations and duplication check the paired runtime's cancellation; close and destruction remain available for cleanup afterward. The table owns its internal objects exclusively: callers must not retire those identifiers directly. The dispatcher and runtime must outlive the table and all operations. The owner must join all callers before destroying the table and must not replace the table while continuing the same handle namespace or runtime generation.

This is a bounded internal lifetime contract, not production Xbox handle compatibility. Named objects, access rights, pseudo handles, guest object pointers and headers, guest argument decoding, NTSTATUS translation of invalid handles, production export registration, mutant ownership retention and timer handle lifetime remain absent. Mutants and timers deliberately have no create or adopt API here: final mutant handle closure must not substitute for owner exit, and timer scheduling lifetime needs its own contract. Their existing internal dispatcher APIs are unchanged. No raw object adoption or low level Wasm export provides a bypass around the image authentication gate.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher-handles.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Both platforms failed compilation because `src/browser/dispatcher-handles.cpp` was absent before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete serial suite passes all 63 tests, including the complete retained story CPU target. Emscripten filesystem support remains disabled for the new contract.
* Five further executions of each compiled native and threaded Wasm fixture pass. Each execution includes 100 close, snapshot, replacement and signal handoffs. These repetitions exercise scheduling variation, not exhaustive interleaving verification or fairness.
* Contracts cover shared event signals and resets, single synchronization consumption through aliases, invalid and double closes, stale lookup, alias duplication, wrong object types, overflowing semaphore release, invalid timeout pointers, backing leases and duplicate underlying wait members without partial consumption.
* Pending waits survive closing their original handle and every remaining alias. A fresh signaled replacement cannot satisfy the old wait, and caller array changes after admission cannot redirect it. Two pending semaphore waiters each consume exactly one permit released through an alias despite final closure.
* An incomplete all wait retains both closed objects until the distinct host watchdog fault. At the 128 object ceiling, a closed but pinned object prevents new construction despite a free handle slot; a real virtual guest timeout releases its capacity and permits later creation. Handle capacity also rejects duplication and construction without leaking objects. Index 63 retains its status through resolution.
* Stop cancels and joins an actual runtime owned synthetic worker whose last handle is already closed. Its pending reference retires on unwind, and unrelated open handles remain available for explicit cleanup after cancellation. Table destruction releases leftover handles once callers have exited. All contracts end with zero handles, objects and waiters.
* No webpage changed, no browser run was performed, no real ISO was read and no generated game function executes. Production handles, real wait exports, thread lifecycle and the independent clock driver remain separate gates. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/dispatcher-handles-tests/before.log`, `tests.log`, `all-tests.log`, `repeat.log`, per platform results and compiled fixtures.

This chooses bounded strong operation references over treating close as dispatcher retirement. Existing timeout translation, atomic predicate consumption, ISO loading and image preparation remain unchanged. Production handle lookup can reuse this lifetime boundary only after its missing object kinds, calling convention and error semantics are established.

## Checked dispatcher wait ABI slice

`src/browser/dispatcher-wait-abi.h` and `src/browser/dispatcher-wait-abi.cpp` add `InvokeDispatcherWait` over the existing handle lifetime and checked timeout adapters. It follows the argument layouts in `src/kernel/xboxkrnl.cpp` for ordinals 233 (`NtWaitForSingleObject`), 234 (`NtWaitForSingleObjectEx`) and 235 (`NtWaitForMultipleObjectsEx`). The caller explicitly supplies the table, memory, context, ordinal and bounded host watchdog. No production registration, process global service pointer or low level Wasm export is added. The native Windows exports are unchanged.

The adapter reads one complete return and argument frame through checked compact memory and decodes little endian dwords without host pointer casts. The frame and, for multiple waits, one through 64 handles are private fixed array snapshots before table resolution. Unaligned spans are supported. Count checks precede array size arithmetic and I/O. Null, wrapping, unmapped and partially uncommitted arrays reject before signal consumption. Wait type zero means all; one means any; other values fault. Extended calls accept processor mode zero or one only. Both use checked memory and the same nonalertable predicate path; user APC delivery and mode dependent pointer probing are not implemented. The low byte of the alertable argument follows the existing native BOOLEAN convention; a nonzero byte faults rather than silently dropping APC behavior.

The stack frame, handle array and timeout storage can be mutated or decommitted after admission without redirecting a pending wait. No guest backing lease or table mutex crosses blocking. Only a completed guest result writes EAX and advances ESP past the original return and arguments, by 16, 20 or 28 bytes. No guest return address is invoked by this host adapter, matching the existing native stdcall wrapper convention. Cleanup uses the original snapshotted stack address; it does not reread guest bytes after waiting or require that backing to remain committed. Stack cleanup must remain representable in 32 bits, so a frame ending at exactly four GiB rejects even if its bytes are mapped. A handle array ending at that boundary remains valid.

Invalid handles, unsupported ordinals, malformed requests, watchdog expiry and cancellation propagate as explicit faults, not invented guest status values. They publish neither a result nor stack cleanup. Existing completion statuses, all or nothing consumption, alias rejection and operation pins retain their prior contracts. The caller must pair the memory, context and table with one runtime generation, keep the context exclusive to its guest caller and join callers before teardown. This is a checked calling convention boundary for the supported internal event and semaphore handles, not a complete production Xbox wait export. Invalid handle NTSTATUS translation, named and pointer objects, dispatcher headers, mutant and timer handles and thread identity remain separate gates.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher-wait-abi.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Both platforms failed compilation because `src/browser/dispatcher-wait-abi.cpp` was absent before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete serial suite passes all 65 tests, including the complete retained story CPU target. Emscripten filesystem support remains disabled for this contract.
* Five further executions of each compiled native and threaded Wasm fixture pass. These runs exercise scheduling variation, not exhaustive interleaving verification or fairness.
* Synthetic frames verify all three argument layouts, little endian unaligned decoding, low byte BOOLEAN handling, exact stdcall cleanup, preservation of unrelated registers, any index selection and atomic all completion. A 64 handle array preserves index 63. A single handle in the final address space dword is accepted; cleanup wrapping from a fully mapped high stack frame is rejected.
* Invalid counts, wait types, processor modes, alertable requests, unknown ordinals, null arrays, unknown and aliased handles, partial stack and array commitment, invalid timeout pointers and inherited backing leases preserve a ready signal and leave EAX, ESP and waiter admission unchanged. A valid stack frame needs no trailing mapped bytes beyond its exact extent. The independent watchdog remains a fault without guest timeout or stack publication.
* A pending all wait survives decommit of its stack, array and timeout pages plus final member closure, then returns a genuine virtual guest timeout without consuming the ready member. Another pending wait ignores later frame and array mutation, completes through a surviving alias after its original handle closes and leaves a replacement signal unconsumed.
* Explicit test only registration invokes ordinal 235 through the runtime kernel dispatch path using a real checked argument frame. Stop cancels and joins an actual runtime owned synthetic worker in the adapted wait after final handle closure, releasing all object pins. All contracts finish with zero handles, objects and waiters. This registration is not a production export catalog or generated game call.
* No webpage changed, no browser run was performed, no real ISO was read and no generated game function executes. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/dispatcher-wait-abi-tests/before.log`, `tests.log`, `all-tests.log`, `repeat.log`, per platform results and compiled fixtures.

This chooses an explicit narrow ABI adapter over copying native unchecked stack access or automatically registering partially supported services. Existing dispatcher predicates, handle lifetime, image authentication and ISO loading are unchanged. Production bootstrap can wire this boundary only after handle kinds, error statuses, guest headers, owner identity and clock driving are established.

## Bounded timer handle and scheduling lifetime slice

`DispatcherHandles` now creates notification and synchronization timers, arms or replaces their schedules and cancels them through checked handles. Duplicates share one signal and deadline, including periodic phase and cancellation. All new operations acquire the same strong operation reference used by event and semaphore operations. Timer construction preflights the existing 128 handle ceiling before allocating a dispatcher object; closed but operation pinned timers still count against the independent 128 object ceiling. Wrong types, closed handles, deadline overflow and runtime cancellation remain explicit faults.

Scheduling is embedded in the live dispatcher object, not a separate worker, callback, queue or owning reference. A deadline alone does not keep a timer alive. Closing the final handle without pending operations retires the object and its schedule immediately, even if periodic. A pending wait pins the timer and keeps its original schedule eligible for `AdvanceTime` after every handle closes. Completion, a genuine guest timeout, the independent host watchdog or runtime cancellation releases that pin. Once all operation pins and handles are gone, retirement removes the object from the same mutex protected table scanned by clock advancement. Expiry racing final retirement cannot resurrect a schedule or redirect a waiter to a replacement. An indefinite closed unsignaled wait still requires an existing schedule, watchdog or runtime cancellation to complete; closure does not invent a signal.

Cancellation through a surviving alias removes the schedule without retiring waiters or clearing an already published signal. Rearming clears the signal and can complete existing pinned waits on a later expiry. Notification expiry broadcasts; synchronization expiry permits one consumer; periodic missed expirations still coalesce. All scheduling and guest timeout units remain explicit virtual 100 ns ticks, with pause behavior unchanged. No host clock driver or conversion of guest timer due time pointers is added. Handle lookup rejects new cancellation operations after runtime cancellation, matching the existing table policy; close and table destruction still retire armed timers during cleanup.

The checked wait ABI needs no new path: resolved timer handles flow through the same fixed snapshots, atomic predicates, status adapter and stdcall publication as events and semaphores. Mutant handles remain absent because ownership must survive acquisition beyond a wait operation. Named timers, guest timer headers, APC and DPC delivery, timer create or set export argument decoding, guest error status translation and production registration remain separate gates. The dispatcher and runtime must outlive the handle table and every operation; callers must still stop and join workers before teardown.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher-timer-handles.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Native and threaded Wasm initially failed compilation with missing `CreateTimer`, `SetTimer` and `CancelTimer` members before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete serial suite passes all 67 tests, including the retained story CPU target. Emscripten filesystem support remains disabled for the new contract.
* Five further executions of each compiled native and threaded Wasm fixture pass. Each execution includes 100 closed pending wait and replacement handoffs plus 100 clock advancement versus final closure races. These runs exercise scheduling variation, not exhaustive interleaving verification or fairness.
* Contracts cover shared deadlines and cancellation through aliases, pause, notification persistence, synchronization consumption, periodic coalescing, rearming, wrong types, stale handles, invalid timeout storage and overflow without losing the original schedule. Duplicate underlying timers reject without consumption. Any index 63 and atomic all consumption across 64 timers retain their status contracts.
* Closed pending timers complete from their original one shot or periodic schedule without consuming a signaled replacement. Mixed event and timer all waits preserve partial signals. Two closed synchronization wait pins consume separate expirations; two closed notification wait pins both complete from one expiry. Guest timeout and the distinct host watchdog retire closed armed timers without fabricating success or internal destruction statuses.
* At the object ceiling, a closed but pinned timer prevents construction despite an available handle slot. Expiry releases that capacity. Final closure and table destruction retire armed periodic timers without waiting for another clock tick. A real checked ordinal 233 stack frame consumes a timer signal and publishes the expected stdcall result.
* Stop cancels and joins an actual runtime owned synthetic worker waiting on a closed armed periodic timer, releasing its final pin. New timer operations reject afterward while close cleans up unrelated armed timers. Every fixture ends with zero handles, objects, waiters and active workers.
* No webpage changed, no browser run was performed, no real ISO was read and no generated game function executes. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/dispatcher-timer-handles-tests/before.log`, `tests.log`, `all-tests.log`, `repeat.log`, per platform results and compiled fixtures.

This chooses operation pinned schedules over an independent scheduler reference that could retain closed periodic timers indefinitely. The existing dispatcher already serializes deadline scanning and retirement, so no extra lifetime registry or worker is needed. Image authentication, ISO loading, guest wait deadlines and production export registration are unchanged.

## Checked signed guest timer scheduling slice

`Dispatcher::SetTimerGuest` accepts a signed 64 bit due time and a signed 32 bit millisecond period. Negative due times are relative 100 ns durations; positive values are absolute system time in the same explicit origin and pausable virtual clock used by guest waits. Zero expires immediately without requiring an origin. A missing origin for a positive due time faults before changing the timer. Past absolute deadlines expire immediately. No browser timestamp is substituted for guest system time.

Waits and timer scheduling now share the same checked deadline conversion under the dispatcher mutex. `INT64_MIN` is decoded without signed negation overflow, and relative deadlines must fit the remaining unsigned virtual clock range. Negative periods fault. Nonnegative periods multiply by 10,000 in widened arithmetic, including `INT32_MAX`; zero remains one shot. Conversion, validation and schedule publication serialize with `AdvanceTime`, so a concurrent clock tick cannot move the relative deadline between calculation and publication. The existing timer expiry path retains notification persistence, synchronization consumption, periodic phase and coalescing. An unrepresentable next periodic expiry publishes the current signal and disarms rather than wrapping.

`DispatcherHandles::SetTimerGuest` adds a narrow internal pointer adapter. It requires a nonnull due time pointer and no inherited backing lease, pins the checked handle, snapshots exactly eight little endian bytes through compact memory and forwards the signed value. Unaligned and final address space spans are supported; null, wrapping, unmapped or partially committed spans reject before publication. There is no retained guest pointer or backing lease. Later mutation or decommit cannot redirect the schedule, including after final handle closure when an admitted wait pins the timer. Wrong object kinds, closed handles, invalid periods, missing origins, arithmetic overflow and cancellation remain explicit faults. Failed replacement preserves the old signal, deadline and period.

This is not an Xbox timer export ABI. The native `KeSetTimer` and `KeSetTimerEx` take a guest timer object pointer and due time by value, not this internal handle and due time pointer contract. Guest timer headers, object pointer resolution, APC and DPC callbacks, create or set export decoding, guest error status translation and production registration remain absent. No low level Wasm export or authentication bypass is added. The production clock driver and real thread lifecycle remain separate gates.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher-guest-timer.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Native and threaded Wasm initially failed compilation with missing `SetTimerGuest` members before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete serial suite passes all 69 tests, including the retained story CPU target and existing wait deadline contracts. Emscripten filesystem support remains disabled for the new contract.
* Five further executions of each compiled native and threaded Wasm fixture pass. Each execution includes 100 relative schedule publications racing independent clock advancement. These repetitions exercise scheduling variation, not exhaustive interleaving verification or fairness.
* Contracts cover exact relative and absolute expiry, zero and past deadlines, explicit origin failure, replacement, pause, alias cancellation, notification persistence, synchronization consumption, periodic coalescing and exact millisecond conversion through the maximum signed period. Extreme signed due times, relative overflow, the final representable clock tick and periodic next deadline overflow retain bounded behavior.
* Invalid required pointers, partial commitment, wrap, wrong kinds, unknown handles, negative periods and inherited backing leases preserve existing schedules. Failed replacement also preserves an already published signal and the original periodic phase. Unaligned due time bytes and an eight byte span ending exactly at four GiB are accepted.
* Guest due storage mutation and decommit after scheduling cannot redirect expiry. A genuine pending wait completes from its signed schedule after every timer handle closes and the due storage is decommitted. All fixtures finish without retained handles, objects or waiters; cancellation rejects new signed scheduling while internal retirement remains available for cleanup.
* No webpage changed, no browser run was performed, no real ISO was read and no generated game function executes. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/dispatcher-guest-timer-tests/before.log`, `tests.log`, `all-tests.log`, `repeat.log`, per platform results and compiled fixtures.

This chooses one mutex protected deadline conversion shared with guest waits over converting absolute or relative deadlines outside the scheduling lock. The existing timer objects and operation pins own lifetime; no clock worker, schedule registry or extra owning reference is introduced. ISO loading, image authentication and production export registration are unchanged.

## Bounded mutant handle and ownership lifetime slice

`DispatcherHandles` now creates unowned or initially owned mutants, releases them through checked handles and exposes explicit owner abandonment for lifecycle cleanup. Handle waits use the nonzero `X86Context::thread_handle` as their internal owner identity. Events, semaphores and timers still accept a zero context identity. Duplicate handles share recursive ownership, signal state and abandonment. The existing wait adapter preserves indexed abandoned NTSTATUS values, including atomic all acquisition; the checked stdcall wait adapter needs no new argument path. Wrong kinds, wrong or zero release owners, closed handles, recursion overflow and cancellation remain explicit faults.

A mutant's ownership must outlive a successful wait and every handle. The final shared handle or operation reference now calls a private dispatcher retirement boundary. Unowned objects retire immediately, preserving previous event, semaphore and timer lifetime. A still owned mutant instead remains in the same bounded live object table with an unreferenced marker. This marker adds no owning reference, allocation, registry or cycle. Closing the final handle cannot impersonate thread exit or make the object available. Closed owned mutants continue consuming the 128 object budget even with no handles or pending operations.

Ownership acquisition, release, abandonment and final reference retirement serialize under the existing dispatcher mutex. A pending wait retains its normal operation reference across closure. If it later acquires a mutant, final operation completion retains the new ownership rather than destroying the object. If it times out, reaches the host watchdog or is cancelled, it does not acquire ownership or drop the previous owner's retention. Explicit abandonment resets the entire recursive count and wakes contenders. With surviving references, one acquiring contender consumes the abandoned state; without references, abandonment retires the closed object immediately. The final release path also retires any unreferenced object before waking waiters. Internal unconditional `Destroy` remains separate from handle closure; raw dispatcher identifiers must not be used to retire handle managed objects.

The dispatcher and runtime must outlive all handle tables and operations. Table destruction closes its references but does not abandon owners. Runtime Stop cancels waits but does not yet perform production thread exit cleanup. The lifecycle owner must stop and join a departing thread, prevent further acquisition under its identity, then explicitly abandon that identity, including during cancellation cleanup. Cleanup remains available after runtime cancellation. Reusing an identity before cleanup or abandoning it concurrently with that same thread acquiring more objects is unsupported. The current synthetic identities are not a production identity authority or scheduler.

Named mutants, guest dispatcher headers, real handle error statuses, create and release export ABI decoding, production registration and automatic owner exit integration remain absent. No new low level Wasm export, image authentication bypass, clock worker or generated game call is added. Existing ISO and image harnesses are unchanged.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher-mutant-handles.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Native and threaded Wasm failed compilation with missing `CreateMutant`, `ReleaseMutant` and `AbandonOwner` members before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete serial suite passes all 71 tests, including the retained story CPU target. Emscripten filesystem support remains disabled for the new contract.
* Five further executions of each compiled native and threaded Wasm fixture pass. Each execution includes 100 closed pending wait handoffs, 100 owner abandonment versus final closure races and 100 release versus final closure races. These repetitions exercise scheduling variation, not exhaustive interleaving verification or fairness.
* Contracts cover initially owned and unowned mutants, recursive alias acquisition, wrong owner rejection, partial and final release, final closure after both initial ownership and successful wait completion, exact one time abandonment and table destruction without fake owner exit. A closed pending wait acquires abandoned original ownership without touching a signaled replacement, then retains that new ownership until explicit cleanup.
* Mixed event and mutant all waits preserve partial signals and report the abandoned input index. Two contenders consume abandonment once and hand off ownership through real release. Atomic all acquisition covers 64 abandoned mutants; any wait preserves abandoned index 63. Aliased sets and the 65,535 recursive acquisition ceiling reject before consuming a ready event.
* Guest timeout and the distinct host watchdog release operation pins without changing original ownership. A checked ordinal 233 stack frame acquires using context identity and retains ownership after final closure. Closed ownership fills the object ceiling independently of available handle slots; abandonment frees all 128 slots without leaking a failed creation.
* Stop cancels and joins an actual runtime owned synthetic mutant waiter. Cancellation neither acquires nor abandons unrelated ownership; explicit owner cleanup retires closed mutants afterward. Every fixture ends with zero handles, objects, waiters and active workers.
* No webpage changed, no browser run was performed, no real ISO was read and no generated game function executes. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/dispatcher-mutant-handles-before.log`, `out/dispatcher-mutant-handles-tests/tests.log`, `all-tests.log`, `repeat.log`, per platform results and compiled fixtures.

This chooses bounded dispatcher retention under the existing ownership mutex over a second strong reference registry that would need atomic publication after each wait. Final operation destruction observes the already committed owner, so no gap exists between successful acquisition and lifetime retention. The remaining production work is explicit thread identity, lifecycle cleanup and export integration, not a hidden detached owner monitor.

## Checked event and mutant object ABI slice

`src/browser/dispatcher-object-abi.h` and `src/browser/dispatcher-object-abi.cpp` add `InvokeDispatcherObject` for six native handle ordinals: 186 `NtClearEvent`, 187 `NtClose`, 189 `NtCreateEvent`, 192 `NtCreateMutant`, 221 `NtReleaseMutant` and 225 `NtSetEvent`. The decoder snapshots a bounded little endian stdcall frame, including its return address, through compact memory before any mutation. Frame reads support unaligned storage. Cleanup overflow, unknown ordinals and inherited backing leases fault without publishing guest registers.

Creation requires a nonnull handle output and null object attributes. Nonnull attributes explicitly fault rather than silently ignoring names, root directories or flags. Event type zero creates a notification event and one creates a synchronization event; other types fault. Initial state and initial ownership use only the low BOOLEAN byte. An initially owned mutant requires a nonzero context thread identity. Release uses that same identity, preserving recursive ownership and checked previous signal values. Set and release accept optional previous state outputs; clear and close have no output argument.

All output spans are checked before handle allocation or signal and ownership mutation. The adapter acquires its own compact memory lease before frame reading and holds it through output publication. This prevents decommit between validation and mutation from creating an inaccessible newly allocated handle, losing a previous signal result or releasing ownership on a failed output write. Inputs remain snapshotted even when an output overlaps the frame. No guest pointer escapes the call, no wait or I/O runs under this lease and no raw host pointer is exported. Existing handle operation pins still protect objects against concurrent final closure. The lock order is memory, handle table, then dispatcher; waits continue releasing their handle lookup lock before reading guest memory or blocking.

Successful operations publish status zero and exact stdcall stack cleanup, preserving unrelated registers. Invalid handles and kinds, unsupported names, wrong owners, exhausted budgets and malformed memory remain explicit `Fault` outcomes, not implemented Xbox error status translations. Close retains its existing cleanup availability after cancellation; allocation and ordinary mutations reject cancellation. Closing an owned mutant still does not impersonate owner exit. Guest dispatcher headers, named objects, duplication ABI, production handle namespace, automatic thread cleanup and production export registration remain absent. No low level Wasm export, authentication bypass or real startup path is added.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher-object-abi.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Both platforms failed compilation because `src/browser/dispatcher-object-abi.cpp` did not exist before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete serial suite passes all 73 tests, including the retained story CPU target. Filesystem support remains disabled for the new Wasm fixture.
* Five further executions of each compiled platform fixture pass. Every execution includes 100 output decommit versus event mutation races, checking that a failed publication cannot signal the event or publish guest register completion. These repetitions exercise scheduling variation, not exhaustive interleaving verification.
* Contracts cover notification persistence, synchronization consumption, low BOOLEAN bytes, previous signal outputs, optional outputs, initially owned and unowned mutants, recursive alias ownership, wrong owner rejection, closure through an alias and ownership retained after final close until explicit abandonment.
* Invalid required and optional output spans, partial commitment, wrapping addresses, invalid frames, unsupported event types, nonnull attributes, wrong object kinds, unknown and closed handles, zero initial owners, inherited leases and capacity exhaustion preserve handles, outputs and relevant signals or ownership. Unaligned outputs, outputs overlapping the decoded frame and a dword ending exactly at four GiB are accepted. A valid frame whose cleanup would wrap is rejected.
* A synthetic test registered kernel thunk traverses runtime dispatch into ordinal 225 and wakes an independently blocked checked ordinal 233 wait after its original handle has closed and frame bytes have changed. Both calls retain their own original cleanup bounds. This is test registration, not a production export catalog or generated game body.
* Cancellation rejects new creation and mutation, while checked close can retire the remaining handle afterward. All fixtures finish with zero handles, objects and waiters. No webpage changed, no browser run was performed and no real ISO or generated game function was used. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/dispatcher-object-abi-before.log`, `out/dispatcher-object-abi-tests/tests.log`, `all-tests.log`, `repeat.log`, per platform results and compiled fixtures.

This chooses a short memory lease across each nonblocking operation over check then mutate then write, which would allow concurrent decommit to lose a created handle or publish an event without its requested result. The lease is deliberately confined to this adapter and never extends into the existing blocking wait ABI. ISO loading, image authentication, timer scheduling and production registration are unchanged.

## Checked semaphore object ABI slice

`InvokeDispatcherObject` in `src/browser/dispatcher-object-abi.cpp` now also decodes ordinal 193 `NtCreateSemaphore` and ordinal 222 `NtReleaseSemaphore`. Creation snapshots four arguments: a required handle output, optional object attributes, signed initial count and signed maximum count. Release snapshots three arguments: a handle, signed release count and optional previous count output. The signatures and ordinal sizes were checked against XboxDev nxdk's [kernel declarations](https://github.com/XboxDev/nxdk/blob/master/lib/xboxkrnl/xboxkrnl.h) and [export definitions](https://github.com/XboxDev/nxdk/blob/master/lib/xboxkrnl/xboxkrnl.exe.def).

Counts decode as complete signed 32 bit values, not low BOOLEAN bytes or implementation dependent unsigned casts. The existing dispatcher rejects negative initial counts, nonpositive limits, initial counts above the limit, nonpositive releases and releases above remaining capacity before changing permits. A valid maximum count of `INT32_MAX` is supported. Successful release returns the exact prior count before any awakened waiter consumes a permit. Aliases share the same object and pending operation references remain pinned after original closure.

The adapter reuses the bounded frame snapshot and short backing lease already used for event and mutant operations. Required and optional outputs are preflighted before allocation or mutation and remain committed through publication. Outputs may overlap decoded inputs without changing their snapshots. Success publishes status zero and exact stdcall cleanup. Malformed memory, wrong kinds, invalid counts, unsupported nonnull attributes, exhausted budgets and cancellation remain explicit faults with no guest completion publication. No new handle namespace, kernel registration, guest object header, error status translation or production bootstrap is added.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher-semaphore-abi.test.mjs tests/browser/dispatcher-object-abi.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Both platforms initially failed at ordinal 193 with `unsupported dispatcher object ordinal` before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete serial suite passes all 75 tests, including the retained story CPU target. The new Wasm fixture has filesystem support disabled.
* Contracts cover exact initial and released permits, shared alias consumption, optional outputs, full signed values, maximum count arithmetic and rejection without permit changes. Null required outputs, invalid and partially committed spans, unaligned frames and outputs, wrong object kinds, unknown and closed handles, nonnull attributes, inherited leases and cleanup overflow are covered. Outputs overlapping count arguments and the final dword of the guest address space are accepted.
* Handle exhaustion and independent object exhaustion with an available handle slot reject without publishing a new handle or output. Cancellation rejects creation and release; checked close still retires the existing semaphore afterward.
* Each platform execution includes 100 output decommit versus semaphore release races. Failure cannot add permits or publish completed registers. Five further executions of each compiled fixture pass; repetitions exercise scheduling variation, not exhaustive interleaving verification.
* A test registered release thunk traverses runtime dispatch and wakes an independently blocked checked ordinal 233 waiter after its original handle has closed and the shared frame has changed. The waiter consumes one real permit, both calls retain their own stdcall cleanup and all handle and operation pins retire.
* Fixtures finish with zero handles, objects and waiters. No webpage changed, no browser run was performed and no real ISO or generated game code was used. No M3 exit item or P3.5 acceptance is claimed.

Ignored evidence: `out/dispatcher-semaphore-abi-before.log`, `out/dispatcher-semaphore-abi-tests/tests.log`, `all-tests.log`, `repeat.log`, per platform results and compiled fixtures. Public ABI reference snapshots are in ignored `out/nxdk-xboxkrnl-reference.txt` and `out/nxdk-xboxkrnl-ordinals.txt`.

This extends the existing nonblocking adapter rather than introducing another object table or count implementation. The underlying dispatcher remains the only authority for atomic count validation, mutation and wake ordering. Production registration, truthful Xbox error statuses and startup integration remain separate gates.

## Checked timer object ABI slice

`InvokeDispatcherObject` in `src/browser/dispatcher-object-abi.cpp` now also decodes ordinal 185 `NtCancelTimer`, ordinal 194 `NtCreateTimer` and ordinal 229 `NtSetTimerEx`. The Xbox signatures and argument counts were checked against the existing local snapshots of XboxDev nxdk's [kernel declarations](https://github.com/XboxDev/nxdk/blob/master/lib/xboxkrnl/xboxkrnl.h) and [export definitions](https://github.com/XboxDev/nxdk/blob/master/lib/xboxkrnl/xboxkrnl.exe.def). The maximum bounded stdcall snapshot is now nine words including the return address. Creation requires a handle output, null object attributes and notification type zero or synchronization type one.

Scheduling snapshots a required eight byte signed due time and a signed millisecond period before any mutation. It shares the existing atomic relative or absolute virtual deadline conversion with guest waits. Negative periods, missing absolute origins and arithmetic overflow fault before replacing a schedule. Nonnull APC routines, nonnull APC context and a true low resume BOOLEAN byte explicitly fault rather than pretending callbacks or wake behavior ran. APC mode must be kernel zero or user one even when no callback is requested. No APC or DPC execution is added.

Timer output state is not schedule membership. `Dispatcher::TimerState`, `SetTimerGuestState` and `CancelTimerState` capture both armed membership and signal state under the dispatcher mutex. Existing internal boolean APIs retain their prior armed membership contract. The new ABI uses the signal value: scheduling returns the prior signal before clearing it; cancellation returns the signal without clearing it. A scheduled but unsignaled timer therefore returns false, while an expired one shot can return true despite no longer being armed. The distinction was checked against the `KeReadStateTimer` paths in ReactOS's [NT executive timer implementation](https://github.com/reactos/reactos/blob/master/ntoskrnl/ex/timer.c), not assumed from `KeSetTimerEx`'s armed return. This is public NT reference behavior, not a captured Xbox execution trace.

Set and cancel accept optional one byte BOOLEAN outputs, not four byte words. Neighbor bytes remain untouched and the final guest address byte is a valid destination. Required create outputs remain full handle words. The adapter holds its short backing lease across frame and due time capture, output preflight, nonblocking mutation and publication. Overlap with due time or frame bytes cannot change snapshotted inputs. No lease or guest pointer survives the call. Handle operations remain reference pinned, including schedules consumed by a pending wait after final handle closure.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher-timer-abi.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Native and threaded Wasm initially rejected ordinal 194 with `unsupported dispatcher object ordinal` before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete serial suite passes all 77 tests. The new Wasm fixture has filesystem support disabled.
* Contracts distinguish signal from armed state, verify exact one byte output publication and neighbor preservation, cancellation without signal reset, synchronization consumption, notification persistence, alias scheduling, absolute due time and signed millisecond periods. Failed replacements preserve the original deadline and period.
* Invalid output and due time spans, partial commitment, null required pointers, malformed frames, inherited leases, cleanup wrap, wrong kinds, closed handles, nonnull attributes, unsupported callbacks and wake behavior, invalid modes and negative periods fault without completion publication. Unaligned spans, overlapping inputs and outputs, a due span ending at four GiB and a BOOLEAN output at the final guest address are accepted. Handle and independent object exhaustion preserve create outputs.
* Each execution includes 100 output decommit versus scheduling races. A failed publication cannot signal the timer or complete guest registers. Five further executions of each platform fixture pass. Repetition exercises scheduling variation, not exhaustive interleaving verification.
* A test registered ordinal 229 thunk traverses runtime dispatch, arms a timer whose original handle wait is independently blocked, then closes the last alias and decommits due storage before virtual expiry. The pinned wait completes with its original stdcall cleanup; all handles, objects and waiters retire. Missing absolute origin, relative deadline overflow and stopped runtime calls preserve outputs and completion registers. Close remains available for cleanup after stop.
* No webpage changed, no browser run was performed and no real ISO or generated game function was used. No M3 exit item or P3.5 acceptance is claimed. Production export registration, Xbox error statuses, object headers, named attributes, APC and DPC behavior, a production clock driver and startup integration remain absent.

Ignored evidence: `out/dispatcher-timer-abi-before.log`, `out/dispatcher-timer-abi-tests/tests.log`, `all-tests.log`, `repeat.log`, per platform results and compiled fixtures. Public reference snapshots remain in `out/nxdk-xboxkrnl-reference.txt`, `out/nxdk-xboxkrnl-ordinals.txt` and `out/nt-timer-reference.txt`.

This extends the existing nonblocking object decoder and dispatcher locking rather than adding a second timer table or exposing internal armed membership as guest signal state. Existing deadline conversion and operation pinned lifetime remain authoritative. ISO loading and image authentication are unchanged.

## Checked dispatcher handle duplication ABI slice

`InvokeDispatcherObject` in `src/browser/dispatcher-object-abi.cpp` now decodes ordinal 197 `NtDuplicateObject`: a source handle, required target handle output and a complete 32 bit options word. The three argument Xbox signature and ordinal match the existing local nxdk declaration and export snapshots. Options zero duplicates the reference; option one additionally closes the source. This close source bit follows the existing native adapter in `src/kernel/xboxkrnl.cpp`. Other option bits explicitly fault rather than being ignored. This is a bounded supported subset, not full duplication flag compatibility or captured Xbox behavior.

`DispatcherHandles::Duplicate` accepts an optional close source request. Source resolution, target publication and source removal share one handle table lock, so a concurrent close cannot retire the source between duplication and removal. The target receives a distinct monotonically increasing identifier but shares the existing object reference. Signals, semaphore permits, timer schedules and recursive mutant ownership are not copied or reset. Pending waits remain pinned to the same object after source closure and can finish after target closure.

The existing short memory lease keeps the decoded frame and preflighted four byte target output stable across the nonblocking operation and publication. Success returns status zero with sixteen bytes of stdcall cleanup. Output may overlap captured arguments without redirecting the source or options. A new handle slot is required even for close source transfer. At capacity, invalid memory, unsupported options, an unknown source or runtime cancellation, the adapter faults without publishing completion or closing the source. These transactional internal faults are not claimed to implement NT failure status or close source behavior on a returned guest error. Production error translation remains a separate gate.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher-duplicate-abi.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Native and threaded Wasm initially rejected ordinal 197 with `unsupported dispatcher object ordinal` before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and real Emscripten pthread contracts under Node pass. The complete serial suite passes all 79 tests. The new Wasm fixture has filesystem support disabled.
* Contracts verify shared synchronization event consumption, unchanged semaphore permits, retained timer expiry, recursive mutant ownership, distinct identifiers, close source invalidation and surviving aliases. Duplication succeeds at full object capacity with an available handle slot without allocating another object.
* Null and invalid outputs, partial commitment, wrapping ranges, malformed frames, cleanup overflow, inherited leases, unsupported full word options, closed or unknown sources, handle exhaustion and stopped runtimes fault without completion publication. Handle exhaustion preserves the source even with close source requested. Unaligned spans, output overlapping captured arguments and a dword ending exactly at four GiB are supported.
* Each execution includes 100 output decommit versus transfer races and 100 concurrent close versus transfer races. Output retirement cannot lose both handles or leak an extra reference. When publication precedes output retirement, the known monotonically assigned target remains usable even though the caller has decommitted its output bytes. Concurrent close and transfer have exactly one successful source removal; the winning transfer preserves its target. Five further executions of each compiled fixture pass. Repetition exercises scheduling variation, not exhaustive interleaving verification.
* A test registered ordinal 197 thunk traverses runtime dispatch and transfers a handle already pinned by an independently blocked checked ordinal 233 wait. Signaling through the target and closing it completes that original wait with its snapshotted frame and no remaining handle, object or waiter references.
* No webpage changed, no browser run was performed and no real ISO or generated game function was used. No M3 exit item or P3.5 acceptance is claimed. Production handles, export registration, NT error statuses, additional duplication options and startup integration remain absent.

Ignored evidence: `out/dispatcher-duplicate-abi-before.log`, `out/dispatcher-duplicate-abi-tests/tests.log`, `all-tests.log`, `repeat.log`, per platform results and compiled fixtures. Existing local nxdk reference snapshots remain unchanged.

This extends the existing handle table rather than composing duplicate then close as separate unlocked operations. It preserves the existing bounded capacity and object lifetime rules while making source transfer atomic with respect to concurrent handle operations. ISO loading and image authentication are unchanged.

## Checked dispatcher object query ABI slice

`InvokeDispatcherObject` in `src/browser/dispatcher-object-abi.cpp` now decodes ordinal 209 `NtQueryEvent`, ordinal 213 `NtQueryMutant` and ordinal 214 `NtQuerySemaphore`. Each accepts a handle and a required information output pointer. These two argument Xbox signatures, ordinals and basic information field layouts match the existing local nxdk declaration and export snapshots. Timer queries remain unsupported; this slice does not invent remaining time behavior.

`Dispatcher` takes each typed snapshot under its object lock. Event queries return notification or synchronization type and current signal state. Semaphore queries return current and maximum counts. Mutant queries return the signed signal count, a one byte caller ownership flag and a one byte abandonment flag. Caller ownership compares the nonzero context thread identity to the recorded owner, not the querying host worker. Queries do not consume a signal, take a semaphore permit, recurse into a mutant or clear abandonment. Each handle query pins the reference through the snapshot, so final handle closure cannot destroy an object already resolved by that query.

The adapter snapshots the full frame, preflights the entire eight byte output and holds the existing short backing lease through publication. It serializes explicit little endian fields rather than copying a host struct. Mutant flags occupy bytes four and five; bytes six and seven are deterministically zero. Unaligned and overlapping outputs are supported. Success returns status zero with twelve bytes of stdcall cleanup. Unknown or closed handles, wrong object types, invalid outputs, malformed frames, inherited leases and runtime cancellation fault without publishing completion or a partial output. These remain internal faults, not production NT failure status translation. Output stores are protected against backing retirement, not promised atomic to an unsynchronized guest reader.

### Reproduction and observations

```sh
node --test tests/browser/dispatcher-query-abi.test.mjs
node --test --test-concurrency=1 tests/browser/*.test.mjs
```

* Tests were written first. Both native and threaded Wasm initially failed at ordinal 209 with `unsupported dispatcher object ordinal` before implementation.
* Native AddressSanitizer and UndefinedBehaviorSanitizer contracts and Emscripten pthread contracts under Node pass. The complete serial suite passes all 81 tests. Subsequent query fixture additions also pass on both platforms. The new Wasm fixture has filesystem support disabled.
* Contracts cover both event types, observational signal and semaphore state, exact semaphore limits, signed recursive mutant counts, caller versus other or zero context identity, abandonment observation and its later consumption by a genuine wait. Alias queries continue to work after source closure.
* Null, unmapped, partially committed and wrapping outputs, wrong object types, unknown and closed handles, malformed frames, cleanup overflow, inherited leases and stopped runtimes preserve completion registers and object budgets. Output overlap with captured arguments, unaligned spans and an output ending exactly at four GiB are supported. Partial output commitment never writes its valid prefix.
* Each execution queries a mutant 1,000 times while another worker acquires and releases it. Count and caller ownership always describe one coherent state. Each execution also includes 100 query versus final close races and 100 query versus output decommit races. Completion is published only for a resolved snapshot with stable backing. No query pin leaks a handle, object or waiter. Five further executions of each compiled fixture pass. This is scheduling variation, not exhaustive interleaving verification.
* A test registered ordinal 209 thunk traverses runtime dispatch and publishes the checked query frame. No production export catalog is registered.
* No webpage changed, no browser run was performed and no real ISO or generated game function was used. No M3 exit item or P3.5 acceptance is claimed. Production handles, guest object headers, NT error status translation, timer queries and startup integration remain absent.

Ignored evidence: `out/dispatcher-query-abi-before.log`, `out/dispatcher-query-abi-tests/tests.log`, `all-tests.log`, `repeat.log`, per platform results and compiled fixtures. Existing local nxdk reference snapshots remain unchanged.

## Integrated production export coverage

The current gate and approved executable provenance are recorded in [M3](03-game-boots.md#current-boot-gate-approved-manifest-reaches-export-binding). This integrated slice adds diagnostic coverage, not kernel implementations or guest execution.

* `InspectImageImports` validates the complete original import table and checked data spans without patching it. Missing function targets remain explicit ledger entries. Binding shares that preflight, holds backing stable and rejects unresolved targets before any patch.
* `story_boot` exposes coverage from its own prepared image and runtime. Reset retires that ledger with the image. The build manifest supplies diagnostic names, ABI declarations, source paths and data sizes from fingerprinted desktop declarations, including custom ordinal 8 `DbgPrint`. A declaration is never registered as a callable browser function.
* `boot-worker.mjs` still imports the trusted manifest directly. `boot-session.mjs` owns one initialization and preparation attempt, rejects overlapping or repeated starts, checks generation retirement after binding and suppresses stale export results. Tests verify initialization, preparation and binding retirement, resource reset and rejection of message supplied manifest replacement.
* Three real ISO runs under Node each report 157 distinct unresolved imports: 139 functions and 18 data exports. All have names, kinds and source paths; 140 declarations come from `src/kernel/xboxkrnl.cpp`, 17 from `src/kernel/files.cpp`. The first failure is still ordinal 277 `RtlEnterCriticalSection`. No import is patched and no guest entry executes.
* Each generation retains the prior 6,463,488 physical bytes read and 74,213,058 compact backing bytes. All three reuse one open source and finish with zero active guest workers, pending reads and staging bytes. Worker contexts are terminated by the external harness. Browser UI responsiveness and browser pthread cleanup are not accepted by this Node run.

```sh
node --test tests/browser/export-catalog.test.mjs tests/browser/image.test.mjs \
  tests/browser/boot.test.mjs tests/browser/boot-session.test.mjs \
  tests/browser/provenance.test.mjs tests/browser/inventory.test.mjs \
  tests/browser/iso-image.test.mjs tests/browser/iso-validation.test.mjs
cmake --build out/boot --target story_boot -j 6
node out/provenance-audit/run-boot.mjs
```

The targeted suite passes 29 tests, including native AddressSanitizer and UndefinedBehaviorSanitizer and Wasm image contracts. Coverage tests failed before implementation. A malformed ownership regression initially escaped structured failure with `TypeError: null is not an integer typed array.`; ownership validation now precedes atomic access and prevents module initialization on invalid requests. A subsequent failed regression identified the custom `DbgPrint` registration missing from macro enumeration; custom registration enumeration fixes it. Ignored evidence is in `out/provenance-audit/coverage-red.log`, `custom-export-red.log`, `ownership-red.log`, `coverage-tests.log`, `coverage-build.log`, `coverage-real.log` and `boot-results.json`. The local harness asserts complete classification and exact counts across all three generations. Proprietary identity and disc details remain in ignored output.

The unresolved prerequisite is a session owned production provider for those functions and initialized data, followed by thread lifecycle and TLS callback launch integration. No additional user supplied files or approvals are needed. No isolated service or callable missing export placeholder was added.

## Decision

Use a dedicated I/O worker and fixed shared mailboxes rather than full image import or callbacks on the blocked guest. The synthetic browser proof establishes independent completion and UI progress. File ranges preserve large positions without 32 bit coercion. The reversal cost is limited to the bridge and future Wasm adapter; the portable reader does not depend on guest memory or Emscripten.

Do not add an image cache yet. Zero caching gives an explicit memory bound while real access patterns are unknown. Add a separately budgeted cache only after startup traces justify it.

## Remaining gates

* P3.1: session controller, real file picker states, runtime bootstrap, diagnostic rate limits, Stop and independent watchdog.
* P3.2: exercise the checked Wasm and compact memory path with real startup callers and measure total resident memory. A real multi GB File now passes preparation, but actual runtime cancellation and responsiveness remain untested.
* P3.3: reproduced generation identity and the approved initial story metadata gate now feed the integrated preflight. Supply production providers for the complete unresolved export ledger, integrate prepared thread TLS with the real thread lifecycle and callback ordering, and support any deferred section loads exercised by startup. The metadata gate does not establish complete startup asset coverage. Validate the Windows disc adapter on its target platform.
* P3.4: guest file handles, completions and separate temporary writable drives.
* P3.5: integrate the bounded event, semaphore, recursive mutant, timer and multiple wait service with guest handles, dispatcher headers, real wait callers and guest clock semantics. Integrate the bounded event, semaphore, timer and mutant handle table and checked stack and handle array wait ABI with production registration and error statuses, preserving operation pinned scheduling, pending reference lifetime and mutant ownership beyond final close. Connect the checked signed timer deadline and millisecond period adapter to real object and export contracts, including any exercised APC and DPC behavior. Integrate the checked unnamed event, mutant, semaphore and timer create, mutate and close ABI with production registration, guest object headers and truthful error statuses; integrate checked duplication and atomic close source transfer plus coherent event, semaphore and mutant information queries, and implement named attributes and additional duplication options when exercised rather than discarding unsupported metadata. Connect context owner identity and explicit abandonment to the production thread lifecycle, preventing identity reuse or further acquisition after owner cleanup. Supply a runtime owned independent clock driver and connect the checked timeout, status and stack ABI adapters to production guest wait exports, coherent guest system time queries and session failure handling. Implement exercised DPC and APC behavior without native process globals or detached pollers. Production thread lifecycle and export coverage remain absent.
* P3.6: real boot threads reaching a genuine diagnostic graphics boundary with timed silent audio.

None of the M3 exit checklist items is claimed complete by this foundation.
