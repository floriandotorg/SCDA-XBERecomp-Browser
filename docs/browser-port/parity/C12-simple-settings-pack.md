# C12: Simple settings pack (anisotropy, brightness and contrast, frame cap, language, audio output)

Class: Simple
Depends on: C01 for items 1 to 3 (settings channel into the game Wasm, app::Settings shim) and item 4
(session start parameters); item 5 (audio output) needs no C01 work
Native sources: src/app/app.h, src/app/settings.cpp, src/app/menu.cpp, src/app/main.cpp, src/app/translations.cpp,
src/gpu/d3d12.cpp, src/xdk/d3d8.cpp, src/kernel/xboxkrnl.cpp, src/audio/output.h, src/audio/xaudio2.cpp, src/xdk/dsound.cpp

## Goal

Port five small native settings onto existing browser infrastructure, wire each into settings.mjs and
the Options panel, and state for each whether it applies live, at the next frame or only in a new
session. Vsync has no browser equivalent and is documented as such.

## Native behaviour (cite file:line)

1. Anisotropic filtering. `aniso` 0, 2, 4, 8, 16 (src/app/app.h:45, src/app/menu.cpp:39-40, 547-549,
   loaded at src/app/settings.cpp:90). Forced only where the title filters linearly across mip
   levels, the texture has more than one level and the stage is not a shadow compare; the title's own
   D3DTEXF_ANISOTROPIC (3) uses its `MaxAnisotropy` stage state; capped at 16:
   `StageSampler` src/gpu/d3d12.cpp:2914-2955. Read every draw, so it applies live.
2. Brightness and contrast. 0 to 100, 50 each is identity (app.h:39-40, settings.cpp:84-85,
   menu.cpp:507-513). A 256 entry ramp: contrast is an S curve around mid grey with exponent
   k = 2^((contrast - 50) / 50), brightness a gamma g = 2^((50 - brightness) / 50) applied after it;
   black and white stay fixed (src/gpu/d3d12.cpp:3891-3922). Rebuilt when the pair changes, checked
   each Present (4060-4061), applied in the final blit (3935-3947). The game's own ramp (Shadow
   Calibration) is ignored: `SetGamma` is empty (4377-4378).
3. Frame cap and vsync. `fps_cap` default 60; 0 unlimited; `kFpsCapRefresh` (1) means refresh rate
   minus 3; versus clamps to 240 (app.h:17-19, 37; menu.cpp:52-53, 514-527). `app::Pace()` sleeps
   after each Swap until the next period (src/app/main.cpp:271-290, called at src/xdk/d3d8.cpp:424).
   `vsync` picks the DXGI sync interval and tearing flag (app.h:43, d3d12.cpp:4074-4075).
4. Language. "auto" or one of `int fra deu ita esp`, the disc's folder names (app.h:29-30, 64).
   "auto" maps the Windows UI language, else English (settings.cpp:49-58, 208). The value in effect is
   latched once per run, `BootLanguage()` (settings.cpp:210-213); the menu marks a different choice
   "(RESTART)" (menu.cpp:573-583). The kernel answers `ExQueryNonVolatileSetting` index 7
   (XC_LANGUAGE) with ids int 1, deu 3, fra 4, esp 5, ita 6, and also writes it at offset 0xE8 of the
   whole EEPROM image for index 0xFFFF (src/kernel/xboxkrnl.cpp:929-966). src/app/translations.cpp
   only translates the recomp's own menu rows (lines 1-4) and has no browser counterpart, since the
   browser Options panel is HTML.
5. Audio output. AUTOMATIC, MONO, 2.0 STEREO, 5.1 SURROUND (app.h:65, menu.cpp:47-49, 585-587;
   constants src/audio/output.h:29-34). The 6 channel mix folds to stereo as
   L = FL + 0.707 C + 0.707 BL, R = FR + 0.707 C + 0.707 BR, no sub; mono hears (L + R) / 2 on both
   sides (src/audio/xaudio2.cpp:25-27, 99-117). `SetOutputMode` applies at once (205-211, re-run from
   menu.cpp:293-297). `IDirectSound_GetSpeakerConfig` returns DSSPEAKER_MONO (1) in mono, else
   stereo 0 or surround (src/xdk/dsound.cpp:859-869).

## Browser today (cite file:line; where the work plugs in)

