# C14c: Communicator headset and voice chat

Class: Complex
Depends on: C01 (hook foundation: memory helpers, symbol macros, app::Settings shim, host-settings channel, selected hook source compilation), C14a (virtual LAN transport and browser xnet provider with the VDP route table). The versus hooks (task 7) also need C13a (versus image runtime). Push to talk (task 5) uses the C05 action table and the C03 input core (`input::EventKeyHeld`).
Native sources: src/xdk/communicator.cpp, src/xdk/communicator.h, src/game/communicator.cpp, src/versus/communicator.cpp, src/audio/voice_link.cpp, src/audio/voice.cpp, src/xdk/dsound.cpp, src/xdk/xnet.cpp, src/core/input.cpp, src/app/app.h, src/app/settings.cpp, CMakeLists.txt

## Goal

A browser player with a microphone appears to the game as an Xbox Communicator on player 1's port. They talk to other players through the game's own voice path, with optional push to talk. In a second phase, a 16 kHz Opus link replaces the game's 8 kHz voice for peers that support it, as on the native PC port. Denying microphone permission leaves the game fully playable.

## Native behaviour (cite file:line)

Communicator HLE (src/xdk/communicator.cpp)
- Header (communicator.cpp:1-4): the Communicator sits on the keyboard player's port. The XPP device records say a headset is plugged in, and `XVoiceCreateMediaObjectEx` hands the game a host microphone and headphone. The encoder (8 kHz Voxware), voice packets and voice detection stay the game's own. Each image hooks its own copies of the XDK functions.
- XMEDIAPACKET layout: +0 buffer, +4 max size, +8 completed size pointer, +0xC status pointer, +0x10 event or context. The media object is a vtable pointer plus 10 slots: XMediaObject's 7 methods and spares (communicator.cpp:32-36).
- Recorded sound is kept at most 200 ms behind; played sound is queued 60 ms ahead (communicator.cpp:37-38). Without a device it records silence and drops sound at the format's pace (`Media`, communicator.cpp:48-58).
- Packets complete on a pump, because the console's USB driver completes them on its own and callbacks run at DPC level (`Pump`, communicator.cpp:144-157).
- Push to talk keeps the mic open 300 ms after release (communicator.cpp:41-42, `MicOpen` 135-142). The default key is T (src/core/input.cpp:225 story, 257 versus).
- `Refresh` and `Create` are the entry points the image hooks call (src/xdk/communicator.h:13-21). Creating a microphone starts the Opus link (communicator.cpp:289-290); releasing it stops the link (communicator.cpp:181).

Story hooks (src/game/communicator.cpp)
- Device record types 0x4EC630 (microphone) and 0x4EC63C (headphone) (line 17).
- `sub_004EDA95` (28), `sub_004EDAB7` (34) and `sub_004EDA54` (40): XGetDevices, XGetDeviceChanges and the game's own record read call `Refresh` first.
- `sub_004F4EE3` (47): XVoiceCreateMediaObjectEx calls `Create`, else the original.
- `sub_00413230` (54): a talker's DirectSound stream at holder +0x14 is muted while the link plays (`dsound::MuteForVoiceLink`, src/xdk/dsound.cpp:1090; checked at dsound.cpp:513).
- `sub_0029888F` (68-81): lock order fix. The voice tick takes the voice locks and then the network lock, while a network thread takes them in the other order. On multi core this deadlocks, so the hook takes the session's network critical section first (`sub_00255732`; kernel ordinals 277 and 294; the byte at 0x6AB0F8 disables network locking).

Versus hooks (src/versus/communicator.cpp)
- Types 0x312890 and 0x31289C (line 13). Device reads `sub_0031394B` (20), `sub_0031396D` (26), `sub_0031390A` (32). XVoiceCreateMediaObjectEx `sub_0031AF6D` (39). Talker stream mute `sub_0021C9F4` (46), stream at this +0x18.

