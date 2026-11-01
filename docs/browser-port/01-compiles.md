# M1: It compiles

Status: accepted for diagnostic compilation. P1.1 through P1.4 validated on macOS and Chromium. See [procedures and evidence](m1-evidence.md). CPU correctness and real boot are not claimed; original generation provenance remains a boot blocker.

Previous: [Roadmap](README.md). Next: [M2: CPU runtime](02-cpu-runtime.md).

## Observable milestone

All generated story C++ compiles to Wasm objects. A diagnostic browser module links the complete function map and instantiates in a browser. It reports its build identity without entering the game.

This proves compiler and linker feasibility, not CPU correctness, game boot or rendering.

## Entry requirements

* `generated/game/x86_recomp.*.cpp`, `x86_func_mapping.cpp`, `x86_recomp_shared.h` and `image_symbols.h` exist.
* Generated sources and configuration describe the same `default.xbe` image.
* Native macOS C++ tools, CMake, Ninja and Node are available.
* No assumption that the Windows DLL, executable or PE import libraries exist.

## P1.1: Inventory and isolated build

Tasks:

* Record a hash manifest of generated inputs, partition count, code range and image identity metadata. Mark missing provenance as a blocker to real boot, not necessarily to compilation experiments.
* Install or locate Emscripten, pin the selected version and document activation on macOS.
* Add an opt in browser build with its own output directory and explicit source lists. Exclude `src/versus/`, Winsock, XAudio2, Win32, DX12, resource compilation and local `clang.exe` execution.
* Treat generated files as existing inputs. The browser configure step must not run `xbe_recomp`, `lld-link.exe`, the native `scda_image` function or a SHA calculation against a nonexistent XBE.
* Compile a small diagnostic host before including generated sources. Use the Emscripten standard library rather than carrying Windows freestanding headers and CRT forwarding into Wasm without review.
* Create a minimal local test server with COOP and COEP headers, correct Wasm MIME type and no asset serving beyond a configured project directory.

Deliverable: documented configure, build and serve procedures for an empty browser diagnostic target.

Acceptance:

* A fresh browser build configures on the Mac without Windows SDK files.
* The diagnostic page instantiates Wasm and reports the pinned build identity.
* The page reports whether cross origin isolation, shared memory and required browser capabilities are present.

## P1.2: Portable generated symbols

Tasks:

* Inventory `__declspec`, `__pragma`, PE aliases, `X86_GAME_CODE`, exports and imported runtime functions.
* Define browser versions of the host API and function declaration macros without changing guest calling semantics.
* Preserve two identities for an overridden function: the hookable public symbol and the original `__imp__` body.
* Choose supported Wasm weak aliases or reproducibly generated forwarding definitions. Prove replacement and original body calls using synthetic functions before compiling every partition.
* If adapters are generated, derive them from parsed declarations or a validated manifest; do not use unchecked global textual replacement of generated C++.
* Retain original generated artifacts unchanged. Record which compatibility header and adapter version the build uses.

Deliverable: a small link test with one ordinary function, one override and a hook that calls its original body.

Acceptance:

* Direct calls and mapping entries use the intended override.
* `GUEST_ORIG` reaches the original body rather than recursively reentering the override.
* Missing and duplicate symbols fail the build with useful names.

## P1.3: Compile every generated partition

Tasks:

* Compile the entire story partition set, not only a handpicked function or an empty module after dead code elimination.
* Enable and consistently link the selected Wasm exception mode. Generated try and catch blocks are required behavior.
* Inventory compiler failures involving atomics, builtins, alignment, inline assembly barriers and type layout.
* Keep guest addresses 32 bit. Do not truncate host pointers into guest fields or widen guest structures for the Mac.
* Add assertions for required guest structure layouts and confirm no Windows specific CRT imports survive.
* Record warnings requiring semantic review. Do not hide all diagnostics with the native generated code `-w` flag.

Deliverable: all generated partitions compiled to Wasm objects plus a categorized unresolved host dependency report.

Acceptance:

* Every expected source partition has an object artifact.
* Unsupported constructs have explicit fixes or a named blocker; silently omitting a partition is not permitted.
* Both a diagnostic configuration and a representative optimized configuration compile.

## P1.4: Link a retained diagnostic module

Tasks:

* Link the function map and generated bodies into the diagnostic runtime. Ensure the map is retained so linker success cannot result from eliminating all game functions.
* Supply portable implementations for small runtime helpers where practical and named trapping placeholders for services not implemented yet.
* Keep compile placeholders distinct from eventual offline or unsupported platform policies. Record every placeholder and the stage that removes it.
* Do not disable undefined symbol errors or substitute arbitrary successful return values.
* Add entry points that report image ranges, mapping counts and diagnostic host readiness without starting the guest entry point.
* Measure object build time, Wasm size, browser compilation and instantiation time, and peak compiler memory on the Mac.

Deliverable: a browser page backed by the linked story diagnostic module.

Acceptance:

* Function map count agrees with the generated inputs and includes representative generated bodies.
* Browser instantiation succeeds and diagnostic calls return known results.
* A deliberately invoked placeholder produces a precise guest service diagnostic.
* No PE DLL loading, `GetProcAddress`, native subprocess compilation or DX12 dependency is present in the browser target.

## Validation and evidence

Use synthetic symbol and linking tests rather than asserting CMake option text. Use agent-browser for the browser page and keep console output bounded.

Record exact tool versions, input manifest, retained function count, unresolved services, build timings and module size. Compilation of a native ARM64 reference harness can be prepared here, but semantic parity belongs to M2.

## Exit checklist

* [x] P1.1 through P1.4 accepted.
* [x] All story partitions compile and the function map links.
* [x] Browser module instantiates without entering game code.
* [x] Override versus original body dispatch is verified.
* [x] No Windows runtime libraries are required.
* [x] Trapping placeholder inventory is complete.

## Stop conditions

Do not advance to game boot if symbol override semantics are unresolved, generated functions were discarded to force a link, exception behavior was compiled away or the build depends on modifying generated files manually.