1. Anisotropy. `MenuBackend::Draw` builds `packet.sampler[n][0..6]` (address U and V, mag, min, mip,
   min and max LOD) and rejects filters other than point and linear, so a title anisotropic filter
   would fault today (src/browser/menu-backend.cpp:1181-1196, require at 1187). The service validates
   the same ranges (src/browser/webgpu-service.cpp:549) and caches samplers by 8 words with no
   `maxAnisotropy` (43-69). `DrawPacket::sampler[4][7]` (src/browser/webgpu-transport.h:20-28, size
   asserted 236 at :44). Stages with border addressing or shadow compare sample in the shader
   (menu-backend.cpp:58-65), so a sampler setting cannot reach them.
2. Brightness and contrast. The guest ramp from `SetGammaRamp` reaches the GPU worker
   (src/browser/diagnostic-graphics.cpp:1257-1267, src/browser/menu-backend.cpp:1638-1645,
   src/browser/webgpu-service.cpp:1014-1016) and is applied per channel in the presentation shader with
   `textureLoad` (webgpu-service.cpp:474-484, 503-509; identity default 127-131). Unlike native, the
   browser honours the game's ramp.
3. Frame pacing. The canvas is configured `WGPUPresentMode_Fifo` (webgpu-service.cpp:837); `Present`
   returns with one frame in flight and blocks the next (1040-1046). The guest Swap calls
   `renderer_->Present` and returns (diagnostic-graphics.cpp:1269-1296); there is no host pacing, so the
   game runs at display rate when it can. Simulation steps are split by `MissionStepper`
   (src/browser/mission-stepper.h:15-43, fed at src/browser/mission-input.cpp:94-97 with the game's
   maximum step read from 0x4F64C8).
4. Language. `InstallKernelProvider` captures `{1, KernelVideoFlags, 0}`, so XC_LANGUAGE is always
   English (src/browser/kernel-provider.cpp:566, served by `InvokeSettings` 228-256, which answers
   only indices 7 to 9 and returns 0xC00000BB for 0xFFFF). Installed once per session at
   src/browser/boot-wasm.cpp:151. docs/browser-port/03-game-boots.md:722 states that worker messages
   and host locale cannot replace these values; this chunk deliberately changes that.
5. Audio output. The worklet renders 2 channels (src/browser/audio-worklet.mjs:10-12) with the same
   0.707 fold as native in `AudioQueue.render` (src/browser/audio-transport.mjs:34-41). Volume is a
   GainNode between worklet and destination (src/browser/browser-audio.mjs:54-60, 80-83), created by
   the shell with the volume setting (src/browser/menu-shell.mjs:307-310, live updates 452-458).
   `GetSpeakerConfig` always writes `kColdSpeakerConfig = 0` (src/browser/audio-provider.cpp:662-666,
   src/browser/audio-provider.h:25).
- Settings: src/browser/settings.mjs:1-10 (normalisation 12-20); tabs src/browser/menu-shell.mjs:10-28;
  rows render options as cycling values and numbers as ranges (menu-shell.mjs:113-170).

## Tasks (numbered, concrete, each verifiable)

Anisotropic filtering (live)
1. Add `uint32_t anisotropy[4]` to `DrawPacket` (update the static_assert) and a ninth word to the
   service sampler cache key; set `descriptor.maxAnisotropy`. Validate 1 to 16.
2. In `MenuBackend::Draw`, port the native rule: forced value when min is linear, mip is not none,
   `levels > 1`, not a shadow stage and not border addressed; title value for filter 3 using
   `kTssMaxAnisotropy`. WebGPU requires mag, min and mip all linear when `maxAnisotropy > 1`, so set
   all three to linear in the packet whenever anisotropy is above 1 (D3D12's anisotropic filter
   behaves the same way). Accept filter 3 instead of faulting. Only the packet changes; the shader key
   (menu-backend.cpp:43-68) does not, so no pipeline recompiles.

Brightness and contrast (live)
3. Add a pure function `ComposeRamp(guest[768], brightness, contrast) -> [768]` in a small browser
   header, implementing the native curve exactly, applied after the guest ramp.
4. `MenuBackend` keeps the last guest ramp from `SetGamma`; at `Present` it compares
   `brightness << 8 | contrast` with the last sent pair and re-sends `SetGamma` with the composed ramp
   when either the pair or the guest ramp changed. No shader or packet change: zero added GPU cost.

Frame cap (live) and vsync (not portable)
5. Add a pacer after `renderer_->Present` in the guest Swap path that mirrors `app::Pace()`:
   cap 0 returns at once; otherwise advance `next` by the period, resynchronise when more than one
   period late, and sleep (`std::this_thread::sleep_for` in the guest worker, which maps to
   `Atomics.wait`) until `next`. Versus clamp is not needed (story only).
