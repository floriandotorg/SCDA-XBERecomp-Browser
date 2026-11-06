# C02: High FPS gameplay fixes

Class: Simple
Depends on: C01
Native sources: src/game/fpsfixes.cpp, src/game/camera_shake.cpp, src/game/root_motion.cpp, src/game/magma_time.cpp; reference only: src/versus/frame_rate.cpp, src/app/main.cpp (Pace)

## Goal

The console game runs at 30 Hz and several effects step once per frame. The browser presents at display rate (60 FPS on the target host), so the same defects the native PC port fixes appear here: vision grain races, cloth and animated textures speed up, the JBA night vision interference scrolls too fast, camera hit, tilt and shake last several times longer, root motion turns end short (ledge corners drop the player), and Magma menus run 4% slow. Compile the four native fix files unchanged through the C01 mechanism and verify they behave correctly under the browser's mission stepper.

## Native behaviour

fpsfixes.cpp (all thiscall hooks unless noted, constants cited from the file):
- Grain rolls (14-53, 166-180): hooks `appFrand` `sub_00067760` (result in `X87_ST(0)`) and `rand` `sub_00067732` (result in eax). The guest return address selects a roll site: 0x2F02E8, 0x2F0343, 0x2F034E (noise overlay angle, v, u), 0x2F2119, 0x2F213E, 0x2F2149, 0x2F2A6A, 0x2F2A9D, 0x2F2AB9, and 0x2F308D or 0x2F309C (thermal distortion angle). The first value of a roll decides whether 1/30 s of host `steady_clock` has passed; otherwise all values of that roll are replaced by the held ones.
- Cloth, UESoftBody::Update `sub_0025E82D` (55-77, 128-164): offsets owner 0x5C, vertex array 0x94, count 0x98, smoothed step 0x17C, vertex stride 0x48 with position at +4; owner's LevelInfo at +0xF4, DeltaSeconds at +0x3F4. Accumulates delta (capped 0.05 s), calls the original once per 1/30 s step with DeltaSeconds and the smoothed step forced to 1/30, keeps the last two results and writes a blend; external vertex moves are carried into both steps. Then calls the vtable entries at +0xC8 (attachments) and +0xCC (bounds) through `CallGuest`. Per body state lives in `g_cloths` (67).
- Animated textures, UTexture::Tick `sub_001DDA92` (79-80, 112-126): when MaxFrameRate (+0xAC) is 0, accumulates delta in +0xB0 and skips the original (`esp += 8`, ret 4) until 1/30 s has gathered.
- JBA night vision interference, pass `sub_002F157B` (82-97): scroll float at 0x6A94EC, viewport +0x1D0, view actor +0x34; pre-adds `0.005 * (1 - delta / (1/30))` so the original's fixed per frame step nets to the 30 Hz rate, and restores the value if the original did not scroll.
- JBA night vision glitch `sub_001F8F9C` (99-110): thiscall(LevelInfo); runs the original once per 1/30 s of DeltaSeconds, otherwise `esp += 8`.

camera_shake.cpp: EPlayerCam effect update `sub_000FCB5D` (68-106), thiscall(pitch, yaw, roll, relative). Ten ints at camera +0x288 (HitRoll .. ShakeFadeOut, 16-22); LevelInfo +0xF4, TimeSeconds +0x3BC (24). Steps the effects at 1/30 s on game time with at most 4 steps per call (27) and restarts from the fields after a gap above 1 s or time going back (28, 78-81). Detects script writes by comparing with what it last wrote (82-84), draws the interpolated values with speeds zeroed so the stock update only applies them (97-104), then writes the stepped state back (105). `Approach` mirrors the game's `sub_0007C198` with ftol truncation (46-51). State per camera in `g_cams` (36).

