# C01: Hook foundation

Class: Complex
Depends on: none
Native sources: src/kernel/kernel.h, src/xdk/hle.h, src/xdk/d3d8.h, src/xdk/d3d8.cpp (d3d:: helpers and fog latch), generated/game/image_symbols.h, src/app/app.h, src/app/settings.cpp, src/app/focus_pause.cpp, src/core/log.h, src/game/magma_time.cpp (proof hook), src/game/intro.cpp (settings proof), src/cpu/x86_ops.h

## Goal

Let native game hooks from `src/game/*.cpp` compile unchanged into the browser story module and run correctly on `browser::Memory` in both the compact (checked) and flat memory builds. Provide the four pieces every later parity chunk needs: a memory and call helper layer with the native names, symbol macros, an `app::Settings` shim, and a live host settings channel from `settings.mjs` into the guest runtime. Prove the path with `magma_time.cpp` end to end, and prove the settings channel with `skip_intros` (guest side) and `pause_on_focus_loss` (host side).

## Native behaviour

- kernel.h:57-81: `extern uint8_t* g_base`; `G<T>(va)` returns a raw `T&` at `g_base + va`; `R8/R16/R32/R64`, `W16/W32/W64` memcpy through `g_base`; `HostPtr(va)` returns `g_base + va` with no size. Every native hook assumes one flat 4 GiB mapping.
- kernel.h:88-98: `Args` reads stack arguments with `R32(esp + 4 + 4*i)`; `Return` sets eax and pops; `CallGuest(ctx, fn, args)` (implemented in src/cpu/dispatch.cpp:188) pushes arguments and runs guest code on the current thread, keeping callee registers.
- hle.h:2-10: includes `image_symbols.h` and `kernel/kernel.h`; `SYM(name)` is `0x` + `SYM_name`, `GUEST(name)` is `sub_` + address (the override, if any), `GUEST_ORIG(name)` is `__imp__sub_` + address. hle.h:13-35 add `HLE_STDCALL/FASTCALL/CDECL`; hle.h:38-48 add `ArgF` and `GF(va)` (float via `HostPtr`).
- Hook shape (x86_ops.h:66-67): `X86_EXTERN_FUNC(__imp__sub_X)` declares the recompiled body; a strong `X86_FUNC(sub_X)` replaces the weak alias emitted by tools/recomp/codegen_emit.cpp:98,109. On Emscripten the weak alias is `__attribute__((weak, alias))` (x86_ops.h:47-49).
- app.h:32-74: `app::Settings` with `skip_intros` (app.h:62, default false) and `pause_on_focus_loss` (app.h:63, default true) among about 40 fields; `app::Get()` returns a mutable process singleton (settings.cpp:62-65) filled from scda.toml by `Load()` (settings.cpp:67-137, the two booleans at 110-111). Readers access fields with no synchronization.
- focus_pause.cpp:28-46: on each `D3DDevice_Swap` (d3d8.cpp:401-402) the game thread sleeps while the window is in the background if `pause_on_focus_loss` is set and no net session runs, holding guest clocks (`kernel::HoldGuestTime`) and audio.
- intro.cpp:26-36 hooks front end PlayVideo `sub_00039AD4` (thiscall, 5 arguments): when `skip_intros` and the name equals the FString at `0x651340 + 0x04` (LogosVideo), returns 0 and pops 0x18. intro.cpp:39-51 hooks ShowScreen `sub_00037274`: skips a path ending in `esrb.tga`.
- magma_time.cpp:11-24 hooks ProcessTick(float seconds) `sub_002303AF`: carries the fraction that the guest's `ftol(seconds * 1000)` drops, so Magma menus advance exact whole milliseconds. At 60 FPS the guest otherwise sees 16 ms per 16.67 ms frame (4% slow menus).
- Lifetime: native relaunches the process between front end and mission (kernel.h:191-193, `Relaunch`), so file-scope hook statics live for one image run.

## Browser today

