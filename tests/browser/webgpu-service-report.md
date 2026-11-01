# P4.1 independent WebGPU service evidence

## Scope

Implemented usable C++ GPU operations over the pinned emdawnwebgpu C API, a yielding dedicated GPU worker, and the owned transport shared with the future game backend. This is independent synthetic topology evidence, not evidence that the real game menu renders or that P4.2 through P4.4 are complete.

Prerequisites used:

* Emscripten 6.0.13 at `/opt/homebrew/opt/emscripten/bin/em++`.
* Pinned Emscripten port `--use-port=emdawnwebgpu`; the coordinator reproduced the final scene using the built-in port after independently verifying the matching public package.
* Dawn `v20260423.175430`, revision `31e25af254ab572c77054edec4946d2244e184dd`.
* Agent-browser session `webgpu-service-worker` with `--webgpu`.
* Synthetic-only staging server at `http://127.0.0.1:8093` with COOP/COEP and `crossOriginIsolated === true`. The preparation script copies an explicit public fixture list, not game assets or private evidence.

No CMake, inventory, boot-shell, generated source, snapshot, shader-generation, or game-backend files were edited by this worker.

## Topology and ownership

`Transport::Execute` copies request bytes into an independent 4 MiB shared mailbox and copies the reply into an owned vector. Actual guest pthreads do not inherit loader `Module.webgpuMailbox`, so the C++ transport synchronously proxies to the loader using `MAIN_THREAD_EM_ASM_INT`. The guest pthread and loader may block, but the separate GPU worker continues yielding and handles device requests, validation scopes, queue completion, and map callbacks.

The synthetic guest fixture is compiled with `-pthread -sPTHREAD_POOL_SIZE=1`. Every command executes from an actual `pthread_create` thread and verifies that its own `Module.webgpuMailbox` is undefined. Thus the passing test exercises the required main-runtime proxy, not accidental worker module-property inheritance.

The mailbox has one serialized producer, a fixed 64-byte header, command/resource IDs, and a 4 MiB payload/reply capacity. C++ serializes callers with a mutex. State transitions are idle 0, submitted 1, executing 2, completed 3, terminal 4, and reserved-writing 5. Compare/exchange publication and a separate terminal-status word prevent shutdown during request or response copying from reopening a closed mailbox or publishing stale success. Command sequences never wrap. Client deadlines are bounded at 30 seconds. Shutdown uses a fresh mailbox for each session; closed mailboxes are not reusable.

GPU resources use nonzero session-local 32-bit IDs, reject duplicates, and are capped at 4,096 tracked IDs, 256 pipelines, and a 256 MiB logical texture budget. RGBA uploads are owned packet bytes; no borrowed guest pointers cross the async boundary. Temporary vertex, index, uniform, sampler, bind-group, view, and readback resources are released after their required lifetime. Validation failures roll back newly created tracked resources.

## API

`src/browser/webgpu-transport.h` defines the binary packet layouts. Struct sizes are asserted for pipeline, draw, and clear packets. Payload scalars use the little-endian layout of the supported Wasm/native targets.

* Configure: canvas dimensions.
* CreateTexture: RGBA8 unorm upload, or depth24plus-stencil8 target with no upload payload.
* CreatePipeline: explicit vertex and fragment WGSL, entrypoints `vs_main` and `fs_main`, fixed topology/cull/front-face/blend/depth/write-mask fields.
* Draw: 16 canonical vec4 attributes per vertex at a 256-byte stride, optional uint16/uint32 indices, owned uniform bytes, four texture/sampler pairs, viewport/scissor, optional depth target.
* Clear: color, depth, and stencil attachment clears.
* Copy: checked full-size color texture copy.
* Present: a dedicated GPU compositor applies the per-channel gamma LUT, scales to the canvas, optionally fits 4:3 with black bars, and copies its owned output target to the actual canvas, with real queue completion before notification.
* SetGamma (12): exactly 768 planar byte entries, R[256], G[256], and B[256], copied into owned state and uploaded to a GPU LUT texture.
* Readback: staging copy and real asynchronous map, returning tightly packed RGBA bytes without row padding. Resource 0 reads the last composited presentation target; normal resource IDs read unchanged raw textures.
* Destroy, Flush, Shutdown: explicit resource destruction, real submitted-work completion, and service resource release.