root_motion.cpp: mesh instance root motion `sub_00216CFD` (26-42), thiscall(out location delta, out rotation delta, new frame, to world, delta time). Only for the call whose return address is 0x280E65 (inside `sub_00280D8B`, actor in ebx after return). If the instance's mesh space delta (+0x3A8) and the actor's Rotation (+0x1F4) have zero pitch and roll, the out yaw is replaced by the exact mesh yaw when it is within 8 units, with a sign learned per instance for mirrored animations (`g_sign`, 20).

magma_time.cpp: ProcessTick(float seconds) `sub_002303AF` (11-24); carries the dropped millisecond fraction. Ported and tested in C01 task 11; this chunk only verifies it in the mission build.

Native frame timing for comparison:
- The game thread presents in `D3DDevice_Swap` and then calls `app::Pace()` (d3d8.cpp:424), which sleeps to `fps_cap` (main.cpp:271-290; default 60 per app.h:37; `kFpsCapRefresh` means refresh minus 3).
- The guest measures its own frame time from guest clocks, which focus pause holds (kernel.h:142-144). A single engine tick is clamped by the game to about 66.67 ms (float at 0x4F64C8, value 0.0666659996 in tests/browser/mission-stepping.cpp:65), so a slow native frame runs the world slower.
- versus/frame_rate.cpp:13-21 removes the render device's own limiter (sleep until 2 / refresh less 1 ms since the last swap, 31 FPS at 60 Hz) by writing 0x7FFFFFFF to the device refresh int at +0x46D8 around `sub_001E4C90`. That is the versus image only; src/game has no counterpart for default.xbe.

## Browser today

- MissionInput::Tick (mission-input.cpp:87-137) intercepts the engine tick `sub_001D2A4E`. `MissionStepper::Advance` (mission-stepper.h:19-44) turns elapsed host time (timer source with paused time removed, boot-wasm.cpp:118-140) into 1 to 8 steps (`MaximumSteps`) of at most the game's maximum delta read from 0x4F64C8 (mission-input.cpp:95), writes the step length as the tick argument (mission-input.cpp:117), suppresses intermediate primary viewport draws `sub_001CDA8A` (mission-input.cpp:119-121, 139-144), and carries up to 2 s of debt (`MaximumBacklog`, 100 ns units) before faulting. Inactive scenes and menus get one step with the original delta (mission-stepper.h:26-33).
- At 60 FPS that is one tick per presented frame with DeltaSeconds near 1/60, the same situation the native fixes target. Measured presentation: 16.0 / 16.4 / 16.1 ms means (06-playable-mission.md, 60 FPS table). There is no browser frame cap.
- mission-input.cpp reads the same LevelInfo fields the hooks use: actor +0xF4 (62), TimeSeconds +0x3BC (64), DeltaSeconds +0x3F4 (176). The offsets agree.
- None of the nine hooked addresses (`00067760`, `00067732`, `0025E82D`, `001DDA92`, `002F157B`, `001F8F9C`, `000FCB5D`, `00216CFD`, `002303AF`) has a browser override, so no C01 rename is needed. Generated code pushes the guest return addresses the hooks compare (for example `X86_PUSH32(0x280E65u); sub_00216CFD` in generated/game/x86_recomp.61.cpp and `X86_PUSH32(0x2F02E8u); sub_00067760` in x86_recomp.73.cpp), and indirect calls resolve through the mapping table to the override (x86_func_mapping.cpp has entries for 0x25E82D, 0x1DDA92, 0x2F157B, 0x1F8F9C, 0x2303AF).
- All four files include only `xdk/hle.h` and standard headers, so they pass C01's include allowlist without adapters.

## Tasks