Wideband voice link (src/audio/voice_link.cpp, src/audio/voice.cpp)
- The Opus link lives in src/audio, not in the communicator files. While the game sends VDP (game data plus its 8 kHz voice) to a peer, a 16 kHz Opus copy of the same microphone goes to that peer over the same socket, and the peer plays it in place of the game's voice (voice_link.cpp:1-5).
- Versus is client and server: whoever receives a talker's own packets passes them on to the other peers, marked relayed, and relayed packets go no further (voice_link.cpp:4-5, 199-200).
- Packet: magic `SCDV`, version 2, 12 byte header (magic, version, flags, sequence, talker), relayed flag 1, payload up to 1276 bytes. 16 kHz, 20 ms frames (320 samples), 16 kbps, complexity 10 (voice_link.cpp:34-40).
- Send to peers the game sent voice to in the last 300 ms. Mute the game's voice while ours was heard in the last 400 ms. Talker timeout 10 s. Jitter buffer: start at 2 frames, conceal up to 3, drop the oldest past 10 (voice_link.cpp:41-46).
- Entry points: `StartVoiceLink` 265, `StopVoiceLink` 276, `VoiceSent` 291, `TakeVoiceLinkPacket` 297, `SetMicOpen` 309.
- Transport: xnet keeps a VDP route per peer (src/xdk/xnet.cpp:80-84, 322-340), sends with `SendVoiceLink` (xnet.cpp:454-470) and strips `SCDV` packets before the game (xnet.cpp:262). VDP sockets are UDP with protocol 254 (xnet.cpp:614-622).
- Capture and playback: SDL3 audio streams (src/audio/voice.cpp:14-78). Opus v1.5.2 is linked natively (CMakeLists.txt:56).
- Settings: `microphone` (device by name, empty for the default) and `push_to_talk` (src/app/app.h:69-70; src/app/settings.cpp:120-121, 179-180).

## Browser today (cite file:line; where the work plugs in)

- There is no Communicator HLE and no capture path. XNet is refused (`XNetStartup` returns 0x276B, src/browser/sdk-provider.cpp:60-68) until C14a replaces it.
- The input layer reports gamepad ports only (src/browser/menu-input.cpp:115) and never reports a headset.
- Audio: one 48 kHz AudioContext with the `mission-audio` worklet fed from a shared queue (src/browser/browser-audio.mjs:1-60, 46-52; src/browser/audio-worklet.mjs:6 requires 48000 Hz). DirectSound streams are emulated in src/browser/audio-provider.cpp (`FindState`, line 228), which has no equivalent of `MuteForVoiceLink`.
- Guest events can be signalled from host code through `Dispatcher::SetEvent` (src/browser/dispatcher.h:97). That is the hook for media packet completion events.
- Microphone access needs a secure context and a user gesture. The served page already requires cross origin isolation (tools/browser/serve.mjs:23-25).

## Tasks (numbered, concrete, each verifiable)

Phase 1: game voice
1. Capture: on a user gesture call `getUserMedia({ audio: { deviceId, echoCancellation: true, noiseSuppression: true, autoGainControl: true } })`. Add an AudioWorklet in the existing 48 kHz context that downsamples to 16 kHz mono s16 into a SharedArrayBuffer capture ring. Write node unit tests first for the resampler (frequency response at 1 kHz, no drift over 60 s) and the ring.
2. Headphone output: a playback ring from the Wasm into a second worklet input, or a mix into the mission queue at 48 kHz, with its own gain.
3. Port src/xdk/communicator.cpp as a browser provider. Keep the media object layout and packet rules. Back the microphone with the capture ring and the headphone with task 2. Complete packets from a pump thread through `Dispatcher::SetEvent`, and keep the 200 ms and 60 ms bounds. With no permission or device, record silence at pace, exactly as native does without a device.
4. Port the story hooks of src/game/communicator.cpp through C01, including the lock order fix `sub_0029888F`. Verify the game shows a headset on player 1's port.
5. Push to talk: add the `push_to_talk` action (key T by default) to the C05 action table and read it through `input::EventKeyHeld` from C03, with a 300 ms hold. Unit test the hold timing with a fake clock.
6. End to end with C14a: two tabs in a co-op session hear each other through the game's own voice. Verify with a fake media stream (a WAV with a tone) on one side and the received energy at the tone frequency on the other.
7. After C13a: port the versus hooks of src/versus/communicator.cpp, compiled only into the versus module, and verify voice between two versus clients.