- Generated code already reaches memory through `X86_MEM*` / `X86_PTR` with `ctx` and `base` in scope: `FlatReference`/`FlatPointer` under `X86_FLAT_MEMORY`, `browser::Access`/`Pointer` under `X86_COMPACT_MEMORY` (x86_ops.h:16-41). `base` is always `Memory::Base()` (the flat page table, cpu_runtime.h:252) and `Memory::From(base)` recovers the owner (cpu_runtime.h:253) in both modes.
- Public memory API usable from hooks without touching cpu_runtime.h: `Read/Write(ctx, va, ptr, size)` (cpu_runtime.h:232-233; flat fast path cpu_runtime.cpp:493-500 and 526-534, otherwise region lookup and faults), `Span(ctx, va, size)` (cpu_runtime.cpp:445-456, requires a lease, faults on a noncontiguous span, marks physical writes), `Runtime::From(ctx)` and `Runtime::Call(ctx, base, target, args)` (cpu_runtime.cpp:1018-1029, saves and restores the same registers as native CallGuest). A guest worker holds an `Execution` lease while running generated code (cpu_runtime.cpp:59-67, 945). Write marks only matter for physical (GPU visible) pages (cpu_runtime.h:116-124).
- Existing host overrides use the same strong-symbol trick: mission-input.cpp:9-30 (`sub_0004B525`, `sub_00039AD4`, `sub_001D2A4E`, `sub_001CDA8A`), loading-io.cpp:5-6 (`sub_004B8EC0`), menu-route.cpp:5-6 (`sub_0018697A`), sdk-provider.cpp:46-75. Four of these addresses are also hooked natively: mouse.cpp:307, intro.cpp:27, bink_io.cpp:20, exit_menu.cpp:127. Compiling those native files as they are gives duplicate strong symbols.
- Build: story_boot_link has an explicit source list (tools/browser/CMakeLists.txt:200-241), `X86_COMPACT_MEMORY` always and `X86_FLAT_MEMORY` with `BROWSER_FLAT_MEMORY` (205-207), `-O3` (241). generated/game is on the include path, so `image_symbols.h` resolves. inventory.mjs:101-161 hashes every input into the build identity; nothing from src/game, src/app, src/core or src/xdk is listed.
- Shared host words: `StoryState` (boot-wasm.cpp:54-72) owns blocks that survive `image_restart` (boot-wasm.cpp:467-495 recreates `Session` only). JS gets offsets through `image_*_address` exports (boot-wasm.cpp:582-633) read by `readInputDescriptor` (boot-session.mjs:213-252), posted as the `input` phase before `_image_execute` (boot-session.mjs:414-423), and consumed on the main thread (menu-runtime.mjs:103-133). Boot time options travel in the worker start message, as `developmentStart` does (boot-session.mjs:306, 331, 380).
- Pause: mission-controls.mjs:24-36 sets motion word 2 when `!focused || document.hidden || suspended`; the mission timer source spins while word 2 is set and subtracts word 4 inactive time (boot-wasm.cpp:118-140). Focus loss therefore always pauses today.
- Settings: settings.mjs:1-10 (`scaling`, `filter`, `volume`, `sensitivity`, `invertY`, `developmentStart`, `showFps`, `showDiagnostics`); Options tabs at menu-shell.mjs:10-27. No value reaches the wasm guest; sensitivity and invertY are applied in JS (menu-shell.mjs:301-305).

## Tasks

1. Hook compat headers in `src/browser/hook-compat/`, placed BEFORE `src` on the include path of hook sources only, so `#include "kernel/kernel.h"` inside src/xdk/hle.h resolves to the shim and hle.h itself is reused unchanged.
   - `kernel/kernel.h`: `kernel::R8..R64`, `W16..W64` via `Memory::Read/Write`; `G<T>(va)` as `*reinterpret_cast<T*>(Span(ctx, va, sizeof(T)))`; `HostPtr(va)` as `Span(ctx, va, 1)` (contiguous to the end of the guest allocation because a region is one host vector, cpu_runtime.h:52-60; only the first page is write marked); `Args`, `Return`, `CallGuest` over `Runtime::Call`; `ReadCString`; the status enum. Fault diagnostics use a static empty context. No declarations of the native kernel object model.
   - Bound memory: `browser::hooks::Bind(Memory&)` / `Unbind()` called from `Session` construction and destruction in boot-wasm.cpp, storing `Memory::Base()` in an atomic pointer read by the helpers. Throw a `Fault` if a helper runs unbound.