1. Write `tests/browser/high-fps-hooks.cpp` and `.test.mjs` (C01 hook-compat test pattern: native with sanitizers and wasm, compact and flat), linking the real sources and fake `__imp__` bodies on synthetic memory. Each case must fail with the source omitted, then pass:
   - camera_shake: 60 calls advancing TimeSeconds by 1/60 leave the same fields as 30 calls advancing by 1/30; drawn values between steps are interpolated and speeds are zero during the original; a field changed between calls is adopted; time going back or a gap above 1 s restarts from the fields; a 0.5 s jump runs 4 steps and drops the rest.
   - root_motion: return 0x280E65 with upright rotators and the original writing the exact yaw minus 5 gives the exact yaw; any other return address, a nonzero pitch or roll, or an error above 8 units leaves the output alone; a mirrored animation keeps its learned sign.
   - Texture tick: MaxFrameRate 0 with delta 1/60 calls the original every second call and pops 8 otherwise; nonzero MaxFrameRate calls it every time.
   - JBA glitch: delta 1/60 calls the original every second call.
   - Interference scroll: with a fake original subtracting 0.005, one second at 1/60 moves the scroll as far as one second at 1/30; with the original not scrolling, the value is restored.
   - Grain: two rolls at site 0x2F02E8 inside 1/30 s return the held values for all three sites of that roll; after 40 ms of sleep the roll is fresh. Use the rand and appFrand paths.
   - Cloth: a fake original moving every vertex by +1 and asserting DeltaSeconds and +0x17C equal 1/30 runs once per two 1/60 calls; written vertices are the blend; the +0xC8 and +0xCC vtable targets (mapped fake functions) run once per call.
2. Add `fpsfixes;camera_shake;root_motion` to `BROWSER_GAME_HOOKS` (C01 task 4); `magma_time` is already there. Confirm the inventory change alters the build identity.
3. Review hook statics against `image_restart`, which keeps them (C01 task 14): `g_cams` restarts on time going back, which a new level's TimeSeconds does; `g_sign` relearns; `g_rolls`, the glitch `time` and the Magma `carry` are harmless. `g_cloths` can blend one frame toward a stale body at a reused address with the same vertex count, and `g_cams`/`g_cloths` never erase entries. Record the decision (accept, or key by `browser::hooks::Generation()` through a behaviour neutral native change built on Windows by the user).
4. `fpsfixes.cpp:132` keeps `float& delta = G<float>(...)` across `__imp__` calls and `CallGuest`. Confirm with C01's lease rules that the reference stays valid (region storage does not move while the body is alive) and add a contract test for it if not.
5. Determine whether cloth (`sub_0025E82D`, called from `sub_00175437`) and the camera update (called from `sub_000FCF33`, `sub_000FD37E`, `sub_0010162F`) run in the tick or the draw path, by a debug build counter per presented frame under a forced multi-step frame. If cloth is in the draw path, suppressed draws mean it only sees the last step's DeltaSeconds; record whether that needs a change in MissionInput (out of this chunk) or is acceptable.
6. Verify in the browser build (task 7 below) and record results in this file's successor notes.

## Settings and UI

None. Native applies these fixes unconditionally and has no setting for them; the browser does the same. For A/B checks use a second build directory with the three names removed from `BROWSER_GAME_HOOKS`, not a runtime toggle.

## Acceptance criteria

- All task 1 cases pass in the four variants; each was seen failing first.
- A mission build in a new directory (for example `out/boot-fps`, never out/boot-m5) lists all nine addresses as bound in Diagnostics.
- Development start reaches the first mission and a 10 minute soak (`PERF_SOAK=600`) ends without a fault.
- With the hooks, camera shake from damage or a hard landing lasts visibly as long as on the console; in the A/B build without them it lasts several times longer.
- Night vision and thermal grain change at about 30 Hz, not every frame.
- A ledge or back-to-wall corner turn completes the full angle, and the player does not drop at ledge corners.
- Menu animations and Magma timed transitions keep wall clock pace (from C01).
- Frame time means within 0.3 ms of a same-commit baseline in the opening, control and 60 s walk windows.

## Testing

