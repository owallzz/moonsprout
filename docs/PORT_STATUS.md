# mppm port status

Live tracker for the native port. The plan and milestones are in [PORT_PLAN.md](PORT_PLAN.md).

## Building (host)

```sh
git submodule update --init --recursive
cmake -S . -B build-pc -G Ninja          # Apple Clang (Xcode) or Linux clang
# optional on macOS: --toolchain cmake/toolchain-macos-llvm.cmake to use Homebrew LLVM instead
ninja -C build-pc mppm
```

After changing `configure.py`'s object lists, regenerate the source list with `python3 tools/pc/gen_sources.py`.
On Linux arm64, add `-DAURORA_NOD_PROVIDER=package` (Aurora only auto-selects the prebuilt disc library on x86_64).
Run `python3 tools/pc/check_include_case.py` after upstream merges: include paths must match on-disk case for Linux.
On an immutable Linux host without dev headers (Fedora Silverblue and similar), build inside a Fedora distrobox that has `clang lld cmake ninja-build` plus the SDL3 X11/Wayland/audio `-devel` packages that `pc-build.yml` installs.
GCC 16's libstdc++ evaluates `RAND_MAX` in `#if`; `include/Dolphin/rand.h` includes `<algorithm>`/`<cstdlib>` before redefining it as a float on PC.
Host-only edits must keep the matching build byte-identical. Check with `python3 configure.py && ninja` (the `CHECK` step compares against `config/GPVE01/build.sha1`). Edits that look neutral can still change MWCC output: an added line shifts `__LINE__` in asserts, `template <>` reorders functions (use `PC_TEMPLATE_SPEC`), and a redundant `(u8*)` cast changes register allocation (use `GD_CURR_U8PTR()`).
The original matching build (`configure.py` + ninja + MWCC) is unaffected by any of this.

## Milestone status

