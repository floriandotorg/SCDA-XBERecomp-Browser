# Menu shader and texture verification

## Scope and integration contract

New implementation files only: `src/browser/menu-shaders.h/.cpp` and `src/browser/menu-texture.h/.cpp`.
Native GPU code and generated code were read as semantic references and not modified.
`gpu::MakePixelKey` and `gpu::MakeFixedKey` are reused directly, so the browser target must also link the unchanged portable `src/gpu/shaders.cpp`.
Texture decoding cannot reuse `src/gpu/texture.cpp` safely: its translation unit depends on kernel memory access, and its decode API does not provide byte-span bounds. The new format layer translates only explicitly recognized formats to WebGPU `rgba8unorm`.

`browser::menu::GenerateShaders` receives state, the supplied-register mask, pretransformed flag, four native `gpu::TexInfo` entries, and optional decode flags. Input is already decoded float4 register data with a canonical 256-byte vertex stride, register N at location N and byte offset N*16. Nonzero raw decode flags are rejected.

Generated entrypoints are `vs_main` and `fs_main`. Group 0 binding 0 is `ShaderConstants`, followed by four texture/sampler pairs at bindings 1/2, 3/4, 5/6, and 7/8. Pipelines may optimize away unused pairs when an automatic layout is selected; an explicit service layout may retain all bindings.

`ShaderConstants` is 3504 bytes, aligned to 16 bytes. Byte offsets: vsc 0, surf 3072, misc 3088, tfactor 3104, texsize 3120, wvp 3184, tm 3248. Matrices are row-major arrays of vec4 rows, not WGSL column-major matrix objects. `BuildConstants` includes world/view/projection and viewport transform, alpha comparison/reference, texture factor, and the existing 192-register hardware-order constant array. Texture sizes contain width, height, and normalized UV offsets, including optional render-target half-texel offsets supplied by the backend.

`DescribeTextureLayout` preflights guest resource sizes without reading memory. `DecodeTexture` accepts a checked span beginning at resource level 0, and applies validated mip and cube-face offsets internally. The backend owns descriptor parsing, checked guest memory access, shader-object lifetimes, surface synchronization, and GPU object creation.

## Supported semantics and explicit limits

Unlit fixed-function vertex processing supports XYZRHW and matrix-transformed positions, native screen-to-clip conversion, WebGPU depth range, the reference far-edge adjustment, diffuse/specular register defaults, color saturation, coordinate selection, texture matrices, projected texture coordinates, and flat colors.

Fixed-function pixel processing supports color/alpha operations 2 through 16, 23, and 24, argument complement/alpha-replicate, current/temp results, up to four stages, texture factor, 2D normalized or texel UVs, alpha kill, and the eight alpha-test comparisons. Specular addition follows the native enable state.

Programmable vertex microcode and programmable pixel combiners are deliberately rejected. Fog, vertex lighting/blending, generated texture coordinates, unsupported combine operations, shadow/HDR/3D/cube sampling, signed/color-key textures, raw vertex decode, and point-sprite/coordinate-wrap modes are explicitly rejected rather than replaced by generic shaders. Actual menu variants have not yet been captured, so no claim is made that this subset renders the real frontend. Rejected results include a state description; `DescribeShaderFailure` preserves the error, state, and both complete WGSL sources for compiler failures.

The texture layer supports explicitly listed native color formats, including channel-order variants, luminance/alpha, 16-bit packed color, palette, signed-channel reference mappings, 2D/3D Morton swizzle, linear pitch, mip chains, cube face alignment, and CPU BC1/BC2/BC3 decompression. Depth, YUV, and unknown formats are rejected. Limits are 4096 per dimension, 64 MiB decoded pixels, 256 MiB total source resource, 13 levels, and validated face/mip/pitch/storage relationships. These are deliberate bounded-menu limits, not claimed hardware limits.

## Verification

Tests were written first and failed for absent implementation files. Final command:

```sh
node --test tests/browser/menu-shaders.test.mjs tests/browser/menu-texture.test.mjs
```

Result: 4/4 passing, comprising native Clang ASan/UBSan and Emscripten Wasm builds/runs of both fixture suites. Shader tests use a 1 MiB Wasm stack because `gpu::State` and fixture copies exceed the default stack. Texture fixtures cover exact BGRA/RGBA/channel/packed/alpha/palette pixels, rectangular and volume swizzle, linear pitch bounds, every truncated volume span, mip offsets, cube alignment, BC1 color interpolation/transparency, BC2 explicit alpha, both BC3 alpha interpolation modes, partial blocks, invalid dimensions/formats/pitches, and empty/truncated inputs.

The native shader test emits deterministic WGSL and uniform fixtures into `out/menu-shaders-tests`. Stage only the explicit synthetic fixtures, then open `tests/browser/menu-shaders-browser.html` with an isolated agent-browser WebGPU session:

```sh
node tools/browser/prepare-menu-shader-tests.mjs
node tools/browser/serve.mjs out/menu-shaders-public 8094
agent-browser --session menu-shaders-38f2d4233b0e --webgpu open http://127.0.0.1:8094/tests/browser/menu-shaders-browser.html
agent-browser --session menu-shaders-38f2d4233b0e eval 'window.menuShaderResult'
```

Observed real WebGPU result:

```json
{"compiled":20,"pixel":[32,128,48,255],"alphaDiscard":[0,0,0,0],"depthClip":[0,0,0,0],"result":"menu-shaders-browser-ok"}
```

The browser compiles 20 complete pipelines, including every supported operation, transformed/projected coordinates, alpha test, all four texture stages, temporary results, alpha kill, and flat interpolation. Render/readback verifies exact texture modulation and RGBA channels, alpha-test discard, reciprocal-W clip position, and rejection beyond the WebGPU clip-depth range. No compiler or uncaptured WebGPU validation errors were observed.