2. Contract test first: `tests/browser/hook-compat.cpp` + `.test.mjs` in the loading-io.test.mjs pattern (native clang++ with ASan/UBSan and `-DX86_PORTABLE_RUNTIME`, plus em++), run in compact and flat variants. Cover R/W round trips, `G<float>` lvalue writes, `HostPtr` memcpy of 40 bytes inside one allocation, a fault on an unmapped address, a physical page write version bump through `W32` and `G`, `CallGuest` register preservation and stdcall cleanup, `Args`/`ArgF`/`GF`. Confirm it fails (headers absent), then implement.
3. Log shim: `src/browser/hook-log.cpp` defines `logx::Write/Flush/Path` (src/core/log.h) by formatting into stderr with a `[game]` prefix, dropping `Debug` in release builds and rate limiting identical lines. src/core/log.cpp is not compiled.
4. Hook selection in CMake: cache list `BROWSER_GAME_HOOKS` (default `magma_time;intro`) builds an OBJECT library `story_hooks` from `${root}/src/game/<name>.cpp`, with `target_include_directories(story_hooks BEFORE PRIVATE ${root}/src/browser/hook-compat)`, browser_flags, generated/game, the same memory definitions, `-O3`, `-msimd128` and `-mtail-call` conditions as story_boot_link, linked into story_boot_link with hook-compat sources and `hook-log.cpp`. Hooks require `BROWSER_MENU`. A later image target (C13) instantiates the same function with its own symbol directory.
5. Override conflicts by rename chaining: when a selected game file hooks an address that a browser file already owns, compile that game file with `sub_XXXXXXXX=game_sub_XXXXXXXX` as a per-source definition. The browser override calls `game_sub_XXXXXXXX(ctx, base)` instead of `__imp__sub_XXXXXXXX`; the browser file also defines `extern "C" __attribute__((weak)) void game_sub_XXXXXXXX(...)` forwarding to `__imp__` so it links when the game file is not selected. Keep the table of renames next to `BROWSER_GAME_HOOKS` (today: `00039AD4`, `0004B525`, `004B8EC0`, `0018697A`).
6. Inventory (tools/browser/inventory.mjs): append `hooks=<sorted list>` to the configuration string passed at CMakeLists.txt:98-100; hash each selected game source, `src/xdk/hle.h`, `src/app/app.h`, `src/core/log.h`, `src/game/magma.h`, every hook-compat file, host-settings sources. Add validators with failing tests first in tests/browser/inventory.test.mjs: (a) a strong `X86_FUNC(sub_X) {` defined twice across compiled browser and hook sources without a rename fails; (b) a selected game file whose `#include "..."` is not on the allowlist (`xdk/hle.h`, `xdk/d3d8.h`, `gpu/state.h`, `app/app.h`, `core/log.h`, `game/magma.h`, plus headers shadowed in hook-compat) fails with the header name.
7. `app` shim: compile src/app/app.h unchanged; `src/browser/host-settings.cpp` defines only what selected hooks reference (the link runs with `-sERROR_ON_UNDEFINED_SYMBOLS=1`, so a missing one is a link error, not a silent default): `app::Get()`, `SettingsRevision()`, `Aspect()` returning 4/3 and `FovScale()` from `fov` (the guest frame is a fixed 640x480, so `Narrow()` is 1 and `Widescreen()` false). `app::Get()` returns a thread local `Settings` refreshed when the channel revision changes; guest writes to it stay thread local (see open questions).
8. Host settings block (`src/browser/host-settings.h`): `std::array<std::atomic<uint32_t>, 64>` plus a 4096 byte area, owned by `StoryState` so it survives restarts. Word 0 revision (odd while writing), word 1 schema version, word 2 bool flags, words 3-63 typed scalars (`u32` or `f32` bit pattern), byte area for string and map fields (`language`, `keys`, `pad_buttons`) as `name=value` lines. Exports `image_settings_address()` and `image_settings_words()`; add the span to `readInputDescriptor` overlap checks.
9. `src/browser/host-settings.mjs` holds the single schema (app::Settings field name, word or bit, type, settings.mjs key) and exports `encodeHostSettings(values)` and `createHostSettingsWriter({ buffer, byteOffset, settings })` (seqlock writes: `Atomics.add` revision to odd, store, add to even, subscribe to `settings.subscribe`). The C++ header carries matching `constexpr` indices; a node test parses the header and compares it to the schema.
10. Boot and live flow: `createMenuRuntime` passes `hostSettings: encodeHostSettings(settings.snapshot())` beside `developmentStart`; boot-session.mjs validates it and writes it into the block before `_image_bind` and every `_image_execute`; the main thread starts a writer on the `input` phase and disposes it on `restarting` (menu-runtime.mjs:103-133). Add the module to the boot and menu module copy lists (CMakeLists.txt:88, 263).
11. Proof hook: select `magma_time`. Contract test `tests/browser/magma-time.cpp` compiles the real src/game/magma_time.cpp through hook-compat with a fake `__imp__sub_002303AF` that records `int32_t(seconds * 1000)` truncation. With 600 calls of 1/60 s the recorded sum must be 10000 ms and every value 16 or 17; without the source the sum is 9600 and the test fails first.
12. Settings proof, guest side: select `intro` with the `00039AD4` rename; mission-input.cpp's `sub_00039AD4` calls `game_sub_00039AD4` when development start is off. Contract test: with `skip_intros` set through the block, PlayVideo of the LogosVideo name returns 0 and pops 0x18 without the original; another name reaches the original; ShowScreen `...esrb.tga` is skipped.
13. Settings proof, host side: mission-controls.mjs takes `pauseOnFocusLoss()`; a blur pauses only when it is true, `document.hidden` and `suspended` always pause. Tests first in tests/browser/mission-controls.test.mjs.
14. Static lifetime: document in this file's successor notes (and enforce in review) that browser hook statics outlive `image_restart`; every chunk lists its statics and why they are restart safe, or keys them by a `browser::hooks::Generation()` counter bumped in `Bind`.
15. Diagnostics: report `hooks: [{ name, address, bound }]` where `bound` checks `runtime.Find(address) == sub_X` (the sdk-provider.cpp:114-116 pattern) under Diagnostics, plus the settings revision.
16. Shared `d3d::` shim for renderer chunks (C09 shadows, C10 glow and HDR, C11 ambient occlusion; hud.cpp, fov.cpp and shadows.cpp also call it). Native interface (xdk/d3d8.h:8-17): `gpu::State& State()`, `std::recursive_mutex& Lock()`, `Occlusion(const float* m, bool gtao)`, `FilterShadows(const float* m)`, `ApplyShadows()`, `ViewScale(float)`; implemented at d3d8.cpp:161-188 over the HLE's `g_state`, `g_lock` and `g_backend`.
   - Place: `src/browser/hook-compat/xdk/d3d8.h` (same declarations, shadows the native header for hook sources) and `src/browser/hook-d3d.cpp`, bound per session next to memory (`browser::hooks::BindGraphics(DiagnosticGraphics&)` in boot-wasm.cpp, unbound before `Session` teardown).
   - `State()` and `Lock()` return DiagnosticGraphics' live `state_` and `lock_` (diagnostic-graphics.h:78, 81) through two new accessors; today `State()` returns a 40 KiB copy (diagnostic-graphics.h:69), which hooks must not use per call.
   - Fog latch: port `IsFogPass`/`LatchFog` (d3d8.cpp:114-157: blend 0x302/0x303, a 64x1 ramp on stage 3, plane c116 against clip w row c107, alpha of PS constant 0) into DiagnosticGraphics before each `renderer_->Draw` (diagnostic-graphics.cpp:1249) and on the up draw path, and clear it at Present as d3d8.cpp:423 does.
   - `Occlusion`, `FilterShadows`, `ApplyShadows` build the same parameter arrays as d3d8.cpp:170-188 (including the projection checks that return early) and forward under the lock to `renderer_` with `state_`. MenuBackend currently throws for all three (menu-backend.cpp:1646-1648), which would abort a mission if a hook ran first. Until C09/C11 implement them, the shim checks a capability (`MenuBackend::Supports(Effect)`, false by default) and records an `unsupported effect` count in Diagnostics instead of calling. These are optional enhancements (README Excluded list), so a counted no-op is acceptable; the throws stay for direct backend misuse.
   - `ViewScale` stores the factor in DiagnosticGraphics and multiplies the viewport constant row it writes at diagnostic-graphics.cpp:576, as d3d8.cpp:98-105 does with `g_view_scale`.
   - Contract test first (`tests/browser/hook-d3d.cpp`): fog latch on a synthetic fog pass and rejection of a non parallel plane; `Occlusion` with an unsupported backend counts and does not throw; with a fake supporting backend it receives the 10 parameters of d3d8.cpp:173-174; `ViewScale(2)` doubles c58 x and y; `State()` aliases the live state under `Lock()`.

