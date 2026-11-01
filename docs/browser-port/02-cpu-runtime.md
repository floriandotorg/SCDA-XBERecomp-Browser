# M2: CPU code runs correctly

Status: accepted for the observable CPU milestone. The complete retained story CPU target executes representative real routines with compact backing, concurrent allocator and span contracts, nested generated continuations and bounded owned workers. See [M2 acceptance and limits](m2-acceptance.md) and the [historical prototype evidence](m2-evidence.md). This does not execute the game entry point.

Previous: [M1: It compiles](01-compiles.md). Next: [M3: Game boot](03-game-boots.md).

## Observable milestone

Selected real generated functions execute in a browser and produce correct register and memory results. Synthetic coverage establishes the behavior needed by the rest of the translated program, including aliasing, atomics, dispatch and exception continuations.

This does not start the entire game or establish a game frame rate.

## Entry requirements

* M1 accepted, including functional override and original body linkage.
* Generated image metadata available.
* Matching XBE bytes available for any real function test that accesses image data. Pure synthetic tests may proceed without them.

## P2.1: Contract and reference tests

Tasks:

* Select small translated routines with bounded inputs and dependencies. Include at least a memory routine, an arithmetic or transform routine and a routine using indirect calls.
* Establish expected results from known instruction semantics or independently checked fixtures. Native versus Wasm agreement alone is not an oracle.
* Build a portable native ARM64 harness where feasible. Clearly separate shared implementation checks from independent expected values.
* Cover register width behavior, flags, signed overflow, shifts, direct and indirect calls, guest stack updates and x87/SSE helper edge cases.
* Define expected NaN, signed zero and rounding behavior without enabling unsafe math transformations.
* Capture the original native memory contract, including allocated ranges, aliases, alignment and page state, before replacing it.

Deliverable: deterministic tests runnable on the Mac and in Wasm.

Acceptance:

* Known bad arithmetic and memory results fail tests with the responsible guest operation identified.
* Real function fixtures list required image data and expected register and memory effects.
* Tests do not bundle proprietary code or data into public fixtures.

## P2.2: Compact guest address translation

Tasks:

* Implement a compact region translator for image data, guest system allocations, stacks and 64 MiB physical memory.
* Map the two physical windows to the same backing bytes. Preserve guest addresses rather than rewriting game pointers in image data.
* Preserve allocator, reserve, commit, decommit, zeroing and query behavior visible to guest callers. Browser backing allocation and guest page bookkeeping are separate concerns.
* Replace memory macros and kernel helpers with a consistent access layer. Inventory every raw `base + address`, `HostPtr`, `G<T>`, pointer arithmetic and address cast in the intended browser sources.
* Define bounded contiguous span access for host routines. Split copies across regions and reject invalid spans rather than returning a pointer that crosses unrelated storage.
* Preserve ordinary unaligned loads and stores, including values crossing translation boundaries. Define and test behavior for unsupported guest addresses and 32 bit address wraparound.
* Use the same translation in bulk operations and atomics. Do not fix scalar loads while leaving `memcpy`, `rep movs`, audio readers or graphics resource access on the old model.
* Add useful invalid memory diagnostics with guest address, access size and current guest context.

Deliverable: tested memory adapter without a full 4 GiB allocation or native virtual mappings.

Acceptance:

* Writes through either physical alias are visible through the other.
* Scalar, bulk and host pointer access paths agree on mapped bytes.
* Cross boundary, overlap, decommit and recommit tests pass.
* Fixed backing allocations remain within a recorded prototype budget and leave space for runtime stacks, heap and later graphics uploads.

## P2.3: Dispatch and guest exception behavior

Tasks:

* Initialize dispatch tables from statically linked metadata instead of `LoadLibraryW` and `GetProcAddress`.
* Preserve indirect guest calls, kernel thunks, host thunks and unknown target diagnostics.
* Validate continuation recognition, stack restoration and nested `X86Resume` propagation.
* Audit actual guest CRT exception paths separately. Catching `X86Resume` does not by itself prove all guest C++ exception behavior works.
* Preserve `CallGuest` register save and restore behavior and test nested calls from hooks.
* Supply a controllable clock for tests of `rdtsc` and guest timing helpers.

Deliverable: a minimal executable CPU runtime with clear service boundaries.

Acceptance:

* Mapping, hook and original body routes produce expected results.
* Nested continuations resume at the correct guest stack depth.
* Unknown targets fail loudly with guest context rather than becoming null calls.
* No Windows CRT exception forwarding is required.

## P2.4: Shared memory, atomics and worker scheduling

Tasks:

* Build the threaded Wasm configuration and precreate an appropriate worker pool before blocking work begins.
* Verify each emitted atomic width, compare exchange, exchange and locked arithmetic operation against shared backing storage.
* Determine whether the emitted code or actual game uses misaligned atomic addresses. WebAssembly atomics require alignment; implement a coherent fallback or stop with a measured unsupported case.
* Ensure any fallback coordinates with ordinary aligned atomic accesses to the same bytes. An isolated lock around only one code path is insufficient.
* Test atomic accesses through both physical aliases with competing workers.
* Verify worker completion, cancellation and bounded waits without blocking the browser UI thread.
* Choose a stack policy based on observed nesting, with diagnostic overflow checks. Do not copy 64 MiB native stack reservations to every worker without measurement.

Deliverable: concurrency tests and a documented worker and stack budget.

Acceptance:

* Repeated concurrency runs preserve the expected totals and compare exchange results.
* Missing isolation or insufficient worker capacity gives a diagnostic instead of a hang.
* A blocked guest worker cannot prevent the context responsible for completing its request from running.

## P2.5: Feasibility measurements

Tasks:

* Benchmark translation, bulk transfers, function dispatch and representative generated arithmetic separately.
* Include memory heavy and branch heavy real routines, not only a favorable scalar loop.
* Record native and Wasm timings, fixed runtime overhead, peak memory and stack high water marks.
* Check optimized results against the same correctness fixtures.
* Identify the dominant translation cost and evaluate one focused alternative only if measurements justify it.

Deliverable: a feasibility report stating which CPU behaviors are proven, which remain unknown and what blocks boot.

Acceptance:

* All correctness suites pass in a real browser with the selected release flags.
* Performance and memory results are reproducible on the named Mac and browser.
* The report does not convert a microbenchmark ratio into an asserted game FPS.

## Exit checklist

* [x] P2.1 through P2.5 accepted.
* [x] Representative real generated functions run correctly.
* [x] Guest memory aliases, boundary accesses and allocation bookkeeping are covered.
* [x] Indirect calls, overrides, original bodies and continuations pass.
* [x] Threaded atomic tests pass without UI blocking.
* [x] Resource budgets and unresolved CPU behavior are documented.

## Stop conditions

Do not proceed with a partial alias implementation, silent invalid memory accesses, an unbounded stack requirement or atomic behavior that differs depending on which helper is used. Revisit memory layout before investing in graphics if its measured overhead is clearly untenable.