Pipeline and sampler integer fields are pinned `webgpu.h` enum values, not raw guest NV/D3D values. WGSL binding 0 is the uniform buffer; bindings 1/2, 3/4, 5/6, and 7/8 are texture/sampler pairs. All four texture IDs must be valid, including unused shader stages. The backend supplies an explicit white texture for absent stages; the service never substitutes a generic shader for unsupported shader state.

`startGPUWorker({canvas, mailbox, moduleURL, signal, onDiagnostic, onPresented, onAdapter})` returns `ready`, `worker`, `webgpuMailbox`, `cancel`, `dispose`, and `shutdown`. The canvas is an owned transferred OffscreenCanvas. `cancel` immediately closes the mailbox and wakes blocked transport callers. `dispose` and `shutdown` are the same asynchronous operation: close the mailbox, request shutdown, await `stopped` with zero tracked resources, then terminate the worker. Initialization defaults to 20 seconds and disposal to 5 seconds; timeout or nonzero resources rejects disposal and reports forced termination rather than claiming successful cleanup. Abort before readiness is supported.

Host code must call/await `dispose` even after sending the Shutdown opcode; the opcode releases service resources, while host disposal owns mailbox closure, the stopped handshake, and worker termination.

The presentation pipeline is an explicit compositor, not a shader fallback for unsupported game shaders. It never modifies the source backbuffer. Its pipeline, LUT texture, and resize-owned presentation target are included in the tracked resource count and released on shutdown.

`onAdapter` reports the actual service adapter, not a separate probe. `onPresented` only fires after the submitted Present copy finishes and its validation scope completes. Device loss resolves an in-flight request with Lost and makes subsequent operations terminal. Uncaptured API errors are reported separately as Error. The production service does not include the forced-loss export; the test build enables it with `WEBGPU_SERVICE_TESTING`.

## Tests-first protocol results

Run:

```sh
node --test tests/browser/webgpu-service*.test.mjs
```

Final result: 15 passed, 0 failed. Initial tests failed before the transport existed. Later red tests reproduced malformed reply handling, missing bounded worker disposal, and a real shutdown-during-response-copy race before their fixes.

Covered owned copies, payload/reply bounds, busy rejection, exceptions, command/resource attribution, sequence exhaustion, deadlines, malformed lengths/status, shutdown while pending, stale completion rejection, request/response copy cancellation races, zero-resource disposal handshake, forced termination on missing handshake, initialization timeout, abort, and rejection of nonzero-resource cleanup claims.

## Browser GPU results

Build and serve:

```sh
node tools/browser/prepare-webgpu-tests.mjs
node tools/browser/serve.mjs out/browser-webgpu-tests/public 8093
agent-browser --session webgpu-service-worker --webgpu open http://127.0.0.1:8093/tests/browser/webgpu-service.html
agent-browser --session webgpu-service-worker wait --fn 'window.webgpuResult?.finalResources === 0'
agent-browser --session webgpu-service-worker eval 'window.webgpuResult'
```

Final scene result: `ok: true`, 47 commands, every command `pthreadMailboxUnbound: true`, and final tracked resources 0.

The scene uploads a 2x2 RGBA texture containing four distinct colors and one alpha-128 texel, samples it using UV interpolation, and verifies every output byte at 4x4 and after resize to 8x4. Expected source texels are `[17,83,149,255]`, `[200,11,57,128]`, `[90,160,30,255]`, and `[7,19,231,255]`. The pixel assertions check orientation, channel order, alpha, and resized output. Nonindexed, uint16-indexed, and uint32-indexed draws pass through the same production packet/resource machinery.