## Settings and UI

| settings.mjs key | Default | app::Settings field | Reaches guest |
| --- | --- | --- | --- |
| `skipIntros` | false (settings.cpp default) | `skip_intros` | flag bit 0; read at boot by intro.cpp, applies from the next boot |
| `pauseOnFocusLoss` | true | `pause_on_focus_loss` | flag bit 1 for parity; enforced in mission-controls.mjs |

Add a `Game` tab to `TABS` (menu-shell.mjs:10-27) after Controls with rows `Skip intros` and `Pause when unfocused`. Later chunks add their rows there and their fields to the schema. `developmentStart` stays on the Developer tab; it skips every video (mission-input.cpp:186-193) and is not the same as `skipIntros`.

## Acceptance criteria

- A boot build in a new directory (for example `out/boot-hooks`) links with `BROWSER_GAME_HOOKS=magma_time;intro`, in both flat (mission default) and compact (`-DBROWSER_FLAT_MEMORY=OFF`) modes, without editing cpu_runtime.h, x86_ops.h or generated code.
- No native source in src/game, src/app, src/kernel or src/xdk changes. If a later chunk needs one, it is behaviour neutral on Windows and flagged for a Windows build by the user.
- Diagnostics list `magma_time` and `intro` hooks as bound.
- With `skipIntros` on, the logos video and rating screen do not appear and the console shows `[game] intro: skipped the logos video`; with it off, boot is unchanged.
- With `pauseOnFocusLoss` off, world time keeps advancing while another window has focus; a hidden tab still pauses.
- All new and existing node and contract tests pass; the build identity changes when the hook list or any hook source changes.
- The `d3d::` shim exists with the native signatures: a hook source including `xdk/d3d8.h` compiles and links unchanged; in a mission build with a temporary test hook calling `d3d::Occlusion` and `d3d::FilterShadows` every frame, the mission runs without a fault and Diagnostics shows the unsupported effect count rising; the fog latch values for the opening scene are visible in Diagnostics and nonzero where the scene has distance fog.

