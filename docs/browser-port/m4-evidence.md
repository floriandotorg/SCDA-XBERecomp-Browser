# M4 implementation evidence and remaining work

Status: **active, incomplete, not accepted**. The experimental WebGPU foundation is implemented. The real game menu has not rendered, and no real menu navigation or mission boundary has been accepted.

Parent: [M4: The real menu works](04-menu-works.md).

## Prerequisites

The accepted M3 package provides the genuine normal-renderer device request, matching retained generation provenance and the local test ISO. Chromium exposes WebGPU and shared memory under the local isolated server. An opt-in capture build extracts the retained default-state commands and owned portable render state without modifying original generated code. These prerequisites did not require another ISO, executable, console identity, dashboard asset, credential or Windows installation.

There is currently **no established missing user-supplied prerequisite**. The remaining blocker is unfinished graphics integration, not missing game files. An undocumented low-level default-state method was investigated; the existing native typed-state policy and retained Z/stencil caller establish its narrow state-only use. This is not permission to ignore arbitrary unknown commands or invent GPU behavior.

## Implemented foundation

* `src/browser/webgpu-service.cpp` uses the Dawn integration pinned by Emscripten. A separate yielding GPU worker owns the device, queue, canvas, resources, validation scopes, completion and device-loss callbacks.
* `webgpu-transport.{h,cpp,mjs}` provides owned bounded packets, explicit command/resource IDs, deadlines, terminal cancellation and a 4 MiB mailbox. A guest pthread proxies synchronously to its loader; the independent GPU worker can complete requests while both wait. Request/response publication races cannot reopen a retired mailbox.
* The original `src/gpu/render_thread.cpp` is not linked into this path. Its SSE streaming-copy implementation and detached desktop worker are not reused. Browser commands copy their payloads into owned storage before an asynchronous boundary.
* `render-snapshot.{h,cpp}` implements bounded portable state serialization with deep-owned shader and resource data. `push-state.{h,cpp}` atomically decodes supported retained state-only commands and explicitly refuses output/control/unknown operations. The actual initial 613-word capture classifies as state-only; proprietary captures remain ignored.
* `menu-shaders.{h,cpp}` generates WGSL for a tested fixed-function subset. `menu-texture.{h,cpp}` provides explicit format decoding, including channel order, packed formats, palettes, swizzling and CPU BC1/BC2/BC3 decoding. Unsupported shader and texture semantics produce diagnostics, not generic replacements.
* `menu-backend.{h,cpp}` implements an opt-in `gpu::Backend` bridge with resource versions, physical-storage identity across native header swaps, canonical vertex decoding, topology conversion, deterministic pipeline keys, bounded residency and actual service operations. Uniform-only changes do not continually allocate pipelines. Host-owned index snapshots are copied before asynchronous use.
* Readback uses real GPU staging bytes and source versions. Zero-size `Sync` follows the native whole-resource contract. A first `SyncLate` without an earlier snapshot performs a genuine `Sync` before recording the next snapshot; it cannot successfully return stale CPU bytes. Unsupported readback contracts still refuse.
* Gamma presentation uses a tested channel-specific LUT compositor without changing the guest front buffer. Presentation preserves aspect ratio, including black letterbox regions.
* `diagnostic-graphics.{h,cpp}` retains the M3 checkpoint mode and adds explicit state-capture/rendering modes. Unmirrored typed APIs, unknown guest work and unavailable results stop rather than exposing stale state. Renderer initialization failure is terminal. Session retirement joins guest workers and releases the injected backend before its image memory can disappear.
* `menu-input.{h,cpp,mjs}` bridges press/release state to checked guest controller APIs. Repeat, focus loss, reactivation and alias keys are tested. Published input storage is retained for its disposable module lifetime, so an in-flight UI event cannot write freed session storage.
* `menu-route.{h,cpp}` rejects the existing title menu's Co-op/Versus selections while delegating Solo and unrelated menu actions. `menu-launch.{h,cpp}` explicitly rejects dashboard/unsupported executable launches and owns copied launch data. An opaque launch request is reported as **unclassified**, not proof of a single-player mission.
* `menu-runtime.mjs`, `menu-shell.mjs` and `menu.html` provide a separate host Start/Stop, canvas and diagnostics. Only an actual completed Present can activate the host's rendering state. This host UI is not a replacement game menu.

## Validation

Complex protocol, ownership, decoding, shader, input and readback changes have red-first regressions. Coordinator evidence is retained in ignored `out/m4-prerequisites/`.