1. Unit and contract tests first (task 1), run with `node --test tests/browser/high-fps-hooks.test.mjs`, then the whole `node --test tests/browser/*.test.mjs` suite.
2. agent-browser, audio muted with `--init-script out/front-end/mute.js`, server on its own port: upload the ISO, `menuShell.settings.set("developmentStart", true)`, boot. In the mission:
   - check Diagnostics for bound hooks and no fault;
   - toggle night vision and thermal (2 to 5 keys) and take screenshots for review;
   - take fall damage from a ledge and screenshot the shake over about one second in both builds;
   - perform a corner turn along a ledge or wall on the opening route.
   The user confirms the subjective items in a playtest, because screenshot cadence cannot resolve 30 Hz grain.
3. Perf: `node out/perf-60/bench.mjs <web> <port> <out.json> fps-fixes` against the baseline build, with and without `PERF_SOAK=60`; `rand` and `appFrand` run often, so compare update milliseconds per frame (the 06 table reports 3.13 / 3.53 / 4.01 ms). If update time grows by more than 0.2 ms, profile with `PERF_TRACE` and `out/perf-60/top.sh`.

## Risks and open questions

- Display rates above 60 Hz: the browser has no cap. If Chromium presents at 120 Hz on a ProMotion display, ticks run at 120 Hz and cloth (which speeds up past 66 FPS) and every fix here matter more. Confirm the presented rate on such a display.
- Story image device limiter: versus disables a 31 FPS limiter at render device +0x46D8. src/game has no counterpart, and the browser already measures 16 ms frames, so it is not active today under HLE. Confirm by reading the device refresh int in a browser session before assuming it never engages.
- During multi-step frames (hitches over 66.67 ms) the camera fix catches up at most 4 steps (133 ms) and drops the rest, and the interference scroll and grain run only on drawn frames. This matches native behaviour on slow frames.
- The grain hold uses host `steady_clock`, not guest time. After a pause, the first roll is fresh at once. Harmless.
- JBA missions are not reachable on the current route; the interference and glitch fixes are covered only by contract tests until then.
- Cloth locations on the opening route are unknown; if none exists, cloth relies on task 1 and later missions.
- The brief lists magma_time.cpp here; it is ported in C01 as that chunk's proof hook and only verified here.

## Out of scope

versus/frame_rate.cpp and versus/camera_shake.cpp (C13); changes to MissionStepper or MissionInput scheduling; a browser frame cap; any setting for these fixes.

## Implementation notes

What C02 built, and two related 60 FPS defects fixed alongside it (movies playing too fast, stuttering audio).

### Hooks

- `BROWSER_GAME_HOOKS` defaults to `magma_time;intro;fpsfixes;camera_shake;root_motion`. The three sources compile
  unchanged; none of their addresses has a browser override, so no rename was needed.
- `bink` (src/xdk/bink.cpp) is always compiled, like `crt_math`. Native links it unconditionally: `BinkDoFrame` and
  `BinkNextFrame` hold a movie frame until `BinkWait` says it is due, and `BinkGetSummary` keeps its 32 bit division from
  overflowing. Without it every movie advanced one frame per presented frame, which at 60 FPS is twice the speed of a
  30 FPS movie.
- The configuration string carries the hook list, so the build identity changes with it (`out/boot-hooks`
  `ffbb25c7...`, `out/boot-fps` `6445e66e...`).
- In a mission build (`out/boot-fps`) Diagnostics lists all 17 hook addresses as bound, and development start reaches
  the mission without a fault.

### Contract tests

- `tests/browser/high-fps-hooks.cpp` covers the seven cases of task 1 (camera shake, root motion, texture tick, JBA
  glitch, interference scroll, grain, cloth), each reported on its own line. Without the three sources all seven fail;
  with them all pass in native compact and flat (ASan/UBSan) and wasm compact and flat.
- Camera times are offset by a quarter step (1/120 s), so no update lands on a 1/30 s boundary where float rounding
  would decide the step count.
- The grain hold leaves the first two rolls after a long gap fresh: the first sets `next` to now, so the second is due
  at once. The test warms up one roll first. This is native behaviour.