## Testing

- Unit tests first, each confirmed failing before the implementation: hook-compat (task 2), inventory validators (task 6), host-settings schema, encoder and seqlock writer with a fake SharedArrayBuffer and a torn write retry (task 9), a C++ reader test with two threads reading `app::Get()` while JS-equivalent writes run (native and wasm), magma-time (task 11), intro (task 12), settings.test.mjs for the two keys, mission-controls.test.mjs (task 13).
- Run with `node --test tests/browser/*.test.mjs`.
- Browser checks with agent-browser on a separate port (never 8125 or out/boot-m5), audio muted with `--init-script out/front-end/mute.js`: boot to the menu with `menuShell.settings.set("skipIntros", true)`, check console lines and that the menu is reached; toggle the setting while the menu runs and check the revision in Diagnostics; with development start reach the mission, blur the window with `pauseOnFocusLoss` false and confirm world time advances in Diagnostics.
- Perf: run `node out/perf-60/bench.mjs <web> <port> <out.json> hooks` (muted) against a baseline build from the same commit; opening, control and 60 s walk means must stay within 0.3 ms of baseline (06-playable-mission.md table: 16.0 / 16.4 / 16.1 ms).

## Risks and open questions

- `HostPtr` has no size. Native callers rely on contiguity within one object, which a region guarantees, but only the first page is write marked. Any hook that writes GPU visible (physical) memory through `HostPtr` across a page needs an explicit sized helper.
- `G<T>` references stay valid only while the lease holds; a `CallGuest` between taking and using the reference may pause execution (cpu_runtime.cpp:73-86). Audit hooks that keep a `G<>` reference across `CallGuest` (fpsfixes.cpp:132-163 does).
- Fault messages from helpers lack the guest register context because the shim does not change cpu_runtime.h to expose the worker context. Acceptable, or add an accessor and pay the 8 minute rebuild once.
- Guest writes through `app::Get()` (options_menu.cpp in game pages) need a reverse channel; not designed here.
- Whether the rename chaining for `0004B525` and `0018697A` keeps C03 and the exit menu chunk ordering correct must be decided by those chunks.
- `skip_intros` timing: does the browser boot path reach `sub_00039AD4` for LogosVideo before the main thread could change the value? The worker writes it before execution, so only live toggles race, which is fine.
- The versus image (C13) needs its own symbol header and `Bind` per module.
- `d3d::Lock()` is DiagnosticGraphics' recursive `lock_`, also taken by every guest D3D call. A hook holding it while calling guest code that reaches the D3D HLE stays on the same thread (recursive), but a hook holding it across a wait on another guest thread can deadlock. Hooks must not hold it across `CallGuest` into code that waits.
- Where the browser applies the c58 viewport row for draws issued between two `SetViewport` calls (diagnostic-graphics.cpp:576) must be confirmed so `ViewScale` takes effect on the next draw, not the next viewport change.

## Out of scope