* `final-js-tests.log`: 82 passing controller/session/runtime/service-protocol tests.
* `final-contracts.log`: 20 passing focused shader, texture, input, route, launch, snapshot and push-state contracts, including native sanitizers and Wasm where applicable.
* `late-first-red.log` and `late-first-green.log`: the first-late-readback regression fails before its fix; both final native sanitizer and Wasm backend contracts pass afterward.
* `final-graphics-tests.log`: focused graphics/SDK/push contracts, including real exported Gamma, Swap and Draw wrappers, state-only decoding, terminal initialization failure and explicit backend retirement.
* `final-server-tests.log`: local diagnostic-server regression coverage.
* The default M3 build was restored and passes the retained real-ISO harness across three fresh generations at `GRAPHICS_CHECKPOINT`, with zero pending reads and staging. Its 233 manifest inputs all match. Agent-browser also reproduces the default checkpoint without page errors. This preserves bounded M3 behavior, not M4 acceptance.
* The broad interim suite was interrupted after contention caused compiler timeouts and before the final frontend safety fixes. It is **not** a passing full-suite result. The old M3 248-test frozen acceptance is historical evidence for that package, not a claim that every test has been rerun successfully on this tree.

Agent-browser independently reproduces the final synthetic GPU scene with **47 commands**, real blocked C++ pthread callers, readback, resize, copies, depth drawing, presentation and gamma. All tracked resources retire to zero. Its three expected diagnostics deliberately exercise malformed gamma data, invalid WGSL and a missing resource; they are not suppressed. Separate lifecycle tests resolve pending requests on actual device loss and shutdown, again with zero tracked resources.

The independent WGSL fixture compiles **20 pipelines** and checks known pixels, alpha discard and clip depth. These are public synthetic fixtures, not captured game variants. See [shader coverage](../../tests/browser/menu-shaders-report.md) and [GPU service evidence](../../tests/browser/webgpu-service-report.md).

## Actual title result

The latest experimental browser run authenticates and executes the genuine title but stops at **typed graphics API 29, `D3DDevice_SetRenderState_Simple`**. This is an explicit unsupported frontend operation, before a game frame. The diagnostic has zero active guest work after retirement and no WebGPU validation errors. Earlier diagnostic captures and synthetic clears are not menu frames.

Observed game rendering evidence:

| Item | Result |
| --- | --- |
| Real menu frames | 0 |
| Real menu shader variants accepted | 0 |
| First visible game frame | Not reached |
| Canvas | 640 by 480 |
| Adapter | Apple, `metal-3`, actual service adapter |
| Maximum 2D texture dimension | 16384 |
| Maximum uniform binding size | 65536 bytes |
| Maximum vertex attributes | 30 |
| Maximum vertex stride | 2048 bytes |
| BC / ETC2 / ASTC adapter support | Available; CPU decoding does not require BC |
| Real idle-menu resource budget | Not measurable before the menu runs |
| Real profile / Solo navigation | Not exercised |
| Verified single-player mission boundary | Not reached |

No M4 exit checkbox is accepted on this evidence.

## Reproduction

Use the pinned Emscripten toolchain and retained trusted generation record already required by M3. Player messages and selected files do not become build authority. From the accepted local build configuration, the experimental rendering build is:

```sh
mkdir -p out/test-tmp
PATH="/opt/homebrew/opt/emscripten/bin:$PATH" TMPDIR="$PWD/out/test-tmp" \
  emcmake cmake -S . -B out/boot -DBROWSER_BUILD=ON -DBROWSER_BOOT=ON \
  -DBROWSER_STORY=OFF -DBROWSER_MENU=ON -DBROWSER_MENU_CAPTURE=OFF
TMPDIR="$PWD/out/test-tmp" cmake --build out/boot --target story_boot -j 2
node tools/browser/serve.mjs out/boot/boot-web 8087
```

Open the local player with agent-browser and select the existing local ISO. Expect the explicit typed-state diagnostic above, not a menu. The default `BROWSER_MENU=OFF` and `BROWSER_MENU_CAPTURE=OFF` retain the bounded M3 checkpoint path.

Synthetic GPU fixtures are staged separately from private evidence:

```sh
node tools/browser/prepare-webgpu-tests.mjs
node tools/browser/serve.mjs out/browser-webgpu-tests/public 8093
```

Run `tests/browser/webgpu-service.html` and `tests/browser/webgpu-service-lifecycle.html` under that origin with agent-browser. For shader fixtures, run the focused shader test, then `tools/browser/prepare-menu-shader-tests.mjs` and serve `out/menu-shaders-public`. Do not serve private evidence or game assets.

## Remaining implementation

1. Port and verify the actual typed render-state, constants, shader/resource, viewport, scissor and target APIs rather than delegating them and drawing stale snapshots.
2. Capture the first genuine menu draw states and implement their required programmable shader operations and texture/resource behavior without fallback substitutions.
3. Exercise the real frontend with `MenuBackend`, including every reached lock, delayed readback, swap and resource release. Independent Probe and backend tests do not replace that integration.
4. Render and inspect actual menu text, imagery, alpha and highlights, then measure idle resource stability and first-frame timing.
5. Navigate the real profile/confirmation/Solo screens with keyboard input and identify a verified single-player launch/loading boundary. Synthetic route rejection and copied opaque launch data are insufficient.
6. Freeze source, rerun the complete regression suite, verify build provenance and repeat browser lifecycle acceptance before marking P4.1 through P4.4 accepted.
