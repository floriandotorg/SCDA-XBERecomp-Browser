# M1 compilation evidence

Status: P1.1 through P1.4 accepted for diagnostic compilation. CPU correctness and game boot remain outside this acceptance.

## Reproduce on macOS

The selected toolchain is the installed Apple Silicon Homebrew Emscripten 6.0.13 package. Its compiler reports `6.0.13-git`, which the browser configure step checks exactly. Changing that version requires a deliberate pin update and rerunning the symbol proofs. No Windows SDK, PE libraries or recompiler executable is used.

```sh
export PATH="/opt/homebrew/opt/emscripten/bin:$PATH"
emcc --version
node --test tests/browser/*.test.mjs

emcmake cmake -S . -B out/browser-empty -G Ninja \
  -DBROWSER_BUILD=ON -DBROWSER_STORY=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build out/browser-empty -j 4
node tools/browser/serve.mjs out/browser-empty/web 8080
```

The empty diagnostic reports zero retained functions. The complete story diagnostic is the default browser configuration:

```sh
emcmake cmake -S . -B out/browser -G Ninja \
  -DBROWSER_BUILD=ON -DCMAKE_BUILD_TYPE=Debug
/usr/bin/time -l cmake --build out/browser -j 4
node tools/browser/verify.mjs out/browser \
  /opt/homebrew/opt/emscripten/libexec/llvm/bin/llvm-nm
node tools/browser/serve.mjs out/browser/web 8081
```

The representative optimized configuration uses CMake Release, including `-O3`:

```sh
emcmake cmake -S . -B out/browser-release -G Ninja \
  -DBROWSER_BUILD=ON -DCMAKE_BUILD_TYPE=Release
/usr/bin/time -l cmake --build out/browser-release -j 4
node tools/browser/verify.mjs out/browser-release \
  /opt/homebrew/opt/emscripten/libexec/llvm/bin/llvm-nm
node tools/browser/serve.mjs out/browser-release/web 8082
```

Open the printed loopback URL. Expect `Diagnostic ready`, 33,172 retained functions, the build identity and no guest entry execution. Click **Test unavailable guest service** and expect:

```text
Placeholder: x86_kernel address=0x00000123: M3 kernel ordinal unavailable
```

Use agent-browser with a named session for browser validation. `window.diagnostic` exposes bounded counters and timings. The server accepts only GET and HEAD, supplies isolation headers and Wasm MIME, rejects traversal and escaping symlinks, and serves only diagnostic web file types beneath the configured directory. Point it at the build's `web` directory, not the repository or asset directory.

## Inputs and identity

Each build writes `tools/browser/input-manifest.json` and `tools/browser/build_identity.h` under its build directory. The manifest records SHA256 for all 128 partitions, the mapping, shared declarations, image symbols, both configuration inputs, compatibility headers and diagnostic build sources. Configuration verifies all partition files, unique body and alias declarations, mapping addresses, and all 227 configured function and data symbols. Input changes trigger configuration again.

Observed inventory:

* 128 partition objects plus the mapping object in both configurations.
* 33,172 real image functions in the retained map, including `sub_00011000` and `sub_004F50DF`.
* 14 additional generated absent symbol traps in the mapping translation unit. These are not real image mapping entries and are counted separately from mapped functions.
* Image range `0x00010000..0x006D1540`, code range `0x00011000..0x004F5150`, entry `0x00054775`, kernel thunk `0x004F5160`.
* Debug identity `560c367de2a5939b347feccec566b7f79dd03ca2d6ff1485c043ce3941b9b141`.
* Release identity `fa39d1335e4af9b8cbc6b05a3463a9de2cfdf2053e1b76d38e0877ff663bb7b1`.

The currently available `assets/default.xbe` has SHA256 `c742d61c9b8d8c1c4f886a4a4a69ffb65ab3961e0283dcad33bfcfa963885143`. Its header reports `XBEH`, image base `0x10000`, 34 sections and image extent matching the generated metadata. This is an observation of the available file, not proof of the original generator's input identity. No original generation hash was supplied. The manifest deliberately keeps generation provenance unknown. Real boot remains blocked until matching ISO identity is established. Configuration never opens or hashes an XBE, so compilation does not depend on this optional local file.

Generated artifacts were not edited. Manifest files and binaries stay under ignored `out/`; they are not distribution artifacts.

## Symbol and exception proof

Decision: use Clang Wasm weak aliases in `src/cpu/x86_ops.h`, gated on `__EMSCRIPTEN__`, compatibility version `wasm-weak-alias-v1`. Keep the native PE branch unchanged. There is no source rewriting or adapter generator.

The synthetic test compiles separate translation units at `-O2`. A caller and mapping in the generated body translation unit both reach a strong replacement from another translation unit. The replacement calls the original `__imp__` body. The original executes once, without recursive replacement. Ordinary calls still reach their bodies. Missing and duplicate strong symbols produce linker failures with their names. A thrown `X86Resume` preserves both fields when caught.

`GUEST_ORIG` keeps its existing `__imp__sub_` naming contract; no guest calling convention or argument layout changes are made. Forwarding adapters remain an alternative if a future compiler stops supporting weak aliases. Reversal requires changing the browser macro and rerunning these tests, not modifying generated files.