6. Skip pacing when the cap is at or above the display rate: the front end measures the display rate
   with `requestAnimationFrame` and sends it with the settings; within 3 Hz of the cap, the Fifo
   back pressure already paces and a timer would only add jitter.
7. Vsync: document in the Options help text that browsers always synchronise canvas presentation
   (WebGPU canvases offer no present mode); no setting is added.

Language (new session)
8. Extend `InstallKernelProvider` with a settings argument (language id, video flags, audio flags);
   boot-wasm.cpp passes the language from the C01 session parameters. Map `int fra deu ita esp` to
   1, 4, 3, 6, 5 exactly as xboxkrnl.cpp:930-935. "auto" resolves in the front end from
   `navigator.language` (fr, de, it, es, else int) before the session starts.
9. Decide index 0xFFFF (open question 2); if the game queries it, return the same EEPROM fields as
   native (xboxkrnl.cpp:948-959).

Audio output (live for the mix)
10. In browser-audio.mjs, a `setOutput('stereo' | 'mono')` sets the GainNode's `channelCount = 1`,
    `channelCountMode = 'explicit'`, `channelInterpretation = 'speakers'` for mono (the Web Audio
    speakers down-mix of stereo to mono is 0.5 * (L + R), the native average) and back to 2 /
    'max' for stereo. The destination up-mixes mono to both sides. No worklet change.
11. Pass the mode to the game Wasm through C01 so `GetSpeakerConfig` returns 1 in mono, as native.

## Settings and UI (settings.mjs entries, defaults, Options panel placement, how they reach guest/renderer)

| Setting | settings.mjs entry | Tab | Reaches | Applies |
| --- | --- | --- | --- | --- |
| Anisotropic filtering | `anisotropy: { initial: 0, options: [0, 2, 4, 8, 16] }` | Graphics | C01 -> `app::Settings::aniso` -> MenuBackend draw packets | Live |
| Brightness | `brightness: { initial: 50, min: 0, max: 100, step: 1 }` | Display | C01 -> `brightness` -> composed ramp | Live |
| Contrast | `contrast: { initial: 50, min: 0, max: 100, step: 1 }` | Display | C01 -> `contrast` -> composed ramp | Live |
| Frame rate cap | `frameCap: { initial: 60, options: [0, 30, 60] }`, 0 shown as "Display rate" | Display | C01 -> `fps_cap` -> Swap pacer | Live |
| Language | `language: { initial: 'auto', options: ['auto', 'int', 'fra', 'deu', 'ita', 'esp'] }` | General (new) | Session parameter -> kernel provider | New session |
| Audio output | `audioOutput: { initial: 'stereo', options: ['stereo', 'mono'] }` | Audio | browser-audio GainNode; C01 -> speaker config | Live mix; speaker config at the game's next query |

- The Graphics tab is shared with C08 and C09. A General tab holds Language (labels in the language's
  own name: English, Français, Deutsch, Italiano, Español, plus Automatic).
- Rows that need a new session get a `restart: true` flag in TABS that renders "applies on next
  start" in menu-shell.mjs; reuse it for MSAA (C08).
- Default frame cap 60 matches native (app.h:37); "Display rate" restores today's behaviour.

## Acceptance criteria

- Defaults reproduce today's output, except the frame cap on displays faster than 60 Hz.
- Anisotropy 16x visibly sharpens oblique floor textures in the opening; no sampler validation errors;
  frame time change recorded.
- Brightness and contrast at 50/50 produce a byte-identical ramp to today; moving them changes the
  picture immediately, including in menus; Shadow Calibration still works on top.
- Frame cap 30 gives 33.3 ms mean presentation intervals with p95 under 36 ms; world time advances at
  real speed; "Display rate" matches the current harness numbers.
- Language French starts a new session with French menus and loading screens.
- Mono plays identical signals on both channels; switching is click free and immediate.

## Testing (unit/contract tests first where practical, agent-browser visual checks with muted audio, perf harness)

Failing tests first:
- tests/browser/settings.test.mjs: each new entry normalises, persists and resets.
- `ComposeRamp`: identity at 50/50 for an identity guest ramp; reference values at
  (0, 50), (100, 50), (50, 0), (50, 100) computed from the native formula; endpoints 0 and 255 fixed.