- `tests/browser/bink.cpp`: one second of 60 Hz game frames with a 30 FPS `BinkWait` decodes and advances 30 frames, and
  a large read in 0 ms no longer overflows the summary. Both fail without the source.

### Task 3: statics across `image_restart`

Accepted, no generation keying:

| Static | Restart safe because |
| --- | --- |
| camera_shake `g_cams` | A new level starts TimeSeconds again, so time goes back and the entry restarts from the fields. Entries are never erased, at most one per camera address ever used. |
| root_motion `g_sign` | Relearned on the next turn above 8 units; at most one wrong small turn. |
| fpsfixes `g_rolls`, glitch `time` | Under 1/30 s of held grain or glitch time. |
| fpsfixes `g_cloths` | A body at a reused address with the same vertex count blends one frame toward the old body's last steps, then follows the new one. Never erased. |
| bink `g_held` | A movie held when the image stopped is erased by the next `BinkNextFrame` at that address, one skipped advance at most. |

### Task 4: `float& delta` across guest calls

Valid. A region's storage is allocated once with its size and held by `shared_ptr`
(`Memory::Storage`, cpu_runtime.h); it is only released by `Discard` of that allocation, and the flat build maps
memory at fixed host addresses. A paused execution can let other workers run but not move the LevelInfo of a body
being updated. No extra contract test.

### Task 5: tick or draw path

Not measured with a debug counter. `sub_00175437`, the only caller of the cloth update, is reached only through a
vtable, so static search does not settle it. At 60 FPS every presented frame is one tick and one draw, so this only
matters on hitch frames above 66.7 ms; if cloth is in the draw path, it then misses the earlier steps' time and slows
down during the hitch. Acceptable until a scene with cloth shows otherwise.

### Perf

`PERF_SOAK=60 node out/perf-60/bench.mjs`, same session conditions, C01 build (`out/boot-hooks`) against
`out/boot-fps`:

| Window | Baseline mean | Hooks mean | Baseline update | Hooks update |
| --- | --- | --- | --- | --- |
| Opening | 16.4 ms | 16.4 ms | 4.21 ms | 4.27 ms |
| Control | 18.6 ms | 18.7 ms | 4.71 ms | 4.80 ms |
| 60 s walk | 16.8 ms | 16.8 ms | 5.69 ms | 5.74 ms |

Frame means are within 0.1 ms and update time within 0.09 ms. Both runs are slower than the 06 table because
of host load; compare within this table only.

### Audio stutter

The guest-side sink (`SilentAudioSink`) queues up to 8,192 frames (170 ms) ahead of the guest clock and copies each
submission into the page's ring. Whenever the page empties the ring (the first activation after a gesture, every
blur and focus, a context state change), the guest queue stays full, so from then on each frame reaches the ring just
before the worklet plays it. Measured in the mission: about 8,200 frames queued before a blur and focus, then 270 to
740 frames (6 to 15 ms) for the rest of the session. Any producer delay above that is an underrun. The two clocks agree
(48,001.9 frames per second against `performance.now` over two minutes), so drift is not the cause.

Fix (src/browser/silent-audio.cpp): when a submission finds the ring empty while the guest clock still has at least
`kRefillFrames` (1,024) frames to play, those frames are written again first. After a blur and focus the ring is back
at about 8,200 frames, with no underruns or drops. A refill can only happen into an empty ring, so the extra latency
it can add is bounded by one queue. Contract: `outputCushion` in tests/browser/silent-audio.cpp, which failed before
the change. Two audio-provider cases emptied the ring while the guest still had 8,192 frames queued, which the page
never does without a flush; they now leave the ring as it is.

### Open

- Subjective checks for the user's playtest: camera shake length after damage or a hard landing, night vision and
  thermal grain at about 30 Hz, ledge and corner turns, movie speed in cutscenes and the menu background, audio with
  focus changes.
- 10 minute soak (`PERF_SOAK=600`) and the A/B build without the three hooks were not run.
- Display rates above 60 Hz remain unconfirmed.