Porting any game hook beyond `magma_time.cpp` and `intro.cpp`; implementing occlusion, soft shadow filtering or HDR in MenuBackend (C09 to C11); core/input.h and app/translations.h adapters and any direct `gpu::Backend` access beyond the `d3d::` shim (owned by the chunks that need them); the F10 imgui panel, console, splash and handover (src/app/menu.cpp, splash.cpp, handover.cpp); toml files; native build changes.

## Implementation notes

What C01 built, for the chunks that follow.

### Adding a hook

1. Add the file name (without `.cpp`) to `BROWSER_GAME_HOOKS` (tools/browser/CMakeLists.txt). The default is
   `magma_time;intro`. Hooks are compiled only into `BROWSER_MENU` builds; inventory rejects them otherwise.
2. Configure. inventory.mjs checks the includes against the allowlist and every strong `sub_X` (also `GUEST(name)` and
   `HLE_*`) against the browser overrides, then writes `hook_manifest.h`, which Diagnostics reads.
3. If the address already has a browser override, add `address:owner` to `browser_hook_renames` and make the owner
   call `game_sub_<address>` through `BROWSER_GAME_CHAIN(address)` (src/browser/hooks.h).
4. Add a contract test that compiles the real source through `tests/browser/hook-contract.mjs` (native and wasm,
   compact and flat).

### Where things are

| Piece | Files |
| --- | --- |
| Native helper names over `browser::Memory` | `src/browser/hook-compat/kernel/kernel.h`, `src/browser/hook-compat.cpp`, `src/browser/hooks.h` |
| `logx` to stderr with `[game]`, relayed to the page console | `src/browser/hook-log.cpp`, boot-session.mjs `phase: 'log'` |
| `app::Get()` and friends, host settings block | `src/browser/host-settings.{h,cpp,mjs}` |
| `d3d::` shim | `src/browser/hook-compat/xdk/d3d8.h`, `src/browser/hook-d3d.cpp`, `src/browser/hook-graphics.{h,cpp}` |
| Hook list, Diagnostics | `hook_manifest.h` (generated), `image_hook_*` exports, `evidence.hooks`, `evidence.hookStatus`, `evidence.settings` |

### Changes from the plan

- The host settings block (`host_settings::Shared()`) is one per wasm module, not a `StoryState` member, because
  `StoryState` only exists in mission builds. It survives `image_restart` in the same way.
- The worker writes the boot snapshot once, before `_image_bind`, not before every `_image_execute`. The block keeps
  the page's live values across a restart; writing the boot snapshot again would undo changes made since the boot.
  Writers take the block with a compare exchange from an even to an odd revision, so the worker and the page can never
  write it at the same time.
- `browser::hooks::BindGraphics` takes the `HookGraphics` that `DiagnosticGraphics` owns (`Hooks()`), not the
  `DiagnosticGraphics` itself, so the fog latch, view scale and effect forwarding are tested without the graphics
  fixture. `DiagnosticGraphics::State()` still returns a copy; hooks get the live state through `d3d::State()`.
- The fog latch runs on the one draw path (`renderer_->Draw` for DrawVertices and DrawIndexedVertices); there is no
  separate up draw path in DiagnosticGraphics. Diagnostics show the fog latched by the last presented frame, because
  the latch is cleared at Present as native does.
- `kernel::R8` exists in the shim although native has none; native hooks cannot use it.

### Static lifetime

Hook statics live for the whole wasm module, across `image_restart`, while native statics live for one image run.
Every chunk lists its statics here and why they are restart safe, or keys them by `browser::hooks::Generation()`.

| Static | Restart safe because |
| --- | --- |
| `magma_time.cpp` `carry` | Under 1 ms of frame time carried into the next run's first menu tick |
| `intro.cpp` | None |
| `host-settings.cpp` thread local `Settings` and seen revision | Each guest thread is a new pthread with fresh thread locals; the revision check refreshes them from the block either way |
| `hook-compat.cpp` bound memory, `hook-d3d.cpp` bound graphics | Rebound by every `Session`, unbound after its runtime stops |
| `hook-graphics.cpp` `HookStatus()` | Diagnostics only, rewritten at every Present |
| `hook-log.cpp` repeat counts | Logging only |

### Fog latch in the first mission

`evidence.hookStatus.fog` also reports `passes` (source alpha blended draws with a stage 3 texture) and `ramps`
(those whose stage 3 texture is the 64 x 1 ramp) for the last presented frame. In the opening of the Iceland mission
and the first minute of play there are 20 to 70 such passes a frame, all with 32 x 32 or 64 x 64 textures on stage 3
and none with the ramp, so the latch stays zero there. Check a scene with visible distance fog before C11 relies on it.