Phase 2: wideband link
8. Choose the codec: libopus 1.5.2 compiled to Wasm (same version as native, deterministic, works in every target browser), or WebCodecs `AudioEncoder` and `AudioDecoder` with `opus` where supported. Record the decision with the browser support matrix checked at the time.
9. Port the link logic of voice_link.cpp: the `SCDV` v2 format, send to peers the game voiced in the last 300 ms, the jitter buffer constants, the versus relay rule, and the talker timeout. Write unit tests first for the packet codec, sequence wrap, the jitter buffer (start, conceal, drop) and the relay rule.
10. Route `SCDV` packets through the C14a provider's VDP route table (the browser equivalents of `SendVoiceLink` and the strip in `Recv`).
11. Add a mute equivalent to `MuteForVoiceLink` in src/browser/audio-provider.cpp, and call it from the talker stream hooks (`sub_00413230` story, `sub_0021C9F4` versus). Verify the game's 8 kHz voice is silent while the link is heard and returns within 400 ms after it stops.

## Settings and UI (settings.mjs entries, Options panel / main menu changes)

- `voiceEnabled`: boolean, initial false. Nothing asks for microphone permission until this is turned on.
- `microphone`: options from `navigator.mediaDevices.enumerateDevices()` (default first), stored by deviceId, with labels shown once permission is granted. Audio tab.
- `pushToTalk`: boolean, initial false. The key (T) is listed on the Controls tab.
- `voiceVolume`: number 0 to 100, step 5, initial 100. Audio tab.
- `widebandVoice`: boolean, initial true; it only takes effect once phase 2 lands.
- Audio tab status line: "Microphone: allowed / blocked / not found" and a live input level meter.

## Acceptance criteria

- With voice enabled and permission granted, the game reports a headset on player 1's port, and each co-op player hears the other.
- Push to talk gates sending, with a 300 ms hold after release.
- Denying permission or having no microphone leaves the game playable, with no headset reported and no errors.
- A player joining a voiced session does not deadlock the game, over a 30 minute soak with repeated joins.
- Phase 2: peers with the link hear 16 kHz voice, and the game's 8 kHz voice is muted while the link plays. In versus (after C13a), voice reaches all clients through the relay rule.

## Testing

- Unit tests first (node): resampler, rings, push to talk timing, `SCDV` codec, jitter buffer, relay rule, settings normalisation.
- Native and Wasm tests for the media object: packet completion order, flush, release while packets are pending, silence at pace without a device.
- Browser: agent-browser CLI with two sessions against a separate served build dir and a local relay on its own port. Never use port 8125 or out/boot-m5. Mute game audio with out/front-end/mute.js. Use Chromium fake media flags (`--use-fake-device-for-media-stream`, `--use-file-for-fake-audio-capture`) and check signal energy instead of listening.

## Risks and open questions

- Which headphone format does the game request (rate, channels) in each image? Read it from the `XVoiceCreateMediaObjectEx` format argument on the first run.
- Echo: without headphones the game's voice output can feed the microphone. Browser echo cancellation helps only for audio played through the same page; check that the AudioWorklet output counts.
- WebCodecs Opus support differs between browsers and versions; libopus in Wasm adds download size, but avoids the browser support gaps.
- Can the browser audio provider identify the talker's DirectSound stream the same way native does, by object address? Confirm before task 11.
- The lock order fix is mandatory on browser pthreads. Without it, a deadlock appears when a player joins with voice.
- Microphone permission prompts inside an embedding page may need a `Permissions-Policy` header on the production host.

## Out of scope

- The transport, rooms, the xnet provider and its VDP route table (C14a).
- Co-op lockstep and local players (C14b).
- Text chat, and voice outside game sessions (a lobby voice room).
- Interop with native PC players over a UDP bridge (the packet format stays compatible).