To run the synthetic proof in a browser after the tests:

```sh
node tools/browser/serve.mjs out/browser-tests 8083
```

Expect `symbol-dispatch-ok`. Chromium produced that marker with no browser errors. The same proof also passed in Node, which supplements browser validation.

`-fwasm-exceptions` and `-pthread` apply consistently to every generated object and the final link. Guest addresses remain `uint32_t`. Assertions cover the Wasm host context offsets, context alignment and size, XMM layout, mapping layout and continuation payload. These are compilation checks, not an assertion that flat guest memory or unaligned atomics are correct.

## Host dependencies and placeholders

`verification.json` in each build records object host references, categorized toolchain dependencies, final Wasm imports and diagnostic results. Undefined symbol errors remain enabled. No unresolved final link services are waived.

All seven guest runtime services are intentionally unavailable:

* `x86_icall`: named trap, removed by M2 indirect dispatch.
* `x86_resume`: named trap, removed by M2 continuation control flow. This is not a successful substitute for `X86Resume` unwinding.
* `x86_kernel`: named trap including ordinal, replaced by staged M3 kernel services.
* `x86_trap`: terminal diagnostic carrying guest address and reason; M2 supplies the guest fault boundary.
* `x86_priv`: named trap carrying address and reason, replaced by M2 privileged instruction policy.
* `x86_rdtsc`: named trap, replaced by M2 guest timing.
* `x86_cpuid`: named trap, replaced by M2 CPU feature policy.

These throw a typed diagnostic failure rather than returning a fake success. The explicit diagnostic entry catches the deliberately invoked kernel placeholder so the page can display it. No game entry point or generated game function is executed during module initialization. The 14 generated absent image functions retain their own `not in this image` traps.

Other object dependencies resolve through Emscripten's standard library: Wasm unwind and C++ exception machinery, function table and stack globals, and `atan2`, `cos`, `exp2`, `fmod`, `frexp`, `ldexp`, `log2`, `sin`, `tan`. The Debug module exposes only Emscripten and WASI browser glue imports. Release minifies those import names. No Windows CRT forwarding, DLL loading, Winsock, XAudio2, DX12, resource compilation or native subprocess compiler enters this target.

These placeholders do not define eventual offline or unsupported platform policies. No networking path is claimed to succeed or be supported.

## Observations

Machine: Apple Silicon arm64, macOS 27.0.1 build 26A434, 10 physical CPU cores, 64 GiB memory. Tools: Emscripten `6.0.13-git` from Homebrew package 6.0.13, Clang `24.0.0git`, CMake 4.4.4, Ninja 1.13.2, Node v26.5.0. Browser validation used agent-browser with Chromium **151.0.7922.71**, arm64, as the initial tested browser pin.

Full initial object and link builds, with four jobs:

* Debug: 73.81 s wall time; largest observed process RSS 895,451,136 bytes; Wasm 284,246,410 bytes including debug information.
* Release: 125.75 s wall time; largest observed process RSS 917,061,632 bytes; Wasm 25,312,726 bytes.

The builds partly overlapped, so these are feasibility observations, not isolated throughput benchmarks. `/usr/bin/time -l` reports maximum process RSS across the command's children, not aggregate concurrent compiler memory. Its separate peak footprint field describes the wrapper and is not used as compiler memory evidence.

Browser calls to `WebAssembly.compile` and async `WebAssembly.instantiate` succeeded for both retained modules:

* Debug: 88.53 ms compilation and 1.635 ms instantiation.
* Release: 27.35 ms compilation and 2.09 ms instantiation.

These are single observations after earlier loads; browser code caching and lazy compilation can affect them. Fetch and Emscripten initialization time are excluded. Synchronous main thread instantiation was rejected for the large Debug module with `WebAssembly.Instance is disallowed on the main thread, if the buffer size is larger than 8MB`; the diagnostic uses the async API instead, with rejection propagated to its visible error state.

Both browser modules reported cross origin isolation, shared buffers, shared Wasm memory, workers, Wasm exceptions, WebGPU, File API and AudioWorklet availability. WebGPU adapter creation, pthread scheduling and audio output are not tested by these presence checks. Browser error output was empty. The deliberate kernel placeholder reported its precise diagnostic without starting the guest.

Compiler inventory:

* All 128 partitions compiled in both configurations, including atomics, builtins, blank inline assembly memory barriers and generated try/catch. No partition was omitted and no generated source fix was needed.
* Debug emitted 144 `-Wparentheses-equality` warnings from generated condition parentheses. Diagnostics remain enabled; no blanket `-w` is used.
* Both links warned about `-pthread` with `ALLOW_MEMORY_GROWTH`. This remains a performance review item for M2; no speculative setting change is hidden here.
* Flat `base + guest_address` access, guest alias windows, atomic alignment and x87/SSE rounding remain M2 semantic work. Successful Wasm compilation does not validate them.

Five automated tests pass: symbol/body/mapping validation, configuration identity validation, server confinement, optimized override/original/exception behavior, and missing/duplicate link failures. Tests were run in failing form before their corresponding implementations. The retained module verifier independently checks every expected object, mapping counts and boundary entries, build identity, and the deliberately invoked placeholder.

Native Windows execution was not tested. Only its unchanged conditional branch and isolated build definition were inspected.
