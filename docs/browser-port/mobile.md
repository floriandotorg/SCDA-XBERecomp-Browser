# Phones and tablets

The menu build (`BROWSER_MENU`, `BROWSER_MISSION`) runs on phones and tablets whose browser has WebGPU, shared WebAssembly memory with threads, SIMD and tail calls: Chrome 121 or newer on Android, Safari on iOS 26 or iPadOS 26 and newer. The page is the same as on a desktop; it adapts to touch.

## Serving to a phone

Browsers allow WebGPU and `SharedArrayBuffer` only in a secure context, so a phone on the same network needs HTTPS:

```sh
node tools/browser/serve.mjs out/<build>/boot-web 8443 --lan --https
```

`--lan` listens on every interface and prints the addresses to open; `--https` creates a self-signed certificate for this machine's names and LAN addresses in `out/https-certificate` and reuses it. The phone warns about the certificate once: on iOS tap Show Details, then visit this website; on Android tap Advanced, then Proceed. A tunnel with a trusted certificate works too, as long as it passes the cross origin isolation headers through.

Add to Home Screen (iOS) or Install app (Android) opens the page full screen and sideways from `manifest.webmanifest`. On iPhone this is the only way to hide the browser bars: Safari has no element full screen there.

Select the ISO from Files (iOS) or the file picker (Android). The picker shows every file on touch screens, as phones cannot always tell a disc image by type; validation rejects anything that is not the game. The phone may copy the image before the page can read it.

## Touch controls

`src/browser/touch-controls.mjs` draws an on-screen controller while the game runs. It is a virtual standard pad that `gamepad-input.mjs` merges with a physical one, so the game reads the console controller and shows its button prompts (the gamepad record's connected word is 2 for a touch-only pad, and `input-core.cpp` then prefers the pad's prompts).

* Left side: a floating stick where the thumb lands, the d-pad (vision modes) above it and LS (back to wall).
* Right side: drag anywhere to look, as mouse movement that leaves the prompts on the pad; A, B, X and Y, the triggers, RS (zoom), LB and RB.
* Top: Back (OPSAT), Start (pause) and the page's own pause and settings button.
* In a game menu the stick and combat buttons hide, and a tap on the picture clicks the item under the finger. Movies keep A, B and Start.
* On the name entry page a keyboard button opens the phone's keyboard; Done accepts as A does.

Options, Touch: show the controls automatically, always or never; opacity; size; look sensitivity; vibration (Android) and full screen on the first touch (Android, iPad). In Automatic a physical controller's button or a keyboard key hides the overlay until the next touch.

## Phone adjustments

* The page fills the screen up to the notch and rounded corners (`viewport-fit=cover` with the safe area insets) and asks to be turned sideways in portrait during play.
* The screen stays awake while the game runs (Screen Wake Lock), and on iOS game audio plays with the ringer switch on silent (`navigator.audioSession`).
* Touch screens start 64 guest thread workers rather than 129 (`Module.pthreadPoolSize`, about 8 MB each); a mission uses about 25.
* Before the file picker opens, the page says which requirement the browser misses (secure context, cross origin isolation or WebGPU).

## Not verified

Touch input, layouts and both thread pool sizes were tested in Chrome with touch and phone screen emulation (874 x 402 at 3x, landscape and portrait) from the main menu through profile creation into the first mission. No physical phone, iOS Safari or Android GPU was available, so frame rate, memory limits and WebKit behavior on real devices are unmeasured.
