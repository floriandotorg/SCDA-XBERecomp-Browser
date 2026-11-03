# Menu shader and texture verification

## Scope and evidence boundary

This work audits the retained native `src/gpu/shaders.cpp`, `src/gpu/d3d12.cpp`, and `src/gpu/texture.cpp` and extends the browser shader subset with public, synthetic fixtures. Native implementation and generated title code were not changed. No proprietary instruction bytes, title images, captures, or observed shader state combinations are tracked here.

A private title-state capture now supplied one programmable shader pair. Offline reconstruction translates that pair and WebGPU compiles its pipeline with synthetic normalized 2D texture metadata for stage 0; this does not establish a successful runtime draw or that the real menu renders. Raw capture data, generated title WGSL/HLSL, and private evidence remain only under ignored `out/`. Unsupported semantics continue to return named failures rather than a generic shader or successful stub.

## Integration contract

`GenerateShaders` consumes a portable `gpu::State`, supplied-register mask, pretransformed flag, four `gpu::TexInfo` entries, and optional decode flags. Vertex input is already decoded float4 data with a canonical 256-byte stride, register N at location N and byte offset N*16. Raw decode flags are rejected.

Entry points are `vs_main` and `fs_main`. Group 0 binding 0 contains `ShaderConstants`, followed by four texture/sampler pairs at bindings 1/2, 3/4, 5/6, and 7/8. Automatic layouts can omit unused bindings. Shader compilation failures retain state and both complete WGSL sources through `DescribeShaderFailure`.

The browser target now needs both unchanged portable `src/gpu/shaders.cpp` and new `src/browser/menu-pixel-shaders.cpp`. The latter is the separately owned, coordinated pixel-combiner translator integrated here.

`ShaderConstants` is now **3792 bytes**, aligned to 16 bytes. Existing offsets are unchanged: vsc 0, surf 3072, misc 3088, tfactor 3104, texsize 3120, wvp 3184, and tm 3248. New `pc[18]` starts at 3504. Matrices remain row-major arrays of vec4 rows. `pc[2*n]` and `pc[2*n+1]` contain ARGB-decoded native simple-state constant0/constant1 for stage N; pc16/17 contain the final constants. They deliberately do not use the unrelated `psc` array. Backend buffer creation and upload sizes must use the updated size.

## Exact shader coverage

### Fixed-function vertex path

Existing coverage remains: unlit XYZRHW and matrix-transformed positions, native screen-to-clip conversion, WebGPU depth range, reference far-edge adjustment, diffuse/specular defaults and saturation, coordinate selection, texture matrices, and projected texture coordinates. Flat state emits `@interpolate(flat)` for diffuse and specular in both VS and FS, but not texture coordinates. A nonuniform-vertex-color readback now verifies first-vertex flat interpolation.

### Programmable vertex subset added

Ordinary vertex programs with type `0x2078`, complete four-word instructions, an END token, and at most 256 instructions are translated directly from the native token fields. Supported operations:

| MAC | Operation |
| --- | --- |
| 0 | NOP |
| 1 | MOV |
| 2 | MUL, including native zero-product selection |
| 3 | ADD |
| 4 | MAD using the same multiplication rule |
| 5 | DP3 |
| 6 | DPH |
| 7 | DP4 |
| 8 | DST |
| 9 | MIN |
| 10 | MAX |
| 11 | SLT |
| 12 | SGE |
| 13 | ARL with native floor(x + 0.001) rule |

ILU supports NOP, vector MOV, and RCC (clamped reciprocal). RCC replicates source C's selected first component before either paired operation consumes it and clamps reciprocal magnitude to the native bounds while preserving sign, including negative zero. Bounded division avoids undefined division-by-zero or finite-input overflow. Translation preserves source swizzles and negation, input defaults, R0-R11 and R12/oPos aliasing, component write masks, paired MAC/ILU read-before-write behavior and R1 routing, immediate constants, and ARL-relative constants clamped to hardware register range 0-191. Supported output registers are position, diffuse, specular, and T0-T3. The native programmable screen-space epilogue, depth scaling, texture output projection, and color saturation are preserved.

Truncated programs, missing END, unsupported program types, MAC14/15, ILU2/4-7, temporary registers 13-15, immediate constants outside 0-191, constant-output writes, and unsupported output registers are rejected. Point-size, back-color, and fog outputs are not silently accepted. No ordinary RCP, reciprocal square root, exponent, logarithm, or LIT support is claimed. Finite synthetic pixel behavior was verified; exhaustive NV2A precision, exceptional floating-point values, and native-driver rounding equivalence were not established.

### Fixed-function pixel path extended

Supported operations are now 2-16 and 18-24. New operations 18-21 implement native MODULATEALPHA_ADDCOLOR, MODULATECOLOR_ADDALPHA, MODULATEINVALPHA_ADDCOLOR, and MODULATEINVCOLOR_ADDALPHA expressions, including argument1 alpha after complement/alpha-replicate. Their alpha path returns argument1 and ignores argument2, including otherwise-invalid unused argument bits. New DOTPRODUCT3 follows the retained native expression, including its scalar-alpha-to-float3 behavior, rather than substituting a different alpha convention. PREMODULATE 17 and operations outside this list remain rejected.

Existing argument complement/alpha-replicate, current/temp destinations, four stages, texture factor, 2D normalized/texel UVs, alpha kill, all eight alpha comparisons, and native fixed-function specular addition remain supported.

### Programmable pixel integration added

`TranslatePixel` is invoked for programmable keys, with failures returned to the caller. Its WGSL body shares the same varyings, textures, samplers and uniform layout. Common alpha testing remains outside the translated body; fixed-function specular addition is not applied to programmable output.

