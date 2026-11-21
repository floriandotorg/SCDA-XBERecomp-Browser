# Splinter Cell: Double Agent - XBE Recomp for the Browser
A fork of [SCDA-XBERecomp](https://github.com/TGP482/SCDA-XBERecomp), the native PC port of the original Xbox version of Splinter Cell: Double Agent, that makes the game run in a web browser. The original project translates the game's x86 code to C++ ahead of time with the [Xbox XBE Recomp](https://github.com/TGP482/XboxXBE-Recomp) tool and implements the Xbox kernel, Direct3D and DirectSound on Windows. This fork compiles the same generated C++ to WebAssembly with Emscripten, draws with WebGPU and plays sound through Web Audio. The game reads its files straight from your own ISO; nothing is uploaded or downloaded.

> [!NOTE]
> This is an unofficial fan project. It is not affiliated with, endorsed, sponsored or approved by Ubisoft, Microsoft or the developers of the original game; see [Legal](#legal).
>
> This project does not include any game assets or generated game code. You need your own legally acquired copy of the game, both to build and to play. There is no hosted version: you build and serve the page yourself.

> [!IMPORTANT]
> The browser port is work in progress. The story campaign boots from the ISO into the original menus and missions are playable, but full campaign coverage has not been verified. Co-op, Versus, LAN and online play are not supported. See the [browser port roadmap](docs/browser-port/README.md) for the current state.

## Playing
- Build the page and start the local server as described in [Building](#building).
- Open the printed address in a supported browser.
- Click **Start game** and select your game ISO. The first time, the page copies the whole image into the browser's private storage, which takes a minute, and asks the browser to keep that storage. Later visits start from that copy and need neither the original file nor permission to read it. **Choose another disc** replaces the copy.
- Settings are in **Options** on the main menu, in the pause menu (<kbd>Esc</kbd>), or in the settings panel (<kbd>F10</kbd>). They are stored in the browser.
- Profiles and saves are stored in the browser and survive closing the page. The top of the screen shows when progress is saved; the page asks before closing while a save is still being written. **Options**, **Progress** exports your progress to a `.scdasave` file and imports it again, for example into another browser. Clearing the site's data in the browser deletes the progress, so export it first. A profile takes about 30 MB, and only one fits for now.

The ISO must contain the same `default.xbe` the build was generated from. Both the XISO layout and full Xbox disc images are accepted; extracted folders are not.

### Phones and Tablets
The same page runs on phones and tablets with an on-screen controller. Browsers allow WebGPU and shared memory only on secure pages, so serve it over HTTPS on your network:

```sh
node tools/browser/serve.mjs out/browser/boot-web 8443 --lan --https
```

The server prints the addresses to open and creates a self-signed certificate that the phone asks you to accept once. Touch support has only been tested in Chrome's device emulation, not on a real phone. See [Phones and tablets](docs/browser-port/mobile.md) for details.

## Features
### Display
- **Scaling** - Fit, integer or stretch scaling of the game picture to the window, with a smooth or sharp filter.
- **Aspect Ratio** - Automatic from the window, or fixed at 4:3, 16:10, 16:9 or 21:9, with widescreen HUD and menus.
- **Resolution** - Render at the console's 640x480, at the window's size, or with a dynamic resolution, scaled from 50% to 200%.
- **Field of View** - Sets the horizontal field of view during gameplay from 45 to 140 degrees, with 75 being the game's default.
- **Frame Rate Cap** - 30 or 60 FPS, or uncapped (follows the display's refresh rate).
- **HDR Output** - HDR canvas output on displays and browsers that support it, with adjustable headroom.
- **Brightness and Contrast**
- **FMV Scale** - Fill widescreen displays with the FMVs, or show them in a 4:3 frame as on the original Xbox.
- **FPS Counter**

### Graphics
- **Anti-Aliasing** - 4x MSAA and SMAA, which can be combined. WebGPU only supports 4x MSAA.
- **Anisotropic Filtering** - 2x up to 16x.
- **Shadow Resolution** - From the Xbox's 512x512 up to 4096x4096.
- **Shadow Distance** - Keeps lights and their shadows on up to 8x farther.
- **Shadow Filter** - The Xbox shadow filter or a soft filter.
- **Ambient Occlusion** - SSAO or GTAO.
- **Shader Model** - Shader Model 1.1, the Xbox look, or Shader Model 3.0 with the Chaos Theory PC style HDR rendering and tone mapping.

### Input
- **Mouse and Keyboard** - Mouse look with pointer lock, separate free look, weapon and sniper sensitivity, inverted look and rebindable keys.
- **Controller** - Any controller the browser's Gamepad API sees, with rebindable buttons. Defaults match the Xbox layout.
- **Touch** - On-screen controller on touch screens with adjustable opacity, size and look sensitivity.

### General
- **Language** - English, French, German, Italian or Spanish, or the browser's language.
- **Mono Audio**
- **Skip Intros**
- **Pause On Focus Loss** - Pauses the game when the tab is in the background.
- **Developer Console** - The game's console, opened with <kbd>`</kbd> once enabled in Options.

## Requirements
- A browser with WebGPU, WebAssembly threads, SIMD and tail calls:
  - Chrome or Edge 121 or newer on desktop or Android
  - Safari on macOS 26, iOS 26 or iPadOS 26 or newer
- The page must be served with cross-origin isolation (`tools/browser/serve.mjs` does this) from `localhost` or over HTTPS.
- Development and testing happen in Chrome on an Apple Silicon Mac (M1 Max), where the game runs at 60 FPS. Other browsers and GPUs are less tested.

## Building
The browser build runs on macOS (and should work on Linux). It needs no Windows toolchain.

### 1. Install Dependencies
- CMake 3.25 or newer, Ninja, Git and a C++20 compiler for the recompiler. On macOS, install the Xcode Command Line Tools.
- [Emscripten](https://emscripten.org/) for the WebAssembly build. Development uses Emscripten 6.0.13 with the Binaryen `wasm-opt` it ships.
- [Node.js](https://nodejs.org/) 24 or newer for the build scripts, the server and the tests.

On macOS with Homebrew:

```sh
brew install cmake ninja emscripten node
```

### 2. Clone the Repository
```sh
git clone <this repository's URL> SCDA-XBERecomp
cd SCDA-XBERecomp
```

### 3. Add the Game Executable
Place `default.xbe` from your game disc in the `assets` folder. The build checks the generated code against it. The rest of the game data is read from the ISO at runtime.

### 4. Generate the C++ Code
Build the recompiler and translate `default.xbe` into `generated/game/`:

```sh
cmake -S . -B out/build/recomp -G Ninja -DRECOMP_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build out/build/recomp --target xbe_recomp --parallel
./out/build/recomp/compiler/xbe_recomp config/scda_v2.toml
```

### 5. Create the Generation Record
The browser build only accepts generated code it can tie to your `default.xbe` and the current recompiler. It reads this from a generation record, a JSON file holding the SHA-256 of `default.xbe`, of the generator inputs and of every generated file. `tools/browser/provenance.mjs` lists the files and fields it checks; configuration fails if any of them do not match. Create the record, for example at `out/provenance-audit/generation-record.json`, after regenerating into a second folder and confirming that both outputs are identical:

```sh
./out/build/recomp/compiler/xbe_recomp config/scda_v2.toml assets/default.xbe out/provenance-audit/regenerated
diff -r generated/game out/provenance-audit/regenerated
```

The record has to be refreshed whenever the recompiler, its configuration or the generated code changes.

### 6. Build the Browser Version
```sh
emcmake cmake -S . -B out/browser -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBROWSER_BUILD=ON -DBROWSER_BOOT=ON -DBROWSER_STORY=OFF \
  -DBROWSER_MENU=ON -DBROWSER_MISSION=ON -DBROWSER_ACTUAL_TAIL_CALLS=ON \
  -DBROWSER_WASM_SIMD=ON -DBROWSER_MENU_TRACE_ABORTS=ON \
  -DBROWSER_GENERATION_RECORD="$PWD/out/provenance-audit/generation-record.json" \
  -DBROWSER_WASM_OPT="$(brew --prefix emscripten)/libexec/binaryen/bin/wasm-opt"
BINARYEN_CORES=4 cmake --build out/browser -j2
```

The first build compiles all 128 generated partitions and takes a while; later changes outside the generated code and its shared headers only relink. Linking needs a lot of memory, which is why the job count is kept low. If `wasm-opt` is not at the Homebrew path, point `BROWSER_WASM_OPT` at the one that comes with your Emscripten install.

### 7. Serve the Page
```sh
node tools/browser/serve.mjs out/browser/boot-web 8080
```

Open `http://127.0.0.1:8080/`. The server sets the cross-origin isolation headers that shared WebAssembly memory needs; a plain static file server will not work.

### Tests
```sh
node --test tests/browser/*.test.mjs
```

The full suite takes a long time. Run single files from `tests/browser/` while working on a part of the port.

## Windows Version
The native Windows build of the original project (`build.bat`, `CMakePresets.json`, `src/app`, `src/gpu/d3d12.cpp`) is still part of this repository, but this fork does not test it. For the Windows version, its features and online play, see the [original repository](https://github.com/TGP482/SCDA-XBERecomp).

## Legal
This is an unofficial, non-commercial fan project, made for game preservation and interoperability. It is not affiliated with, endorsed, sponsored or approved by Ubisoft Entertainment, Microsoft or any studio, developer or publisher of the original game, and none of them is responsible for it or provides support for it.

The repository contains only source code written for this project and the project it is forked from. It contains no code or assets from the game and no code from Microsoft's software, and it does not provide the game, any part of it, any way to obtain it or any way to bypass copy protection. To use it you need your own legally obtained copy of the game, which is read locally on your device. Please do not ask for or share game files, disc images or generated code in this repository.

The MIT license in [LICENSE](LICENSE) applies only to this project's source code. It grants no rights to the game, its code, its assets or its trademarks, which remain the property of Ubisoft Entertainment and its licensors.

Tom Clancy's, Splinter Cell, Splinter Cell Double Agent and Ubisoft are trademarks of Ubisoft Entertainment in the U.S. and/or other countries. Xbox is a trademark of the Microsoft group of companies. All other trademarks are the property of their respective owners. They are mentioned only to identify the game and the platform this software is compatible with.

If you hold rights to the game and have a concern about this project, please open an issue in this repository. It will be answered promptly and in good faith.
