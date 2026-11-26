<p align="center">
  <img src="port/launcher/res/img/wordmark.png" alt="Moonsprout" width="520">
</p>

<p align="center">
  <b>A native PC port of Pikmin 2 for Linux, Windows and macOS. You bring your own disc.</b>
</p>

<p align="center">
  <a href="https://github.com/owallzz/moonsprout/actions/workflows/ci.yml"><img src="https://github.com/owallzz/moonsprout/actions/workflows/ci.yml/badge.svg" alt="CI"></a>
  <a href="https://github.com/owallzz/moonsprout/releases"><img src="https://img.shields.io/github/v/release/owallzz/moonsprout?include_prereleases&label=release" alt="Latest release"></a>
  <a href="LICENSE.MD"><img src="https://img.shields.io/badge/license-CC0--1.0-blue" alt="License: CC0-1.0"></a>
</p>

<p align="center">
  <img src="docs/images/start-menu.png" alt="The Moonsprout start menu" width="760">
</p>

Moonsprout runs the GameCube game Pikmin 2 as a native desktop program, with no emulator. It
compiles the [projectPiki/pikmin2](https://github.com/projectPiki/pikmin2) decompilation for your
PC and runs it on [Aurora](https://github.com/encounter/aurora), which provides the GameCube
graphics, audio, input and memory card on top of SDL3 and WebGPU (Vulkan, Direct3D 12 or Metal).

**This repository contains no game data.** Moonsprout needs a disc image of
**Pikmin 2 (USA, `GPVE01`)** that you dumped from your own disc. All models, textures, sounds,
movies and text are read from that image at run time.

> [!NOTE]
> Moonsprout is a fan project. It is not affiliated with, endorsed by or sponsored by Nintendo.
> Pikmin is a trademark of Nintendo. Don't ask for, or share, disc images or game files here.

## Status

Moonsprout is a work in progress. You can play story mode from the start through several days,
Challenge Mode caves and 2-Player Battle. Later story areas, every cave floor and the ending haven't
been played through yet, so expect bugs. Audio plays streams, music and sound effects, but some of
the console's audio effects (reverb, filters) aren't recreated yet. See
[docs/PORT_STATUS.md](docs/PORT_STATUS.md) for the details.

## Features

- **Start menu and settings.** You can pick the disc image from a file dialog or drop it onto the
  window, and Moonsprout checks it against the known dump. Settings are split into tabs (video,
  input, audio, gameplay, cheats, interface, online and tools), and most of them can be changed in
  game.
- **Widescreen.** Any window shape, from 4:3 up to ultrawide. The HUD stays anchored to the edges,
  and you can raise the internal resolution. The **Original** preset keeps the 4:3 picture at
  GameCube resolution.
- **Controllers, keyboard and mouse.** Controllers can be plugged in at any time. The keyboard
  works out of the box, and every button can be remapped.
- **Save manager.** Memory card saves sit in your user folder, and you can manage them from the
  menu.
- **Mods.** Data mods replace or add files on the virtual disc without touching your image. See
  [docs/mods/README.md](docs/mods/README.md).
- **Online play (experimental).** 2-Player Battle over the internet, with lockstep netcode, join
  codes and a small lobby and relay server you can host yourself ([server/](server/README.md)).
- **Update check.** Moonsprout can tell you when a new release is out. It never downloads anything
  by itself.

<p align="center">
  <img src="docs/images/settings.png" alt="The settings window" width="640">
</p>

## Download

Download the build for your system from the
[latest release](https://github.com/owallzz/moonsprout/releases):

| System | Download | Notes |
|---|---|---|
| Windows 10/11 (x64) | `Moonsprout-<version>-windows-x64-setup.exe` (installer) or `…-windows-x64-portable.zip` | The portable build keeps its settings and saves next to the program. The builds aren't signed yet, so click **More info → Run anyway** if SmartScreen warns you. |
| Linux (x86_64) | `Moonsprout-<version>-linux-x86_64.AppImage` or `…-linux-x86_64.tar.gz` | Run `chmod +x` on the AppImage, then start it. Needs glibc 2.39 or newer (Ubuntu 24.04, Fedora 40, SteamOS 3.6 and later) and a Vulkan driver. |
| macOS 13+ (Apple silicon / Intel) | `Moonsprout-<version>-macos-arm64.dmg` / `…-macos-x86_64.dmg` | Drag Moonsprout to Applications. The app isn't notarized, so the first time, right-click it and choose **Open**. If macOS says the app is damaged, run `xattr -dr com.apple.quarantine /Applications/Moonsprout.app`. |

On first start, choose a look (Modern or Original), then **Choose Disc Image** and pick your
`GPVE01` image (`.iso`, `.gcm`, `.ciso`, `.gcz`, `.rvz`, `.wia`, `.nfs` or `.tgc`). Settings and
saves are stored in your user data folder (`~/.local/share/mppm` on Linux,
`%APPDATA%\mppm` on Windows, `~/Library/Application Support/mppm` on macOS).

## Building from source

You need CMake 3.25+, Ninja, Clang (Apple Clang on macOS, clang-cl on Windows), Python 3 and Git.
The build downloads Aurora's dependencies (SDL3, Dawn and others) as it goes.

```sh
git clone --recursive https://github.com/owallzz/moonsprout.git
cd moonsprout
cmake -S . -B build-pc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-pc --target mppm
./build-pc/mppm                      # opens the start menu
./build-pc/mppm /path/to/GPVE01.iso  # or boot a disc image straight away
```

On Linux, install the SDL3 build dependencies first. For Ubuntu 24.04:

```sh
sudo apt-get install ninja-build clang lld libvulkan-dev libx11-xcb-dev libxi-dev libxrandr-dev \
  libxinerama-dev libxcursor-dev libxss-dev libxtst-dev libxfixes-dev libasound2-dev libpulse-dev \
  libudev-dev libdbus-1-dev libdecor-0-dev libpipewire-0.3-dev libwayland-dev libxkbcommon-dev \
  libcurl4-openssl-dev libssl-dev
```

Online play needs a libcurl with WebSocket (`wss://`) support. Ubuntu 24.04's has none, so the build
then fetches and builds its own libcurl against OpenSSL (hence `libssl-dev`); a distribution whose
libcurl has it, such as Fedora, uses the system one. The configure stops with an error if neither
works.

On Windows, install Visual Studio 2022 or newer with the **Desktop development with C++** workload
and its **C++ Clang tools** component, open a **Developer PowerShell**, and configure with
Visual Studio's own clang-cl (a standalone LLVM may be too old for its C++ library):

```powershell
$cl = "$env:VCINSTALLDIR\Tools\Llvm\x64\bin\clang-cl.exe"
cmake -S . -B build-pc -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_C_COMPILER=$cl" "-DCMAKE_CXX_COMPILER=$cl"
cmake --build build-pc --target mppm
```
On macOS, install Xcode and `brew install ninja llvm` (the build uses LLVM's `llvm-objcopy`), and leave out the compiler options.

[.github/workflows/ci.yml](.github/workflows/ci.yml) builds all three systems on every push, and
[docs/PORT_STATUS.md](docs/PORT_STATUS.md) has developer switches, test scripts and notes on
running headless. The original matching decompilation build (which needs the original compilers
and your own extracted game files) is still described in [docs/DECOMP.md](docs/DECOMP.md).

## Repository layout

| Path | What's there |
|---|---|
| `src/`, `include/` | The decompiled game, shared with the matching build. PC-only changes are marked `PC_ONLY`. |
| `port/` | The native port: SDK shims, audio, asset byte-swapping, the launcher (`port/launcher`), mods and netcode. |
| `extern/` | Aurora and borealis, as submodules. Local Aurora fixes are in `port/patches/aurora/`. |
| `server/` | `mppm-server`, the online lobby, signalling and relay server (Rust). |
| `tools/`, `tests/` | Source list generation, self-tests and the headless determinism harness. |
| `docs/` | Port status, plans, design notes for the launcher and online play, the modding guide. |
| `config/` | The decomp's symbol and split maps for the matching build. |

## Contributing

Issues and pull requests are welcome. Code in `src/` and `include/` must keep the matching build
byte-identical (`python3 configure.py && ninja` checks it against `config/GPVE01/build.sha1`), so put
PC-only changes behind `PC_ONLY` or into `port/`. Please never attach game files, disc images or
copyrighted assets to an issue or pull request.

## Credits

- [projectPiki/pikmin2](https://github.com/projectPiki/pikmin2) and its contributors, for the
  decompilation this is built on.
- [Aurora](https://github.com/encounter/aurora) and [borealis](https://github.com/encounter/borealis)
  by encounter (MIT).
- Dusklight's CC0 RmlUi interface framework, which the launcher UI is built from.
- [ENet](https://github.com/lsalzman/enet) (MIT) and [libjuice](https://github.com/paullouisageneau/libjuice)
  (MPL-2.0) for networking. The math functions in `port/libm` come from musl and FreeBSD (see
  `port/libm/COPYRIGHT`).
- Fonts: Fredoka, Gluten, Nunito and Noto Mono (SIL OFL 1.1), and Material Symbols (Apache-2.0).
  The license files are in `port/launcher/res/fonts/licenses/`.

## License

Moonsprout is released under [CC0 1.0](LICENSE.MD), the same as the decompilation it's built on.
Submodules, fetched dependencies, fonts and `port/libm` keep their own licenses. No game data is
included or distributed.