| Milestone | State |
|---|---|
| M0 Repo + plan | ✅ done |
| M1 Host compile | ✅ done. All 963 game translation units compile with Clang (macOS arm64, Linux x86_64/arm64) and link against Aurora. The binary initializes Aurora/Metal and enters the game's `main`. |
| M2 Boots | ✅ done. Boots through the health-and-safety screen, title screen and menu, the memory card check (creates a save file in Aurora's card image), the file select and the opening cutscene, into controllable gameplay in the Valley of Repose (about 25 fps headless under Xvfb). |
| M3 Complete game | ⏳ in progress. Verified headless: story mode (title, file select, opening THP movie, landing cutscene, Valley of Repose gameplay at 30 fps, day end with results, mail and saving, world map, loading the save on day 2), Challenge Mode caves (Explorer's Cave, give up and retry), 2-Player Battle, Options, High Scores, Bonuses, title attract movies. Audio runs through a software stand-in for the DSP (streams, BGM, sound effects); oscillator voices, reverb and filters are not synthesised yet. Untested: later story areas, cave floor transitions, Piklopedia contents, the ending. |

## Moonsprout launcher

Running `build-pc/mppm` without a disc argument opens **Moonsprout**, the start menu (specs in
`docs/launcher/`). It offers a first-run choice of a Modern or Original preset, disc selection by
file dialog or drag-and-drop with XXH3 verification against the GPVE01 dump, and settings in eight
tabs (Prelaunch, Video, Input, Audio, Gameplay, Cheats, Interface, Tools) that can be changed in
game from the F1 / Back bar. It also has widescreen with edge-anchored HUD, a save manager, a
controller window, and an update check against GitHub releases. With a remembered disc, the menu
sits over a live scene rendered by the game: Olimar, a red, yellow and blue Pikmin and the ship. On
Play, Olimar boards and the ship lifts off. A disc on the command line (or `MPPM_DISC`) still boots
straight into the game, as the headless scripts expect.

Code: `port/launcher/` (RmlUi UI ported from Dusklight, CC0, as `mppm::ui`; settings through
borealis::config), `port/game/scene/` (the live scene), `port/patches/aurora/` (aspect lock and
frame preservation). Settings are stored in `~/.local/share/mppm/config.json`.

## Running and testing

`./build-pc/mppm "<path to Pikmin 2 (USA).iso>"` (or set `MPPM_DISC`). Keyboard on port 1 when no
layout is saved: WASD stick, IJKL C-stick, Space A, Left Shift B, E X, Q Y, Enter Start, Tab Z,
F/R L/R, arrows D-pad. `MPPM_FPS=1` logs the frame rate; panics abort with a core dump unless
`MPPM_PANIC_SCREEN=1`.

Audio (port/audio/audio.cpp) is on by default; `MPPM_AUDIO=0` turns it off. `MPPM_AUDIO_DUMP=out.wav`
also writes the mixed output to a WAV file (useful with the dummy SDL audio driver), and
`MPPM_AUDIO_DEBUG=1` / `MPPM_AUDIO_VLOG=1` log voice state and every voice start and stop.
`MPPM_UNLOCK_CHALLENGE=1` offers Challenge Mode on the title menu without save progress (quick
access to caves for testing). `MPPM_ALLOW_SUNSET=1` enables "Go to Sunset" on day 1 and
`MPPM_TIME_SCALE=<factor>` speeds up the in-game clock (to reach day end quickly).
`MPPM_ALLOW_CPU_ADAPTER=1` lets Aurora use a software Vulkan adapter (lavapipe, with
`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`), for tests that must leave the GPU alone.
Each `MPPM_*` switch above overrides a launcher setting (docs/launcher/SETTINGS.md 1.3) and locks it
while set. Flags follow one rule: unset or empty = no override, `0` = off, anything else = on (so
`MPPM_FPS=0` now means off). Any setting can also be forced for one run with
`--cvar key=value` (e.g. `--cvar video.aspectRatio=stretch`); the environment wins over `--cvar`,
which wins over config.json. `MPPM_WINDOW=WxH` sets the window size.
Particle resources (.jpc) are converted to host order on load by port/assets/jpa.cpp.

Headless test runs (real GPU through Vulkan, no window, no sound):

```
env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
  xvfb-run -n 99 -s "-screen 0 1280x960x24" ./build-pc/mppm "<iso>"
```

Screenshot with `magick import -display :99 -window root shot.png`; input with
`DISPLAY=:99 xdotool windowfocus <win>; DISPLAY=:99 xdotool key Return`. Aurora names the main
thread "Main thread", so match the process with `pgrep -f '^./build-pc/mppm'`, not `-x mppm`.

Input without window focus or system-wide key events (any OS, including a hidden Windows desktop):
`MPPM_TEST_PAD="<sec>:<input>[:<ms>];..."` drives a virtual SDL gamepad, pressing each input that
many seconds after the first launcher frame (held 100 ms unless `:<ms>` is given). Inputs are SDL
gamepad names (`a`, `b`, `x`, `y`, `start`, `dpup`, `dpleft`, ...) or a stick pushed all the way
(`leftx+`, `lefty-`, ...). Keep physical controllers out with SDL's hints (`SDL_XINPUT_ENABLED=0`,
`SDL_JOYSTICK_RAWINPUT=0`, `SDL_JOYSTICK_HIDAPI=0`, `SDL_JOYSTICK_WGI=0`, `SDL_JOYSTICK_GAMEINPUT=0`,
`SDL_JOYSTICK_DIRECTINPUT=0`) and add `--cvar input.backgroundInput=true` when the window has no
focus. Example, Play on the start menu then skip the take-off: `MPPM_TEST_PAD="12:a;15:a"`.

## How resource data is handled on the host

- **`BE(T)` fields / `BE_READ()`** (types.h, port/include/p2_be.h): structs read straight from
  files stay big-endian in memory and swap on access (ResTIMG, J2D blocks, J3D file header, name
  tables, JMessage).
- **In-place conversion on load** where the runtime reads tables every frame: RARC, BFN fonts,
  J3D models (port/assets/j3d_model.cpp) and animations (port/assets/j3d_anim.cpp). Swapping the
  first block's type marks a file as converted.
- Vertex arrays and display lists stay in GameCube layout; Aurora reads them big-endian.
- `MC64('Ngame', 0x...)` spells 5-8 character multi-character literals (64-bit in MWCC).

## How the host build fits around the decomp

- **`TARGET_PC`** guards every host-only change. Edits outside a guard are codegen-neutral for MWCC: `this->`, `typename`, `template <>`, `&Class::member`, `nullptr` → `0` where MWCC's `nullptr` was `0`, and `uptr`/`sptr`, which are `u32`/`s32` under MWCC.
- **SDK headers.** Game code compiles against Aurora's Dolphin SDK headers, so struct layouts match its implementation (for example, `GXTexObj` is 64 bytes on PC versus 32 on the GameCube).
  - `include/Dolphin/*` forwards to Aurora under `TARGET_PC`.
  - `port/include/p2_sdk_compat.h` restores decomp-era names on top of Aurora.
  - `port/include/p2_sdk_aliases.h` maps enum names onto Aurora's by value. It is generated by `tools/pc/gen_sdk_aliases.py`.
- **C library wrappers.** `include/stl` is on the `-iquote` path only, so the game's `"string.h"` includes find the decomp's wrappers (which forward to the host libc), while Aurora and system `<...>` includes never do. `types.h` is force-included first in every game TU.
- **Helper macros** (in `types.h`):
  - `PC_SCOPE_BEGIN`/`END`: block scopes that only exist on PC, for switch cases.
  - `RETURN_GARBAGE(v)`: a value for bare `return;` in non-void functions.
  - `PC_INLINE`: makes header-defined explicit specializations inline on PC.
- **Mechanical fixes** were applied with `tools/pc/autofix.py`, driven by Clang diagnostics: pointer casts, switch scopes, `this->`, `nullptr`, safe Clang fix-its, and SDK field names. Re-run it after upstream merges.

## Classes of host bugs found so far

Worth checking first when something misbehaves:

- **Big-endian data read natively.** Fix by `BE()` fields, `BE_READ()` or in-place conversion (see above).
- **PowerPC paired-single `asm`** with no C body (silently empty on the host).
- **Implicit returns.** Non-void functions that relied on MWCC leaving the last call's result in r3/f1. Find them with `-Wreturn-type` (all game TUs were scanned once; real ones return explicitly under `PC_SWITCH`).
- **Layout assumptions.** Clang puts the vtable pointer first and pointers are 8 bytes: hard-coded member offsets (intrusive `JGadget::TLinkList` offsets, offset+1 tables), `((Base*)this)` casts that relied on member order, `&ptr[2]` on pointer-sized wrappers.
- **Memory-order punning**: `*(u32*)&color`, u64 ids read as characters.
- **GameCube address tricks**: pointer values compared against `0xC0000000`, unsigned 32-bit offsets that are really negative.
- **Pointer truncation and fixed sizes**: values passed through `u32` (callback arguments, DSP addresses; use `uptr`), and sizes written as literals or shifts (`count << 2` for an `OSMessage` buffer, `0x118` for a pool block) that grow on 64-bit hosts.
- **Overlapping views of the same bytes**: a `u32` array that overlaps 16-bit registers (JASTrack registers 0x28-0x2B), two `u16` halves read as one word.
- **Use after delete through a stale vtable**: code that deletes an object and keeps calling it worked on the GameCube because the freed block kept the derived vtable; Clang's destructors leave the base (often abstract) vtable.
- **Traps the GameCube does not have**: integer division or modulo by zero (`p2_div`, `p2_mod`).
- **Cooperative scheduler races** (port/sdk/os_thread.cpp): anything that changes `g_current` while the lock is dropped around interrupt handlers.
- **Decomp header values that differ from the SDK**: under TARGET_PC game code sees Aurora's (correct) SDK enums, so a decomp header with a wrong value changes behaviour (GX_CTF_R8 in the decomp is really GX_CTF_A8: the shadow mask copy). Use PC_SWITCH with the SDK name at the call site.
- **Sizes derived from element counts**: J3D's mTexCoordNum assumes 8-byte float UVs; host vertex arrays are sized from the VTX1 block bounds instead.
- **Aurora differences**: fixed by wrappers in `port/sdk/*_fixes.cpp` (via macros in `p2_sdk_compat.h`) or patches in `port/patches/aurora/` (applied to the submodule by CMake).

## Host-only stubs to replace

| File | What | Replace in |
|---|---|---|
| `port/sdk/os_stubs.cpp` | threads, mutex, cond, alarms, reset, ROM font | M2 |
| `port/sdk/vi.cpp` | retrace and frame pacing | M2 |
| `port/sdk/hw_stubs.cpp` | GBA, a few GX calls, `C_MTX44Copy` | as needed |
| `port/audio/audio.cpp` | AI DMA (SDL3 stream) and a software DSP; no oscillator voices, reverb or filters yet | M3 |
| `port/sdk/gx_extras.cpp` | draw sync, fog adjust table (real), indirect mask | as needed |