The coordinated translator covers up to eight native combiner stages, zero/constant0/constant1/diffuse/specular/T0-T3/R0/R1 inputs, all eight input mappings, RGB and alpha channel selection, AB/CD products, sum and MSB-alpha mux, native six output mappings, stage saturation, RGB-to-alpha output flags, simultaneous RGB/alpha reads, per-stage/shared constants, final combiner constants, and 2D mode-1/projective texture fetches plus mode-4 coordinates. Fog inputs, RGB dot products, final EF/color-sum registers, unsupported texture modes, two-sided lighting, texture compare/dot/input-texture metadata, unknown registers/destinations, unsupported output mappings, and unsupported final flags remain named errors. The helper's own report/tests provide its complete internal contract.

## Texture audit

No texture implementation change was necessary for this bounded extension. Existing decoding agrees with retained native mappings for the explicitly listed formats: channel-order variants, luminance/alpha, 16-bit packed color, palette, signed-channel reference byte mappings, rectangular/volume Morton swizzle, linear pitch, mip chains, cube face alignment, and CPU BC1/BC2/BC3 decompression. Output is explicit `rgba8unorm`, not a blanket reinterpretation of native host formats.

`DescribeTextureLayout` preflights resource storage without reading guest memory. `DecodeTexture` receives a checked span beginning at resource level 0 and applies validated mip/face offsets itself. Limits remain 4096 per dimension, 64 MiB decoded output, 256 MiB source storage, 13 levels, and validated face/mip/pitch relationships. YUV, depth and unknown formats remain rejected. Shader sampling of volume/cube, shadow/HDR, signed/color-key textures, generated coordinates, fog/lighting/blending, point sprites, coordinate wrapping, and raw vertex decoding remains unsupported even where CPU resource decoding exists.

## Verification and reproduction

Complex extensions were tested before implementation. Native and Wasm shader tests first failed for fixed ops18-22, programmable vertex support, the pc layout, and integrated programmable pixel support, then passed after each implementation. Final focused run:

```sh
node --test tests/browser/menu-shaders.test.mjs tests/browser/menu-texture.test.mjs tests/browser/menu-pixel-shaders.test.mjs
```

Result: **6/6 passing**, native Clang ASan/UBSan and Emscripten Wasm runs for all three suites. The shader Wasm test uses a 1 MiB stack. Texture fixtures retain exact channel/packed/palette checks, swizzle and volume bounds, all truncated-volume spans, pitch/mip/cube offsets, both BC3 alpha modes, BC1 transparency, BC2 alpha, partial blocks, and invalid/truncated resources.

Serve only the explicitly whitelisted synthetic fixture tree:

```sh
node tools/browser/prepare-menu-shader-tests.mjs
node tools/browser/serve.mjs out/menu-shaders-public 8095
agent-browser --session m4-shader-worker-38f2d4233b0e --webgpu open http://127.0.0.1:8095/tests/browser/menu-shaders-browser.html
agent-browser --session m4-shader-worker-38f2d4233b0e eval 'window.menuShaderResult'
```

Final synthetic WebGPU result: **68 complete pipelines compiled**, `result: menu-shaders-browser-ok`, and no compiler or uncaptured validation errors. Exact readbacks include:

- Textured, flat, programmable vertex MOV, paired old-register reads, relative constants, and combined programmable VS/PS: `[32,128,48,255]`.
- Every supported vertex MAC operation has its own deterministic rendered pixel check.
- RCC has ten `rgba32float` readbacks, covering positive/negative 2, zero, 1e-30, 1e30, and 1. All four lanes match native clamped reciprocal results within 1e-6 relative tolerance, including signed zero and both saturation bounds. Native/Wasm assertions additionally check scalar C replication in paired MAD/RCC, read-before-write ordering, and forced R1 routing.
- Fixed ops18-22: `[80,136,240,64]`, `[80,80,208,64]`, `[112,152,255,64]`, `[112,80,111,64]`, `[127,127,127,255]`.
- All eight programmable pixel input mappings and all six output mappings have exact pixel checks.
- Final constants: `[12,21,32,80]`.
- Simultaneous RGB/alpha old-register reads: `[255,255,255,64]`.
- Programmable mux with high/low source alpha: `[64,128,192,255]` and `[128,255,64,64]`.
- Fixed and programmable alpha-test discard, programmable texture alpha-kill with nonzero RGB and zero alpha, and depth clipping: `[0,0,0,0]`.

Observed adapter was Apple, architecture `metal-3`. Exposed limits: uniform binding 65536 bytes, vertex attributes 30, interstage variables 28, sampled textures per stage 48, samplers per stage 16, and 2D texture dimension 16384. This path requires 3792 uniform bytes, up to 16 vertex attributes, six interstage variables, four sampled textures/samplers, and bounded dimensions up to 4096. `texture-compression-bc` was available but is not required because uploads use tested CPU decompression.

## Next blockers

One private captured shader pair compiles offline after the required RCC extension; its stage-0 texture metadata is synthetic. Subsequent runtime integration completed pre-menu draws and Presents, but did not establish a readable menu. Corrected real directory discovery now reaches the unsupported browser `XboxSignatureKey` data export before presentation; this requires a native-parity audit, not a proven user-supplied key prerequisite. See [M4 evidence](../../docs/browser-port/m4-evidence.md). Further runtime pipeline and draw acceptance remains necessary. The 68 public synthetic variants are not an actual-title variant count. Capture further rejected states privately and extend only the required operations with native/Wasm and numerical GPU checks. Named capability boundaries remain ILU2/4-7 and unsupported vertex outputs, PS texture/compare modes and final flags, fog/lighting/generated coordinates, and unsupported texture resource/sampling classes. Do not claim M4 acceptance from these tests alone.