- Sampler rule in tests/browser/menu-backend.test.mjs: forced value only with linear min, mips and no
  shadow or border; filter 3 accepted; packets otherwise unchanged.
- Pacer: a unit test with an injected clock: 30 Hz cap yields 33.3 ms periods, a late frame
  resynchronises, cap 0 never sleeps, cap at display rate is skipped.
- tests/browser/kernel-settings.test.mjs: XC_LANGUAGE returns the configured id for each language.
- tests/browser/browser-audio.test.mjs: `setOutput` sets the gain node's channel properties.
Then agent-browser runs with out/front-end/mute.js (French title screen capture; mono check through the
audio snapshot rather than listening); perf with out/perf-60/bench.mjs at anisotropy 16 and frame cap 30.

## Risks and open questions

1. Brightness parity: native replaces the game's ramp, the browser applies it today. This plan
   composes (keeps Shadow Calibration working). Confirm with the user, or switch to replace.
2. Does the game query 0xFFFF (whole EEPROM) for the language? Native implements it; the browser
   faults it with 0xC00000BB. Check the native log line `ExQueryNonVolatileSetting %X`
   (xboxkrnl.cpp:966) for index FFFF.
3. Does the user's disc image contain all five language folders? Not verifiable here (no extracted
   game data in assets/). Grey out languages whose folders are missing.
4. The raw cache and saves may hold language specific files (docs/browser-port/raw-cache-policy.md);
   confirm a language switch does not reuse stale cached data.
5. Does the game read the speaker config only at boot? If so, mono's guest side needs a new session;
   the Web Audio mix still applies live.
6. A 60 cap on a 120 Hz display relies on timer accuracy in a worker; measure jitter. Effects that
   native holds to 30 Hz (src/game/fpsfixes.cpp) are not ported here and race above 60.
7. 5.1 surround output is not offered (2 channel worklet).

## Out of scope

- FPS counter and diagnostics overlay (already in the browser front end).
- Translating the HTML front end; native translations.cpp covers recomp menu rows only.
- Vsync off and tearing, HDR output, field of view, FMV scale, the fpsfixes.cpp effect timing.

## Implementation notes

What C12 built. `src/browser/settings-pack.h` holds the native rules, tested by `tests/browser/settings-pack.cpp`
(ramp identity and fixed ends, anisotropy rule, pacer with an injected clock, language ids).

| Setting | Where | Applies |
| --- | --- | --- |
| Anisotropic filtering (Graphics tab; in game GRAPHICS > ANISOTROPIC FILTERING) | host word 14; `DrawPacket::anisotropy` per stage (packet 252 bytes), a ninth sampler cache word and `maxAnisotropy` in the service; native `StageSampler` rule, all filters linear above 1; the title's filter 3 is accepted | Next Present |
| Brightness, Contrast (Display tab; DISPLAY > BRIGHTNESS, CONTRAST) | words 15, 16; MenuBackend keeps the game's ramp and sends `ComposeRamp(game ramp, brightness, contrast)` when either changes, so Shadow Calibration still works under it (decision 1: compose) | Next Present |
| Frame rate cap (Display tab: Display rate, 30, 60; DISPLAY > MAX FRAME RATE) | word 17; `Pacer` after every presented Swap (`DiagnosticGraphics::SetPacer`), skipped when the cap is at or above the display rate the page measures from animation frames (word 18) | Live |
| Language (Game tab, marked "next start"; GENERAL > LANGUAGE) | Automatic resolves from `navigator.language`; the host settings text carries `language=`; `app::BootLanguage()` latches it and the kernel answers XC_LANGUAGE with it (decision 2: a language setting overrides the kernel default) | New start |
| Audio output (Audio tab: Stereo, Mono; GENERAL > AUDIO OUTPUT) | browser-audio's `setOutput` (gain node down-mix); word 19 makes `GetSpeakerConfig` answer DSSPEAKER_MONO | Mix live, speaker config at the game's next query |

Vsync: browsers always synchronise canvas presentation; no setting.

### Checked in the browser (out/parity)

- Language French: the title read "APPUYEZ SUR START POUR COMMENCER".
- Frame cap 30: 30.8 presented frames a second on the title.
- Anisotropy 16 and brightness 75: no validation errors, the picture visibly brighter.

### Not checked

Index 0xFFFF (whole EEPROM) still answers 0xC00000BB; the game booted French without it. Perf at anisotropy 16 and
the cap's interval jitter were not measured with the harness. Disc images without all five language folders are not
detected.
