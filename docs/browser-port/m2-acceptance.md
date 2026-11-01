# M2 CPU runtime acceptance

Status: accepted for the observable M2 CPU milestone. M3 has not started. This is not a boot, menu, game performance or complete x86 emulation claim.

The [initial prototype evidence](m2-evidence.md) is historical. The work below closes its CPU target integration, allocation concurrency, metadata, nested continuation, worker lifecycle and measurement gates. Native filesystem, audio, graphics and kernel service consumers are not linked into the CPU target and must not receive its opaque memory handle without adaptation.

## Reproduce

Machine: Apple Silicon `MacBookPro18,4`, ARM64, macOS `27.0.1`, 64 GiB memory. Native compiler: Apple Clang `21.0.0`. Wasm compiler: pinned Emscripten `6.0.13-git`. Node: `v26.5.0`. Browser: agent-browser Chromium `151.0.0.0` with cross origin isolation enabled.

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
node --test tests/browser/*.test.mjs
node tools/browser/cpu-inventory.mjs
node tools/browser/serve.mjs out 8086
```

The suite builds the full retained CPU target incrementally using Ninja. It does not run the recompiler, read an XBE or ISO, initialize Windows services or modify generated inputs.

Open these paths under `http://127.0.0.1:8086`:

* `/full-cpu/cpu-web/index.html`: complete story metadata and CPU contracts.
* `/cpu-tests/index.html`: the same CPU contracts against selected extracted bodies.
* `/allocation-tests/index.html`: the actual shared allocator and concurrent callers.
* `/memory-concurrency/index.html`: backing transitions and host span lifetime.
* `/arithmetic-tests/index.html`: emitted arithmetic and the independent instruction oracle.

The full target can also be built separately:

```sh
emcmake cmake -S . -B out/full-cpu -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBROWSER_BUILD=ON -DBROWSER_FULL_CPU=ON
cmake --build out/full-cpu --target cpu_contracts --parallel 6
node out/full-cpu/cpu-web/run.mjs
```

The test suite writes `run.mjs`. All pages report errors before module creation when isolation or `SharedArrayBuffer` is unavailable. CPU pages retain `guestHeartbeats`, counted only between the guest work begin and completion messages, not during module loading.

## Inputs and build boundary

The tested full target manifest identity is:

```text
6548490a5ae432b4b0d5b241551d4d81b11f1632829e134666a4a4562e790260
```

`out/full-cpu/tools/browser/input-manifest.json` records generated inputs, configuration, the CPU runtime, adapters and harness. Its compatibility identifier is `wasm-compact-leased-v1`. Unchanged manifests and headers keep their timestamps, so reconfiguration does not force another full link. M1 retains its separate diagnostic ABI and placeholders.

The target compiles all 128 story partitions and the original mapping source with compact memory access. It initializes 33,172 real mappings and all four original continuation entries. Four synthetic dispatch targets and one synthetic continuation are added by the harness, producing:

```text
full-metadata-ok mappings=33176 continuations=5
cpu-runtime-ok backing=69452344
```

`prepare-cpu.mjs` writes adapted copies only under the ignored build directory. `adapt-memory.mjs` scopes all 112 numeric generated host pointer expressions with backing leases. The 18,389 XMM pointer expressions remain value proxies that copy all 16 bytes through the same translator. Unknown pointer types or unscoped forms stop preparation. No original generated file is edited.

`adapt-arithmetic.mjs` fixes carry rotation overflow flags in build copies. The original emitted `rcr` fails the independent oracle at `rcr32 value=00000000 count=1 overflow`. The adapter sets defined single bit overflow flags for `rcr` and `rcl`, preserves zero count behavior and rejects unknown forms. The test suite retains the original failure as an expected regression case.

The generation XBE hash remains absent. Every executed fixture uses invented state instead of original image data. This is sufficient for the selected M2 routines, not permission to execute the guest entry point. M3 still requires matching disc and executable identity validation.

## P2.1 accepted: independent CPU contracts

Eleven real generated bodies execute with explicit synthetic dependencies:

* The original six routines listed in the prototype evidence retain their register, sparse store, multiplication, tree, indirect call and exception frame expectations.
* `sub_002CDA25`, partition 69, adds the inner real generated exception frame. A controlled `sub_002CD410` override requests a synthetic continuation. Returns through the inner override distinguish an inner catch from propagation to the outer frame. Tests cover a target below the inner frame, between the frames and at the outer frame boundary.
* `sub_00452604`, partition 109, performs a locked increment. Inputs zero, signed maximum and unsigned maximum check the returned count, wraparound, physical alias bytes and eight byte guest stack increment.
* `sub_00365CA1`, partition 84, pops two words, exchanges a stack word atomically and tail dispatches to a synthetic callback. The replaced stack word, callback result and twelve byte stack increment are checked independently.
* `sub_00452449`, partition 109, replaces the x87 top with a ten byte value read at `0x005DB9B0`. Those bytes are invented to encode 1.5, not original image data. The top index, neighboring register and return stack effect are checked.
* `sub_00309660`, partition 75, copies a variable byte count using real `rep movsd` and `rep movsb` sequences. Unaligned source and destination, zero through 1 MiB plus three bytes, untouched surrounding bytes, restored `esi` and `edi`, returned length and guest stack are checked against an independent byte pattern.

A separate fixture extracts 33 actual emitted instruction blocks without publishing their bodies. It checks 185,184 register and flag cases across 8, 16 and 32 bit operands. Coverage includes add, subtract, carry and borrow, negation, increment, decrement, signed overflow, variable shifts, double shifts and rotates. Undefined flags are not treated as an oracle. Immediate and variable count cases include masked counts, zero counts and preserved upper register bits.

Existing synthetic contracts retain scalar and vector boundary access, proxy assignment, sequential overlapping string operations, physical aliases, floating rounding, NaNs, signed zero and integer indefinite conversion. Forward and backward string faults also preserve completed bytes and advance registers only for the completed element. Deliberate sparse store and multiply mutations still fail with the responsible guest operation.

## P2.2 accepted: concurrent compact backing

Every public backing operation locks the same recursive memory mutex before inspecting regions, commitment or bytes. Reads, writes, copies and leased atomics therefore coordinate with mapping, commitment, decommit and release. Both physical windows retain one storage object and commitment map.

A raw host span requires `Memory::Borrow()`. Its lease holds the memory lock until the consuming scope exits. Another thread cannot release or relocate backing beneath that pointer. Reentrant structural mutation throws before changing storage. Generated numeric pointers receive one lease per emitted instruction block; spans must not escape that scope.

The lock order is allocator bookkeeping before memory backing. Shared allocator APIs reject entry while the calling thread holds a backing lease, before acquiring the allocator mutex. Guest waits and joins also reject backing leases. This prevents completion workers or allocating threads from waiting for memory retained by their waiter.

Tests cover a borrowed span racing release, mutation attempts inside a lease, 2,000 mapping transitions alongside two alias counters, and 1,024 concurrent shared allocator allocation and release cycles with host span access. Totals remain 20,000, released allocations return backing budget, and each allocation retains its own bytes. Native ThreadSanitizer passes the CPU, allocator and backing concurrency harnesses.

The source inventory contains no direct flat address addition, `HostPtr`, `G<T>` or address cast in generated inputs. Generated bulk helpers operate on local register and lane buffers. Native helpers in `kernel.h`, files, audio and graphics remain excluded from the intended CPU sources. Their future inclusion requires an access layer audit, not a definition change that silently treats `base` as a flat byte array.

## P2.3 accepted: complete metadata and nested continuations

The full target constructs dispatch from retained static metadata, validates continuation bodies and runs the same mapping, strong override, original body, host thunk, kernel route and unknown target checks as the small harness. Two nested actual generated catch frames now select the owning guest frame. Unhandled continuations retain their requested guest stack rather than receiving normal return register restoration.

Clock state is atomic and bound through a scoped thread local `ClockBinding`. Nested bindings restore the previous runtime. Owned worker entries install their binding automatically; an unbound clock remains a diagnostic. This is the controllable M2 clock, not a production elapsed time or pause implementation.

### Guest CRT audit

There are two different exception mechanisms:

* Generated C++ catches use host Wasm exceptions for `X86Resume`. The complete target links and executes these without the Windows `vcruntime140` export forwarding in `src/cpu/dispatch.cpp`.
* Guest C++ throwing and destructor unwinding still execute guest CRT code and call kernel services. `src/kernel/xboxkrnl.cpp` ordinals 302 and 312 walk the guest `fs:[0]` handler chain, construct guest scratch context and call guest handlers. `tools/recomp/analysis.cpp::EhContinuations` discovers catch and unwind targets from guest FuncInfo metadata.

The second path is audited but not claimed as executed by these continuation fixtures. Those kernel ordinals remain explicit unavailable service faults in the CPU target. Before real startup reaches them, M3 must port their memory access, bound handler traversal and validate real exception records. No fake guest CRT success was added to obtain M2 acceptance.

## P2.4 accepted: bounded workers and cancellation

`Runtime::Launch` owns worker threads and their contexts. At most two additional guest workers are active beside the proxied harness worker. A third request fails before creating another worker. Completed slots can be reused. `Join` retains typed failures, worker faults cancel sibling waits, and `Stop` cancels then joins every owned thread. Joining from an owned worker or while holding backing is a diagnostic instead of a deadlock.

Waits use a condition variable, monotonic deadline and sequence counter. Completion before a wait is not lost. Negative or greater than 30 second budgets are rejected. Cancellation wakes outstanding waits and prevents further dispatch, continuation resume, kernel calls, generated backedges and string iterations. Stop is cooperative at these boundaries; arbitrary blocking native hosts are not supported by this CPU harness. M3 still needs the browser controller's independent watchdog and worker termination boundary.

The pinned toolchain can grow its pthread pool under `PROXY_TO_PTHREAD` even with `PTHREAD_POOL_SIZE_STRICT=2`. An experiment exhausted the nominal pool without receiving `EAGAIN`. The runtime therefore enforces its own capacity instead of treating that setting as proof of a bound. Real capacity, repeated start and stop, worker entry, slot reuse, self join, failure retention, sibling cancellation and independent completion are tested.

Three consecutive full browser runs completed with exit zero and 17, 16 and 15 UI timer callbacks during guest work. Three separate backing concurrency browser runs also passed. Missing isolation was tested through a server without isolation headers: the page reported `CPU error: cross origin isolation required` and made no Wasm request.

## P2.5 accepted: measurements and budgets

Release compilation uses `-O3`, pthreads, Wasm exceptions and no unsafe math flags. Threaded targets precreate three workers, use a 1 MiB main stack setting, 256 KiB default worker stacks, fixed 128 MiB linear memory and `STACK_OVERFLOW_CHECK=2`.

Observed resource values:

* Current guest backing including commitment maps: 69,452,344 bytes. Physical backing is counted once.
* Backing cap: 112 MiB. Snapshot copy scratch cap: 8 MiB.
* Actual Wasm memory size and maximum: both 134,217,728 bytes. No growth is enabled.
* Main fixture sampled stack peak: 6,752 bytes against the observed 1,048,576 byte stack capacity.
* A four frame synthetic nesting probe increases the sampled peak to 39,456 bytes.
* The runtime with owned workers observes a minimum stack capacity of 262,144 bytes and maximum sampled use of 7,152 bytes across its launch and worker checkpoints.
* Maximum owned worker stop times across the three browser runs: 0.075, 0.070 and 0.060 milliseconds. The correctness deadline is two seconds, not those measured timings.
* Native selected harness maximum RSS: 73,629,696 bytes using macOS `/usr/bin/time -l`.
* Complete Wasm artifact: approximately 48 MiB. This contains locally built translated code and is not a public distribution artifact.
* A fresh dedicated browser session process tree had sampled aggregate RSS of 1,503,952,896 bytes before navigation and a peak of 2,228,191,232 bytes during three full target runs. The measurement includes the automation daemon and browser descendants. Shared resident pages can be counted more than once. This is not a private heap size or a proof of a precise instantaneous maximum.

Stack figures are sampled at runtime checkpoints, not an exhaustive high water measurement of every possible translated call graph. Stack pointer overflow checks remain enabled. Future boot threads need new measurements; these results do not justify native 64 MiB reservations per worker.

The first full browser run recorded nanoseconds per operation:

* Validated scalar store: 27.70.
* Bounded 64 KiB snapshot copy: 4,570.00.
* Complete table dispatch: 50.50.
* Real sparse stores: 111.60.
* Real signed multiplication: 24.25.
* Real tree branches: 188.65.
* Real 1 MiB string copy: 16,078,421.33.

The remaining runs varied, notably full table dispatch at 39.65 and 38.25 nanoseconds. These are short feasibility observations without statistical confidence, not game FPS estimates.

### Focused alternative

Per access validation, lookup and synchronization dominate simple stores. Per element translation and cancellation checks also make the real string copy much slower than a bounded bulk transfer. A single leased contiguous span, with the same scalar output checked against the translated loop, measured 0.40 nanoseconds per store in the first run and 0.20 and 0.25 subsequently. This loop can be vectorized and amortizes all checks; it is not an equivalent individual translated instruction benchmark.

Do not adopt that as a global generated pointer cache. Holding a lease across arbitrary calls would block allocation and completion. Replacing sequential overlapping string operations with `memmove` would also change guest behavior and fault progress. The safe per access path is retained until real boot traces identify a bounded consumer worth batching. No source layout migration is needed to reverse this decision.

## Validation artifacts

Nineteen automated tests pass. Native AddressSanitizer and UndefinedBehaviorSanitizer pass CPU, allocator, backing concurrency and emitted arithmetic fixtures. Native ThreadSanitizer passes CPU worker lifecycle, allocator concurrency and backing transitions. All five correctness pages pass in a real isolated Chromium session with no browser errors.

Ignored local evidence includes:

* `out/m2-final-tests.log`.
* `out/full-cpu/node-results.txt`, `browser-results-1.json` through `browser-results-3.json`, and `browser-memory.json`.
* `out/cpu-tests`, `out/allocation-tests`, `out/memory-concurrency` and `out/arithmetic-tests` native, Wasm, browser and sanitizer results.
* `out/cpu-tests/no-isolation-results.json` and `no-isolation-network.txt`.
* `out/cpu-tests/memory-inventory.json`.

Reproduce sanitizer coverage using the source lists in the test runners with `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer`, or separately `-fsanitize=thread` for the three threaded native harnesses. Do not combine ThreadSanitizer with AddressSanitizer.

For browser RSS sampling, get the background PID from `agent-browser session info --json` in the named test session, then run this while navigating the full CPU page:

```sh
node tools/browser/measure-memory.mjs SESSION_PID 30000 > out/full-cpu/browser-memory.json
```

## Explicit limits after M2

* The game entry point, ISO loading, kernel thread objects, file reads, graphics and audio have not run. M3 remains not started.
* The compact ABI belongs to the explicit CPU targets and shared allocator test backend. Native Windows consumers are unchanged and were not executed on this Mac.
* Misaligned host atomic pointers are unsupported, with no incoherent fallback. The complete metadata harness reaches the actual emitted `0x004DF229` exchange using invented aligned object state and confirms its derived misaligned address faults before exchanging bytes. This proves the emitted case can trap; it does not establish that real startup reaches it or uses that object alignment.
* Unmasked floating exceptions, floating status flags and extended precision beyond the original double based x87 approximation are not established by these tests. Neither is every possible emitted instruction input. M2 accepts representative real routines plus the documented contracts, not universal CPU correctness.
* Original image provenance and actual startup memory and stack demands remain M3 dependencies. No placeholder reports successful file, GPU or guest CRT behavior.
