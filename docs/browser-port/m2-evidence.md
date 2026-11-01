# M2 CPU runtime prototype evidence

Status: historical prototype record, superseded by [M2 acceptance and limits](m2-acceptance.md). The observations and open gates below describe the initial checkpoint, not the current CPU target. At that checkpoint six real generated routines and isolated shared allocator contracts ran; milestone acceptance had not been established.

## Reproduce

Use the M1 pinned Emscripten installation on the Apple Silicon Mac:

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
node --test tests/browser/*.test.mjs
node tools/browser/cpu-inventory.mjs
node tools/browser/serve.mjs out/cpu-tests 8084
```

Open `http://localhost:8084/`. Expect `cpu-runtime-ok backing=67342389`. The page retains timings above that marker. `window.cpuEvidence` exposes output, isolation, UI timer count and exit status. An isolation failure is visible before module creation; the page reports a 30 second completion timeout. Node executions also have a 30 second process timeout.

The runner builds native ARM64 and threaded Wasm at `-O3`, without unsafe math flags. Wasm uses `-fwasm-exceptions`, a precreated pool of three workers, `PROXY_TO_PTHREAD` and strict pool exhaustion diagnostics. Blocking guest work does not run on the UI thread. No XBE, ISO, database or Windows service is opened.

Only selected function bodies are extracted from existing local generated files into ignored `out/cpu-tests/fixtures.cpp`. The runner does not modify generated inputs, regenerate translations or publish those bodies. Public fixtures contain synthetic state and independently checked expectations, not game section bytes. Native uses weak forwarding functions because Darwin does not support Clang's ELF style alias attribute. Wasm retains M1 weak aliases. Native and Wasm timing comparisons include that linkage difference.

Additional native diagnostics:

```sh
clang++ -std=c++20 -O1 -g -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -DX86_PORTABLE_RUNTIME -DX86_COMPACT_MEMORY \
  -Isrc tests/browser/cpu.cpp src/browser/cpu_runtime.cpp \
  out/cpu-tests/fixtures.cpp -o out/cpu-tests/cpu-sanitized
out/cpu-tests/cpu-sanitized
```

## Implementation boundary

`src/browser/cpu_runtime.{h,cpp}` supplies compact backing, dispatch, typed faults, test clocks and CPU feature policy. `X86_COMPACT_MEMORY` selects its accessors in `src/cpu/x86_ops.h`. In that ABI, `base` is an opaque `Memory` handle, never a guest byte buffer. Mixing it with the native kernel's `g_base`, `G<T>` or `HostPtr` is invalid. Only the isolated CPU and allocator test runners enable this ABI today. Native Windows and the M1 diagnostic target keep their existing branches.

Each region has bounded backing and separate page commitment bits. Both physical windows share one backing object and one commitment map. Ordinary scalar and vector values can cross adjacent regions; host spans cannot cross unrelated backing. Access validation precedes writes. Page operations are idempotent, preserve already committed data and zero pages on recommit. Physical pages stay committed, matching the native contract. Mapping and page transitions require quiescent workers; returned host spans must not survive a page transition. Concurrent remapping and allocator reuse are not proven.

`Memory::Copy` validates both ranges and snapshots before writing, preserving overlap even through physical aliases. Its scratch allocation is bounded at 8 MiB. Larger requests fail explicitly. `rep movs` instead preserves sequential guest instruction semantics, including propagating bytes during overlapping copies. Its guest registers advance only after each completed element. `rep stos` uses the same translated writes. Neither compact helper multiplies a guest element count into a potentially wrapped bulk byte count.

Dispatch receives static metadata spans, sorts mappings and rejects duplicate, null or reserved entries. It handles mapping bodies, explicitly registered host thunks, kernel thunks and recognized continuation targets. Unknown targets and unavailable ordinals throw diagnostics containing address, size, guest stack and last indirect call. `Call` restores the eight integer registers on normal return, while exceptions propagate with the guest stack depth unchanged, matching native `CallGuest`. Resume validates its continuation target and restores the recorded stack before entering its body. This is not a proof of guest CRT unwinding.

The controllable `rdtsc` source is explicitly installed per runtime thread and reports a diagnostic if missing. Its units are guest ticks, not milliseconds. Production conversion from elapsed time to the native 733,333,333 Hz contract is not installed. CPUID preserves the native family and feature responses. Privileged instructions fail instead of silently succeeding.

## P2.1 fixture contracts

All guest stack effects below include the translated return instruction. Each fixture has a mapped, committed synthetic stack and no immutable image byte requirement.

* `sub_00011000`, partition 0: sparse object initialization at an unaligned `ecx`. Expected zero dwords at offsets 0, 4, 0x30, 0x38 and 0x3C; other bytes remain 0xAA. Expected `eax` equals the original object address, `ecx` equals zero and `esp` advances by four.
* `sub_0016E504`, partition 34: signed multiplication of `[ecx+0x1C]` by 28. Input negative one produces `eax=0xFFFFFFE4`; `esp` advances by four. This is checked against instruction arithmetic, not native agreement.
* `sub_003E0540`, partition 98: vtable tail dispatch through slot 0x40. Synthetic object and vtable point to a registered callback. Expected callback register effects and one return stack increment.
* `sub_0019B695`, partition 38: add 0x248 to the receiver, then tail dispatch through slot 0x14. Expected synthetic callback result and preserved guest pointer arithmetic.
* `sub_00011013`, partition 0: signed tree membership search. The mutable root at `0x005E3E00` points to a synthetic three node tree containing 10, 20 and 30. Inputs signed minimum, 9, 31 and signed maximum are absent; 10, 20 and 30 are present. `esi` is restored and `esp` advances by four. The root variable is supplied state, not original image data or proof of XBE provenance.

Mutation tests change one local extracted store or multiplication constant. Both fail with the responsible guest operation in the error. Their failing execution is the test's expected result. Shared native and Wasm implementations are checked against these independent expectations.

Synthetic coverage includes physical aliases, scalar and SSE vector region crossings, ten byte x87 loads and stores across regions, proxy assignment, bulk overlap, sequential string overlap in both directions, invalid spans, address wraparound, decommit and zeroed recommit. Dispatch checks include nested hook calls, integer register restoration, host thunk reuse, kernel routes, malformed thunk addresses, continuation payloads, stack restoration and propagation through a nested call.

Floating conversion fixtures cover ties to even, downward rounding, integer indefinite results, NaN operand selection and signed zero selection. They exposed that the old SSE conversion ignored MXCSR. The compact branch now reads guest MXCSR rounding bits; x87 nearest rounding is independent of host rounding mode. Floating status exceptions, extended precision, all emitted shifts and all register flag combinations remain unproven.

## Original native memory contract

Captured from `src/kernel/memory.{h,cpp}` before adding compact backing. The allocator bookkeeping is now shared with an opt in compact backend in `memory.cpp`. Native platform backing remains separate. Shared safety fixes reject overflowing sizes, invalid alignment, commits outside a physical alias, overlapping image reservations and decommit ranges outside their reservation; failed backing allocation no longer reports success. No Windows execution is claimed.

* Native backing reserves the guest 4 GiB address space. Linux style mappings and Windows virtual memory APIs supply commitment, with two shared physical mappings.
* Guest page size is 4096. Physical windows are `0x80000000..0x83FFFFFF` and `0xF0000000..0xF3FFFFFF`. The first physical MiB is kernel data; contiguous allocation excludes it. Physical pages are always reported committed and native decommit skips them.
* User virtual addresses range from `0x00010000` to exclusive `0x7FFE0000`. Image reservation rounds outward to 64 KiB and removes that interval from the user free list.
* System allocation uses `0xB0000000..0xBFFFFFFF`, minimum alignment 16, size rounded to 16 and explicit zeroing. System free returns the range without decommitting pages.
* Stack allocation uses `0xD0000000..0xDFFFFFFF`, size and alignment rounded to 64 KiB, plus an uncommitted 64 KiB interval below the stack. It returns the stack top, not its base.
* Contiguous allocation rounds to pages, enforces physical lower and upper bounds, uses at least page alignment and explicitly zeroes the allocation. The aliases do not rewrite guest pointers.
* Virtual reserve rounds outward to 64 KiB; commit rounds outward to pages. Commit with a null base also reserves. The allocator supports first fit and top down placement, tracks requested protection and returns NTSTATUS results.
* Virtual release removes the reservation and decommits it. Query reports the consecutive run of equal commitment state starting at the queried page, allocation base and protection, plus free range state outside reservations.

The prototype's `Map` alone is not a replacement for those allocator APIs. The new allocator harness exercises the actual shared APIs against compact backing, including reserve without backing, lazy commit, query runs, release, reuse and allocation failure. Production integration and concurrent mapping safety remain blocking P2.2 tasks.

## Source inventory

`tools/browser/cpu-inventory.mjs` records every matched raw address, host pointer, guest reference, address cast, bulk operation and atomic site under `src/` and `generated/game/`. The ignored JSON includes paths, line numbers, source text and whether each site belongs to the CPU prototype or a native service not yet ported. This is a review inventory, not a claim that a regex establishes pointer safety.

Observed 254 source files and 19,606 matched lines. Generated atomics contain six fetch additions, 21 compare exchanges, 54 exchanges, 43 fences and two atomic loads. Direct atomic pointer expressions contain 49 dword sites and 30 byte sites; two additional byte arithmetic loops first bind a local pointer. These counts are not execution or reachability counts.

Remaining raw paths include `src/kernel/kernel.h`, native allocator zeroing, file buffers, graphics and audio consumers. Those sources are deliberately not linked into this CPU harness. Generated x87 80 bit loads and stores pass a byte pointer to a helper that consumes ten bytes. A failing split load fixture exposed that width mismatch. Compact wrappers now recover the pointer's guest location, validate all ten bytes and transfer through a bounded local buffer. Reverse lookup canonicalizes physical aliases to the first window without changing guest stored pointers. Ten byte memory safety is covered; extended precision arithmetic is still approximated by the original double based helpers.

## P2.4 concurrency and budgets

Two competing workers each perform 10,000 locked additions through opposite physical aliases. Expected total is 20,000. Tests also exercise successful and failed compare exchange, exchange, subtraction, OR, AND and XOR for widths 8, 16, 32 and 64. The emitted game currently uses only byte and dword pointer widths in the inventory.

Misaligned atomic pointers throw before the compiler intrinsic. There is no private fallback lock, so aligned accesses cannot bypass an incoherent fallback. A synthetic misaligned dword is a measured unsupported case, not a successful emulation. Actual game alignment and reachability still need measurement. An emitted dword exchange using `ctx.esi + 0x1E` demonstrates why address expression inspection alone cannot prove alignment.

The harness uses one guest execution worker and two competitors in a pool of three. Module stack configuration is 1 MiB and default pthread stacks are 256 KiB each. Wasm initial memory is 128 MiB without memory growth. Compact backing and commitment maps have a 112 MiB cap; copy scratch has a separate 8 MiB cap. The actual fixture backing is 67,342,389 bytes, including commitment maps and the physical allocation counted once. Vector capacity, allocator overhead, compiled code, JS objects and browser processes are not included in that number.

These are prototype limits, not a measured game stack policy. Stack high water marks, cancellation, bounded guest waits, request completion services and peak browser memory remain open. Pool strictness and page timeouts diagnose harness exhaustion or noncompletion; they are not production deadlock recovery.

## P2.5 observations

Machine and toolchain match M1: Apple Silicon ARM64, macOS 27.0.1, 64 GiB memory, Emscripten `6.0.13-git`, Node v26.5.0. The portable reference used Apple Clang 21.0.0. Browser testing used agent-browser Chromium 151 with cross origin isolation enabled.

Nine automated tests pass, including the existing five M1 tests. The native AddressSanitizer and UndefinedBehaviorSanitizer run passes. Three consecutive browser runs completed with exit status zero, expected atomic totals and no browser errors. Five UI timer callbacks ran during each execution, confirming that the page event loop remained responsive during guest work.

One browser run recorded the following nanoseconds per operation:

* Translated scalar store: 28.55.
* Bounded 64 KiB bulk copy: 5,232.00.
* Function dispatch: 17.60.
* Real sparse object stores: 90.70.
* Real memory load and signed multiplication: 23.20.
* Real signed tree branches: 163.45.

Subsequent runs varied, particularly dispatch as compilation warmed. Native and Node Wasm timings are written to `out/cpu-tests/native-results.txt` and `wasm-results.txt`. Browser timings stay in `window.cpuEvidence`. These short observations have no warmup control or statistical confidence and do not estimate game FPS. Bulk timings include snapshot allocation. Scalar translation currently performs a region lookup during validation and again during access; optimize only after a representative workload identifies its cost.

## Remaining acceptance gates at the initial checkpoint

* P2.1 active: broaden real arithmetic, shift and flag fixtures; audit extended precision and floating exception behavior. Strong override and original body routing now pass inside the compact runtime.
* P2.2 blocked from full target integration: shared allocation and query APIs pass isolated contracts, but concurrent allocation and returned span lifetime are not proven. Adapt every native host consumer and measure full runtime backing demands before enabling compact backing in the full target.
* P2.3 active: initialize from the complete retained metadata, test nested generated continuation catches and audit guest CRT exception paths. One actual generated catch now passes below, at and above its frame boundary. The harness still supplies selected static mappings.
* P2.4 active: establish actual atomic alignment, cancellation, bounded waits and measured stack policy. Do not enable an incoherent misalignment fallback.
* P2.5 active: measure larger real memory workloads, runtime initialization, peak memory and stack use, then rerun the complete accepted suite with the selected release target.

## Allocator and compact dispatch follow up

`tests/browser/allocation.test.mjs` builds `tests/browser/allocation.cpp`, the actual shared `src/kernel/memory.cpp` allocator and the compact CPU runtime for native ARM64 and threaded Wasm. Run it through the full test command above. Serve `out/allocation-tests` to run the same executable in a browser. It reports `allocation-contracts-ok backing=67260449`; `window.allocationEvidence` retains isolation, output, exit status and UI heartbeat count.

The tests were written before the backend. Initial native and Wasm builds failed on the native virtual mapping dependencies. Subsequent regression fixtures failed on overflowing allocation sizes, overlapping image reservations and commits crossing either physical alias before their fixes. All eleven automated browser test cases now pass. Both CPU and allocator native AddressSanitizer and UndefinedBehaviorSanitizer executions pass. Final allocator and CPU browser runs completed with isolation enabled, exit zero and no browser errors. Their UI heartbeats advanced 17 and five times respectively. Ignored `out/allocation-tests/browser-results.json` and `out/cpu-tests/browser-results.json` retain these observations; their backing byte totals were 67,260,449 and 67,342,389.

Reproduce allocator sanitizer coverage with:

```sh
clang++ -std=c++20 -O1 -g -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -DX86_PORTABLE_RUNTIME -DX86_COMPACT_MEMORY \
  -Isrc tests/browser/allocation.cpp src/kernel/memory.cpp \
  src/browser/cpu_runtime.cpp -o out/allocation-tests/allocation-sanitized
out/allocation-tests/allocation-sanitized
```

The new allocator contracts cover a 512 MiB reservation without backing growth, page rounding, idempotent commit, decommit confinement, zeroed recommit, address overflow, release returning backing budget, first fit and top down reuse, system alignment and zeroing, stack guards, physical aliases, image bounds, allocation statistics and failure rollback. Backing budget exhaustion returns `STATUS_NO_MEMORY` with unchanged caller outputs and no published reservation. Metadata allocation failure beyond the explicit backing budget is not exhaustively fault injected.

`Memory::Ensure` prepares all missing backing before publishing it. `Discard` releases complete owned regions and rejects partial ownership or shared physical backing. Released virtual and stack allocations return their backing budget. Decommitted pages retain backing until release; system free retains committed pages, matching the native contract. Backing remains capped at 112 MiB, independent of reserved guest address space. All mapping, page transitions and release still require quiescent workers. These APIs do not yet establish safe allocation while another guest worker accesses the region index or holds a host span.

The CPU harness now extracts `sub_002CDA10` from partition 69 alongside the original five routines. Synthetic TLS and stack bytes establish its guest exception record. An explicit test override of its tail callee throws `X86Resume`: the real generated catch resumes a target below its frame and propagates targets at or above that frame. The tail callee is not its original game body; this tests generated catch mechanics, not guest CRT unwinding or the complete function's gameplay behavior. A strong `sub_0016E504` override calls its original generated body, adjusts the result and is reached through both direct and mapped dispatch. Duplicate mappings, null bodies and continuations without bodies are rejected. The arithmetic benchmark continues to call the original body, not the override.

No M2 exit checkbox was accepted at this initial checkpoint. The subsequent [M2 acceptance record](m2-acceptance.md) documents the complete CPU target, concurrent backing leases, nested catches, owned workers and measurements that close those gates. The compact ABI remains limited to explicit CPU targets and the shared allocator test backend. No boot, game frame rate or universal CPU correctness is claimed.