The fixture also creates and clears depth24plus-stencil8 targets, performs depth-enabled draws, clears color, copies to a second color target, performs real readback, presents four times, destroys resources, and flushes the real queue. Present notifications are command 13/resource 5, command 16/resource 5, command 20/resource 5, and command 37/resource 5. A malformed 767-byte gamma request is attributed to command 23/resource 0; invalid WGSL to command 42/resource 77; and an unknown resource to command 43/resource 999. These three deliberate errors are the only expected scene diagnostics. The invalid WGSL resource is rolled back.

The gamma extension was implemented only after its new browser test failed at command 14 with unsupported opcode. The green fixture verifies default and restored identity presentation byte-for-byte, then an intentionally distinct LUT that inverts red, offsets green by 37 modulo 256, and halves blue. Every composited pixel is checked per channel, including unchanged alpha. A second readback verifies the original source backbuffer remains unchanged. An 8x8 canvas with 4:3 presentation is checked as an 8x6 scaled image with two black rows. A short gamma upload must fail without corrupting the restored identity LUT. The compositor resources are counted in the reported resource total and released to zero.

Actual service adapter:

* Vendor: `apple`.
* Architecture: `metal-3`.
* Adapter max texture dimension 2D: 16,384.
* Adapter max vertex attributes: 30.
* Adapter max vertex buffer array stride: 2,048.
* Adapter max uniform buffer binding size: 65,536.
* Actual adapter `textureCompressionBC`, `textureCompressionETC2`, and `textureCompressionASTC`: all true, queried with `wgpuAdapterHasFeature` on that same adapter.
* The callback also records supported feature enum values from the pinned C header. CPU BC-to-RGBA decoding does not require enabling BC on the service device.

The depth test initially exposed a pinned binding issue: a load-only depth attachment still serialized its default nonfinite clear value. Explicitly supplying a finite depth clear value fixed the browser failure.

Lifecycle browser fixture:

```sh
agent-browser --session webgpu-service-worker open http://127.0.0.1:8093/tests/browser/webgpu-service-lifecycle.html
agent-browser --session webgpu-service-worker wait --fn 'window.webgpuResult !== undefined'
agent-browser --session webgpu-service-worker eval 'window.webgpuResult'
```

Final result: `ok: true`. Destroying the real test device while Flush command 91 was pending resolved that request with Lost (3), emitted an attributed device-loss diagnostic, made the next request return Lost, and released tracked resources to 0. A separate actual guest pthread was observed blocked with mailbox state 1; host cancellation woke it with Closed (2). Both run with cross-origin isolation enabled.

Machine-readable local outputs are in `out/browser-webgpu-tests/draw-result.json` and `out/browser-webgpu-tests/lifecycle-result.json`; these are ignored build evidence, not checked-in generated files.

## Honest limits and remaining integration

* This worker did not validate the actual game menu or claim the M4 milestone.
* The service is usable GPU operations rather than the full `gpu::Backend` adapter; the coordinator/backend worker owns game-state decoding and integration.
* Presentation supports nearest sampled stretching to the configured canvas or an optional centered 4:3 fit with black bars. Higher-order presentation filtering and color spaces beyond RGBA8 unorm are not claimed.
* Copy currently supports full-size unaliased color textures, not partial regions, depth copies, resolves, or format conversion.
* Depth comparison/write and attachment stencil clears are supported. Programmable stencil operations are not represented by PipelinePacket and must be rejected upstream when required.
* Indexed strip pipelines need a strip index format or upstream topology conversion; that path is not claimed tested. The tested indexed topology is triangle-list.
* Samplers support clamp/repeat/mirror and nearest/linear with one mip level. Border color, anisotropy, mip chains, multisampling, compressed GPU texture uploads, and arbitrary formats are not claimed.
* The 4 MiB packet limit means large textures require an additional chunked upload operation before they can be supported; the current API rejects rather than truncates them.
* Successful Draw replies mean validated queue acceptance. Call Flush for GPU completion, or use Readback/Present, which perform their required asynchronous completion waits.
* Full device-loss recovery is a fresh worker/device/mailbox session, not recovery in the lost session.
