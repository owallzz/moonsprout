# mppm launcher: settings specification

Status: implementation-ready spec. No code has been changed for it yet.

## Review notes

Changed in the cross-spec review (see `README.md` in this folder for the full list of decisions):

- `video.aspect` is now **`video.aspectRatio`** with the value set of ASPECT.md section 4; the
  reserved `wide` value and the "Widescreen" experimental row are gone. Internal resolution counts
  448-line multiples, not 640x480.
- `interface.reducedMotion` is now **`interface.reduceMotion`** (body class `reduce-motion`), the
  name DESIGN.md and SCENE.md use.
- Keys the other three specs need were added to section 9.1: `interface.hudLayout`,
  `interface.hudMaxAspect` (ASPECT.md), `interface.highContrast`, `interface.glassEffects`,
  `interface.launchAnimation` (DESIGN.md; it was `prelaunch.launchAnimation` there),
  `interface.sceneArea`, `interface.sceneTime`, `interface.sceneMusic` (SCENE.md section 12).
- Menu Size follows DESIGN.md 9.3: Automatic, then 75-130% in steps of 5 (above 4/3 of the window-fit base the layout no longer fits; a stored 135-200 still loads and shows the size used).
- Test-only environment variables from ASPECT.md and SCENE.md were added to the table in 1.3.
- The `launcherOpen` title gate (section 6) is not needed in v1: per SCENE.md the title section
  never runs behind the start menu.
- The product name is undecided (DESIGN.md 1.2 proposes "Sproutlight"). Every "mppm" in user-facing
  copy below stands for the product name and must come from one constant, not be typed into strings.

Path prefixes used below:

- **M:** this repository
- **A:** `M:extern/aurora`
- **B:** `M:extern/borealis`
- **D:** a Dusklight checkout (reference only, CC0)

Every `file:line` was checked against the tree at commit `b6a510681` unless it is marked
"(from research notes)".

---

## 0. Decisions at a glance

1. **One settings store.** `borealis::config` (`B:include/borealis/config.hpp`), already wired in
   `M:port/launcher/settings.{hpp,cpp}` and loaded in `M:port/launcher/launcher.cpp:39-51`. File:
   `<SDL_GetPrefPath(nullptr, "mppm")>/config.json` (`launcher.cpp:30-35`). Only user values are written, unknown keys are kept
   (`B:src/config.cpp:371,456`), autosave 1 s after a change (`config.hpp:386`), flushed on close.
2. **Keys are the public interface.** The dotted key is the save format, the `--cvar` name and the
   documented name. Keys never get renamed; a rename uses `VarOptions::aliases` (`config.hpp:126`).
3. **Layers.** default < user (config.json) < `--cvar` overlay < environment overlay < netplay
   overlay. Overlays are never saved. A var supplied by the environment or netplay overlay is
   *locked*: its control is disabled and shows why.
4. **Game code never sees the config library.** The launcher mirrors the handful of values the
   game needs into a plain C struct, `gP2Config` (new `M:port/include/p2_config.h`). The game reads
   fields; `p2_env_flag` / `p2_env_float` go away. All game-side reads stay inside `PC_ONLY(...)`,
   so the MWCC build is untouched.
5. **The game's own options (music volume, effects volume, sound mode, rumble) are owned by mppm's
   config**, synchronised both ways with the game's Options screen. The memory card copy becomes a
   passive mirror.
6. **Eight tabs**, in this order: PRELAUNCH (launcher only), VIDEO, INPUT, AUDIO, GAMEPLAY, CHEATS,
   INTERFACE, TOOLS. A curated set rather than a long one.
   Anything unproven sits in an **Experimental** group that is hidden until *Advanced settings* is
   on, and is never part of a preset.
7. **Restart handling:** eight keys need a restart. A boot snapshot detects pending changes; a
   footer banner and one modal offer "Restart now" (process re-exec).
8. **Netplay:** every key carries a class: **L** (local, free to differ), **S** (part of the
   simulation, the host's value is forced on everyone) or **X** (forced to a fixed value online).
   Enforcement is one `Overlay`, so no settings code needs to know about netplay.
9. **In-game menu** on F1 / Back (or R+Start): Settings, Controllers, Reset Game, Start Menu, Quit,
   with reserved slots for Mods and Online.
10. **First run:** a two-card preset screen ("Original" / "Enhanced") that also asks for
    update-check consent.

Legend used in the tables:

| Tag | Meaning |
|---|---|
| **RT** | Applies immediately |
| **RS** | Needs a restart of mppm (restart banner) |
| **NL** | Read once at the next launch; no restart prompt |
| **LO** | Launcher only (PRELAUNCH tab; not shown in the in-game menu) |
| **v1** | Ships with the first launcher release: the hook exists today or is host-side only |
| **v1.1** | Needs a small `PC_ONLY` hook in game code |
| **later** | Key reserved, no UI yet |
| **Adv** | Shown only when `interface.advancedSettings` is on |
| **Exp** | Experimental group (implies Adv) |

---

## 1. Settings model

### 1.1 Store and namespaces

`M:port/launcher/settings.hpp` grows from two groups to eight. Group names are the key prefixes:

| Prefix | Contents | Tab(s) |
|---|---|---|
| `backend.` | Boot-time host choices: disc, graphics backend, save format | PRELAUNCH |
| `video.` | Window, resolution, filtering, FPS counter | VIDEO |
| `input.` | Global input behaviour (bindings live in Aurora's own files) | INPUT |
| `audio.` | Volumes, output | AUDIO |
| `game.` | Behaviour that is not a cheat | GAMEPLAY |
| `cheat.` | Everything that changes difficulty or progress | CHEATS |
| `interface.` | The mppm UI itself | INTERFACE |
| `tools.` | Diagnostics and developer switches | TOOLS |
| `online.` | Online play: the lobby server and the player's name (section 18; `net.` was the reserved name until M5.3b) | ONLINE |
| `mods.` | **Reserved**: `mods.enabled.<id>`, `mods.<id>.<option>` | future MODS tab |

The two existing keys keep their names: `backend.discPath`, `backend.skipLauncher`
(`settings.cpp:9-10`), `interface.uiScale`, `interface.controllerToasts` (`settings.cpp:13-14`).

Types: `bool`, `int`, `std::string`, and enums. Enums are stored as lower-case strings through
`config_enum_values` tables (`config.hpp:56-66`), never as numbers, so the file stays readable and
values can be added later. Percentages are `int` (0-100, 50-200), never float.

### 1.2 Metadata table

`settings.cpp` gets one table that every other mechanism reads. It is the single place where a new
option is declared.

```cpp
enum class Apply : uint8_t { Runtime, Restart, NextLaunch };
enum class Net   : uint8_t { Local, Synced, Forced };

struct Meta {
    borealis::config::VarBase* var;
    Apply apply;
    Net net;
    const char* env;        // MPPM_* override name, or nullptr
    const char* netValue;   // Forced: value in set_string() syntax; Synced: nullptr (host's value)
};
std::span<const Meta> settings_meta();
```

Uses: the environment overlay (1.3), restart detection (section 11), the netplay overlay
(section 13), "Reset this tab" and "Reset all settings" (`VarBase::reset()`, `config.hpp:233`).

**Start-menu class (`MenuApply`).** How a change made on the start menu applies
(live_options_design 3.5); in game `Apply` alone decides. `menu_apply(meta)` defaults from `Apply`
(Runtime: Live, Restart: Relaunch, NextLaunch: None) with two override lists in `settings.cpp`:

| Class | Keys | On the start menu |
|---|---|---|
| Live | every Runtime key, plus `video.msaa`, `interface.liveBackground`, `sceneArea`, `sceneTime`, `sceneMusic` | applies at once |
| AtBoot | `backend.discPath`, `backend.cardFileType`, `audio.enabled`, `audio.outputDevice`, `tools.audioDumpPath` | nothing before the game boots (the boot reads them); a relaunch once it has booted behind the menu |
| Relaunch | the other Restart keys: `backend.graphicsBackend`, `video.allowCpuAdapter` | a short relaunch back to the same Settings row (section 11) |
| None | the NextLaunch keys | read each time the app starts; no badge |

`menu_action(meta, gameBooted)` turns the class into Nothing, Live or Relaunch.

### 1.3 Overrides: environment and command line

Built in `launcher::preinit` (`launcher.cpp:39`), right after `borealis::config::load`:

- **`--cvar key=value`** (repeatable). `BOREALIS_ENABLE_CLI` is off (`M:CMakeLists.txt:65`), so
  `preinit` scans argv itself and calls `borealis::config::apply_overrides` (`config.hpp:436`).
  Values land in `cli_overlay()`, which a later user edit releases (`config.hpp:438`).
- **Environment overlay:** `Overlay env{"environment", {.priority = 200, .releaseOnUserSet = false}}`.
  For each `Meta` with `env != nullptr` and the variable set, call `var.set_string` semantics into
  the overlay. The var reports `locked()` (`config.hpp:230`), the control is disabled, and its help
  pane gets a line: "Set by the environment variable MPPM_X. Remove it to change this here."
- **Flag parsing is unified** on today's `p2_env_flag` rule (`M:port/sdk/os_report.cpp:37-41`):
  unset or empty = no override; `0` = false; anything else = true. This changes four switches that
  today are "set at all = on" (`MPPM_FPS`, `MPPM_AUDIO_DEBUG`, `MPPM_AUDIO_VLOG`,
  `MPPM_SCHED_DEBUG`): `MPPM_FPS=0` now means off. `MPPM_AUDIO` used `atoi(...) == 0` = off
  (`M:port/audio/audio.cpp:44-45`); only the literal `0` turns audio off from now on. Update
  `M:docs/PORT_STATUS.md:33-44` in the same change.
- **Positional disc argument** stays as it is (`launcher.cpp:41-45`): first argument that does not
  start with `-`, else `MPPM_DISC`. It is a boot path, not a setting: it is never written to
  `backend.discPath` and it skips the start menu (`launcher.cpp:93`).

Complete mapping of today's switches:

| Environment variable | Read today at | Becomes key | Notes |
|---|---|---|---|
| `MPPM_DISC` | `launcher.cpp:43` | (boot path, no key) | Unchanged. Skips the start menu |
| `MPPM_SKIP_LAUNCHER` | `launcher.cpp:93` | `backend.skipLauncher` | Overlay value `true` |
| `MPPM_FORCE_LAUNCHER` | `launcher.cpp:94` | (no key) | Test-only; stays a raw env check |
| `MPPM_ALLOW_CPU_ADAPTER` | `M:port/main.cpp:147-148` | `video.allowCpuAdapter` | RS |
| `MPPM_FPS` | `main.cpp:93` (function-local static) | `tools.logFps` | stderr log, distinct from the on-screen counter |
| `MPPM_PANIC_SCREEN` | `M:src/sysGCU/system.cpp:129`, `M:src/JSystem/JUtility/JUTException.cpp:166` | `tools.panicScreen` | The JUTException site uses raw `getenv` today |
| `MPPM_AUDIO` | `audio.cpp:44` | `audio.enabled` | RS |
| `MPPM_AUDIO_DUMP` | `audio.cpp:84` | `tools.audioDumpPath` | `persist = false`, no UI |
| `MPPM_AUDIO_DEBUG` | `audio.cpp:153,589`, `M:port/sdk/os_thread.cpp:426` | `tools.audioDebug` | |
| `MPPM_AUDIO_VLOG` | `audio.cpp:389` | `tools.audioVoiceLog` | |
| `MPPM_SCHED_DEBUG` | `os_thread.cpp:282` | `tools.schedDebug` | |
| `MPPM_UNLOCK_CHALLENGE` | `M:src/plugProjectKandoU/gamePlayCommonData.cpp:188` | `cheat.unlockChallenge` | |
| `MPPM_ALLOW_SUNSET` | `M:src/plugProjectKandoU/singleGameSection.cpp:1048` | `cheat.sunsetOnDayOne` | |
| `MPPM_TIME_SCALE` | `M:src/plugProjectYamashitaU/timeMgr.cpp:167` | `cheat.daySpeed` | Float factor; overlay value = `round(factor * 100)` |
| `MPPM_WINDOW=WxH` | new (ASPECT.md 10) | `video.windowWidth`, `video.windowHeight` | Test window shape |
| `MPPM_SCENE_AREA`, `MPPM_SCENE_TIME` | new (SCENE.md 8.3) | `interface.sceneArea`, `interface.sceneTime` | |
| `MPPM_FRAME_RATE` | new (fps_design 3.1) | `video.frameRate` | Same syntax as the file; sanitized |
| `MPPM_VSYNC` | new (fps_design 3.1) | `video.vsync` | Flag; lets headless tests set VSync |
| `MPPM_SMOOTH_MOTION` | new (fps_design 3.1) | `video.smoothMotion` | Flag; row is Experimental (FPS phase 2) |
| `MPPM_SCENE_TUNE`, `MPPM_SCENE_AUTOPLAY`, `MPPM_HUD_ANCHORS`, `MPPM_WIDE_TEST` | new (SCENE.md 8.3, ASPECT.md 7, 10) | (no key) | Test-only; stay raw env checks |

Headless test scripts keep working unchanged: every variable they use still exists and still wins.

### 1.4 Bridge to game code: `gP2Config`

`p2_env_float` caches one value forever (`os_report.cpp:45-54`), so it cannot serve live settings.
Replace both helpers with a mirror struct.

New `M:port/include/p2_config.h` (plain C, included by `p2_host.h`):

```c
typedef struct P2Config {
    /* cheats / gameplay (read on the game thread) */
    float daySpeed;            /* 1.0 = normal, 0 = sun stopped */
    int   sunsetOnDayOne;
    int   unlockChallenge;
    int   invincibleCaptains;
    int   infiniteSprays;
    int   idleDemo;            /* title attract movies */
    int   skipBootLogos;
    int   titleSeason;         /* -1 = original behaviour, 0..3 = fixed */
    int   hideHud;
    /* boot-time */
    int   cardFileType;        /* CARDFileType */
    int   audioEnabled;
    /* diagnostics */
    int   panicScreen;
    /* launcher state */
    int   launcherOpen;        /* start menu is up: hold title timers */
} P2Config;
extern P2Config gP2Config;     /* defined in port/sdk/os_report.cpp, defaults = original game */
```

- Defined game-side with defaults that equal the original game, so a build without the launcher
  (`haveLauncher == false`, `main.cpp:152,160`) behaves as today.
- Filled by `mppm::bind_game_config()` in `settings.cpp`, called from `preinit` after overlays are
  applied. It uses `Var::observe` (`config.hpp:325-326`): runs once immediately, then on every
  change.
- Writes are safe without locks: config changes happen in `launcher::update()`, which runs on the
  game thread while no other game thread is active (`main.cpp:113-115`).
- Values read **off** the game thread do not go in the mirror. They get their own atomics and
  setters next to the reader: master gain and the debug flags in `audio.cpp` (mixer thread), the
  scheduler debug flag in `os_thread.cpp` (timer thread).
- After migration, delete `p2_env_flag` / `p2_env_float` from `p2_host.h:35-37` and
  `os_report.cpp:37-55`.

### 1.5 Binding vars to controls

`BOREALIS_ENABLE_UI` is off (`M:CMakeLists.txt:64`); the UI is the Dusklight port in
`M:port/launcher/ui/`. It has no config helpers yet. Add `M:port/launcher/ui/config_bind.hpp`, a
copy of `B:include/borealis/ui/config.hpp` (MIT) retargeted to `mppm::ui`:
`bind(Var<bool>&, BoolButton::Props)`, `bind(Var<int>&, NumberButton::Props)`,
`bind_dropdown(Var<Enum>&, ...)`. They fill `getValue`, `setValue`, `isModified` (`var.modified()`)
and `isDisabled` (`var.locked() || caller's predicate`).

Row anatomy in the settings window (left pane = controls, right pane = help for the focused row,
as `D:src/dusk/ui/settings.cpp:494-495`):

- Label, current value, a dot when the value differs from the default.
- Badges, in this priority: lock ("Set by environment" / "Set by the host"), "Restart required",
  "Next launch", "Experimental".
- Help pane: the help text below, then badge explanations, then "Default: <value>" and a
  **Reset** button when modified.
- Every user change plays `kSoundItemChange` (`M:port/launcher/ui/sound.hpp:19`) if
  `audio.menuSounds` is on.

---

## 2. PRELAUNCH (launcher only)

Shown only by `SettingsWindow(prelaunch = true)`, opened from the start menu. Everything here is
needed before the game boots, or cannot change while it runs.

| Label | Key | Type | Default | Applies | Net | Phase |
|---|---|---|---|---|---|---|
| Disc Image | `backend.discPath` | string | `""` | RS* | L | v1 |
| (hidden) | `backend.discVerification` | enum `unknown` / `verified` / `mismatch-accepted` | `unknown` | - | L | v1 |
| (hidden) | `backend.discFingerprint` | string `"<size>:<mtime>"` | `""` | - | L | v1 |
| Verify Disc | (button) | | | | | v1 |
| Save Manager | (button) | | | | | v1 |
| Data Folder | (button row) | | | | | v1 |
| Graphics Backend | `backend.graphicsBackend` | string | `"auto"` | RS | L | v1 |
| Save Format | `backend.cardFileType` | enum `gci-folder` / `card-image` | `gci-folder` | RS | L | v1, Adv |

\* RS only when a game is already booted behind the start menu; before the first boot it applies on
Play.

**Disc Image**
- Control: select button showing the file name, or "(none)". Opens the picker
  (`borealis::file_select`, already used at `M:port/launcher/prelaunch.cpp:158`).
- Help: "The Pikmin 2 disc image mppm plays from. Use your own copy of the GameCube game (USA).
  Supported formats: ISO, GCM, RVZ, CISO, GCZ, WIA. Nothing is copied or changed."
- Hook: read at `launcher.cpp:85-86` (`disc::inspect` must return `State::Ready`,
  `M:port/launcher/disc.hpp:11-17`), opened by `aurora_dvd_open` at `main.cpp:173`.
- Notes: on change, clear `backend.discVerification` and `backend.discFingerprint`. The status line
  under the control uses `disc::state_label` (`disc.hpp:31`).

**Verify Disc**
- Help: "Reads the whole disc and compares it with a known good copy. This can take a while on a
  slow drive. mppm works without it, but a bad dump can cause crashes that look like bugs."
- Hook: `borealis::disc` (linked at `M:CMakeLists.txt:174-175`), worker thread, progress modal with
  Cancel. On mismatch: "Continue anyway" stores `mismatch-accepted`. Re-run automatically only when
  `backend.discFingerprint` no longer matches the file.

**Save Manager**: opens the window in section 10. Disabled until a valid disc is set.
Help: "Import, export, back up and delete your Pikmin 2 saves."

**Data Folder**
- Shows the path (`AuroraInfo.userPath`). One button: **Open Folder** (`SDL_OpenURL("file://...")`).
- Help: "Where mppm keeps your settings, saves, controller layouts and logs."
- Changing the location (as `D:src/dusk/ui/settings.cpp:541-572`) is not in v1; it needs
  `borealis::data`, which is not linked.

**Graphics Backend**
- Choices: "Automatic" plus each entry of `aurora_get_available_backends()` (`A:include/aurora/aurora.h:143`)
  except `BACKEND_NULL`. Stored ids: `auto`, `vulkan`, `d3d12`, `d3d11`, `metal`, `opengl`,
  `opengles`, `webgpu` (same strings as `D:src/dusk/ui/settings.cpp:120-208`).
- Help: "How mppm talks to your graphics card. Leave this on Automatic unless you see graphical
  problems or a crash at startup."
- Hook: `AuroraConfig.desiredBackend` (`aurora.h:87`), filled in `launcher::preinit`
  (`launcher.cpp:39-40`, currently `(void)config`). The row shows the *running* backend from
  `aurora_get_backend()` (`aurora.h:142`) unless a change is pending.
- Notes: an unknown or unavailable id falls back to `auto` without rewriting the file. When the
  backend asked for at boot could not start and Aurora fell back to another, its entry reads
  "OpenGL ES (failed, using Vulkan)" and the help pane says so; choosing an entry that resolves to
  the running backend (Automatic after such a fallback) does not ask for a restart.

**Save Format** (Adv)
- Choices: "GCI Folder (recommended)", "Memory Card Image".
- Help: "How saves are stored on disk. GCI Folder keeps one file per save and works directly with
  Dolphin. Memory Card Image keeps a single .raw card file. Changing this does not move your saves;
  use the Save Manager to import them."
- Hook: new `PC_ONLY(CARDSetLoadType((CARDFileType)gP2Config.cardFileType);)` directly before
  `CARDInit("GPVE", "01")` at `M:src/sysGCU/memoryCard.cpp:166` (`CARDSetLoadType`:
  `A:include/dolphin/card.h:253`; enum at `card.h:20-22`).

---

## 3. VIDEO

| Group | Label | Key | Type | Default | Range / choices | Applies | Net | Phase |
|---|---|---|---|---|---|---|---|---|
| Display | Fullscreen | `video.fullscreen` | bool | `false` | | RT | L | v1 |
| Display | Aspect Ratio | `video.aspectRatio` | enum | `original`; becomes `auto` when ASPECT.md phase 1 ships | `auto`, `original`, `16:10`, `16:9`, `21:9`, `stretch` | RT | L since M5.1b: the simulation frustum uses a session constant (`p2_net_sim_k`), not the window's shape | v1: `original`, `stretch`; the others with ASPECT.md phase 1 |
| Display | VSync | `video.vsync` | bool | `true` | | RT | L | v1 |
| Display | Frame Rate | `video.frameRate` | string | `original` | `original`, `display`, `60`, `90`, `120`, `144`, `165`, `240`, `unlimited`; the file takes any integer 30-1000 | RT | L | FPS phase 1 |
| Display | Reset Window Size | (button) | | | | RT | | v1 |
| (hidden) | | `video.windowWidth`, `video.windowHeight` | int | `0` | 0 = default, else 320-16384 | NL | L | v1 |
| Quality | Internal Resolution | `video.internalResolution` | int | `0` | 0 = Auto, 1-8 = multiples of 448 lines; width follows the aspect | RT | L | v1 |
| Quality | Output Scaling | `video.resampler` | enum | `bilinear` | `bilinear`, `area` | RT | L | v1 |
| Quality | Texture Filtering | `video.anisotropy` | int | `1` | 1, 2, 4, 8, 16 | RT | L | v1 |
| Overlay | FPS Counter | `video.fpsCounter` | enum | `off` | `off`, `top-left`, `top-right`, `bottom-left`, `bottom-right` | RT | L | v1 |
| Advanced | Allow Software Rendering | `video.allowCpuAdapter` | bool | `false` | | RS | L | v1, Adv |
| Experimental | Anti-Aliasing (MSAA) | `video.msaa` | int | `1` | 1, 4 | RS (live on the start menu) | L | v1, Exp |
| Display (Experimental) | Smooth Motion | `video.smoothMotion` (alias `video.gameplay60`) | bool | `false` (`true` at graduation) | | RT | X (off) until the purity check passes, then L | key in FPS phase 1, row in phase 2 (stage 2a) |

**Fullscreen**
- Help: "Fill the whole screen. You can also press F11 or Alt+Enter at any time."
- Hook: startup `AuroraConfig.startFullscreen` (`aurora.h:91`); runtime `VISetWindowFullscreen`
  (`A:include/dolphin/vi.h:36`). F11 and Alt+Enter are handled in `launcher::handle_event`
  (`launcher.cpp:64`) and write the var, so the setting and the window never disagree. Neither key
  is bound by the default keyboard layout (`main.cpp:67-79`).

**Aspect Ratio**
- Choices, meaning and Aurora state: ASPECT.md section 4. Labels: "Automatic (fill the window)",
  "Original (4:3)", "16:10", "16:9", "21:9", "Stretch to Window". Until ASPECT.md phase 1 ships
  only "Original (4:3)" and "Stretch to Window" are listed.
- Help: "Automatic shows more of the world on wide screens without stretching anything. Original
  keeps the picture exactly as on a GameCube, with bars on wide screens. Stretch fills the window
  and makes everything wider than intended."
- Hook: `mppm::video::apply()` (ASPECT.md section 5) is the only caller of
  `AuroraSetViewportPolicy`, `VILockAspectRatio` and `VISetFrameBufferScale`. It runs once after
  `aurora_initialize`, then on change. While the start menu is open it applies `auto` whatever the
  setting says, so the menu and the live scene always fill the window (ASPECT.md section 5).
- Wide aspects change what a player can see, so online the key is **S** for now (section 14).

**VSync**
- Help: "Match the picture to your monitor's refresh rate to avoid tearing. Turn it off only if you
  notice input delay. The game's speed does not change either way."
- Hook: startup `AuroraConfig.vsync` (`aurora.h:90`); runtime `aurora_enable_vsync`
  (`A:include/aurora/gfx.h:34`). The simulation is paced by the 59.94 Hz retrace timer
  (`os_thread.cpp:269-308`), so VSync never changes the game's speed. It reaches the game thread
  only through Aurora's two frame slots: the host skips a present rather than wait on a slow one,
  so ticks stay on time while one present completes within two tick periods (fps_design 5.3).
  It is also an input of the frame policy (Frame Rate, below). Environment: `MPPM_VSYNC`.

**Frame Rate**
- Choices, built when the window opens: "Original (30 / 60)", "Match Display (144 Hz)" (plain
  "Match Display" when the refresh rate is unknown), "60 FPS", "90 FPS", "120 FPS", "144 FPS",
  "165 FPS", "240 FPS", "Unlimited". A number in the file that the list does not offer is
  appended as "N FPS (custom)".
  An unknown value is sanitized to Original when the file loads, and the file holds the clean
  value from the next save on.
- Sanitizer (`sanitize_frame_rate`): trim and lower-case; `0` (or `00`…), `off`, `uncapped` become
  `unlimited`; `match`, `auto` become `display`; a whole-string integer (leading zeros ignored)
  clamps to 30-1000; anything else becomes `original`. Environment: `MPPM_FRAME_RATE` (same syntax).
- Help (phases 1-2): "How many pictures mppm shows each second. Original keeps the game as on a
  GameCube: 30 in the field, 60 in menus. Higher settings make the start menu and mppm's own
  menus smoother. In the field the game still shows 30 pictures a second unless Smooth Motion is
  on. The game's speed never changes." From graduation the fourth sentence becomes "With Smooth
  Motion on, the game itself is drawn at up to this rate."
- Live note while VSync is on, the refresh rate is known and the limit is above it: "VSync is on,
  so no more than your display's N Hz is shown."
- Meaning: the game's logic never changes rate; ticks stay on the 59.94 Hz retrace timer.

  | Value | Field and caves | 60 Hz game sections | Start-menu scene | mppm's own menus |
  |---|---|---|---|---|
  | Original | 30, as on a GameCube | 60 | 59.94 fixed tick | display rate |
  | 60 to 240, Match Display | 30 until Smooth Motion, then up to N | 60 | N | N |
  | Unlimited | 30 until Smooth Motion, then GPU-bound | 60 | unbounded with VSync off | max(refresh, 240) |

  A limit below the tick rate (30 in a 60 Hz section, a 50 Hz display) skips presents and never
  slows ticks.
- Hook: `mppm::launcher::frame_policy()` (`port/launcher/frame_policy.cpp`,
  `mppm/frame_policy.hpp`) derives the cap, the present interval and the sleep slots from
  `video.frameRate`, `video.smoothMotion`, `video.vsync` and the refresh rate of the window's
  display (`video::display_refresh_hz()`, re-read on `SDL_EVENT_WINDOW_DISPLAY_CHANGED` and
  `SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED`). The host loops in `port/main.cpp` read it every
  frame, so a change applies at once. With `tools.logFps` each new policy is logged:
  `mppm: display 60.00 Hz, vsync off, frame rate 120 -> cap 120.0 Hz, sleep 8.33 ms, ui sleep 8.33 ms`.
  Derivation (R = refresh rate, V = VSync):

  | Setting | V, R | cap | game-frame sleep |
  |---|---|---|---|
  | `original` | V and R known | R (skipping only) | none |
  | `original` | otherwise | none | none |
  | `display` | V, R known | R | 1/(1.05 R) |
  | `display` | V, R unknown | none | none |
  | `display` | not V | R, or 60 if unknown | 1/cap |
  | N | V, R known | min(N, R) | 1/N if N < R, else 1/(1.05 R) |
  | N | otherwise | N | 1/N |
  | `unlimited` | V, R known | R | 1/(1.05 R) |
  | `unlimited` | otherwise | none | none |

  UI-only loops (the hold loop, the standalone start menu) sleep to the `display` row for
  `original` and `display`, 1/N for a number and 1/max(R, 240) for `unlimited`. Where that row
  gives no sleep (VSync on, refresh unknown) they keep 1/60, so a swapchain that does not block
  cannot make them spin.
- In the start-menu scene a rate above Original runs one scene tick per shown picture (FreeRun):
  the scene renews the request every tick and takes each tick's real length as its time step, so
  the take-off has the same timing at any rate. Every other section keeps the 59.94 Hz ticks.
- The FPS counter counts new pictures once a game runs (`p2_host_picture_fps`), not presents.

**Smooth Motion** (`video.smoothMotion`, Experimental row after Frame Rate; FPS phase 2, stage 2a)
- The key replaces `video.gameplay60`, which it reads as an alias. Environment:
  `MPPM_SMOOTH_MOTION`. Forced off online (net class X) until graduation.
- Row (`Tier::Experimental`, so shown with Advanced Settings on), disabled with "Choose a Frame
  Rate above Original first." while the rate is `original`. Help: "Draws in-between pictures so
  the game moves at up to the Frame Rate. The game's speed and rules do not change. Characters
  and the camera move smoothly; particles, shadows and the HUD still change 30 times a second.
  Off during cutscenes, pauses and two-player games. May cause small visual glitches."
- What it does (fps_design 7, `port/game/present/Present.cpp`, loop in `port/main.cpp`): at the
  end of every field or cave tick the host captures the inputs of the draw matrices of every J3D
  model that entered the draw buffers (edit G4) and every camera's view. While it waits for the
  next 30 Hz tick it draws in-between pictures: the models and cameras are set to
  `lerp(previous tick, newest tick, alpha)`, the section draws once more, and the tick's values are
  copied back before any game code runs. `alpha` is wall time since the newest tick became due,
  over one tick period. The tick's own picture is still shown (it equals alpha 0), so with
  `MPPM_SMOOTH_ALPHA=1` every in-between picture matches the next tick's picture pixel for pixel.
- Engages only when the cap exceeds the section's tick rate by more than 5 Hz (or there is no
  cap), the game is not held by a menu, and in story (`SingleGameSection` field or cave state) or
  one-player challenge (`VsGameSection` game state) play: not in two-player games, movies,
  pauses, freezes, fades, resets, DVD errors, save screens, or on frames that capture the pause
  backdrop or the cave radar. The history is dropped there and on every section change.
- Still steps at 30 Hz (stage 2a): particles' world positions and animation, cylinder and joint
  shadows (drawn at the newest tick, so up to half a tick ahead of a moving model), the HUD and
  2D screens, life gauges, carry numbers, grass and other direct-draw shapes (at their tick
  positions, under the smooth camera), material and texture animation, and models skinned on the
  CPU. Pikmin leaves follow their Pikmin (they are drawn from the Pikmin's joint matrices).
- Log (`tools.logFps`): `interp` counts in-between pictures, `shown` includes them, `mode smooth`
  marks a window whose ticks were reached through the Smooth Motion loop.
- Test hooks: `MPPM_SMOOTH_ALPHA=<0..1>` fixes alpha; `MPPM_PRESENT_CHECK=1` (or `2` for every
  range) runs the purity check of fps_design 7.5 once a second and logs `mppm: present check`
  lines (stage 2 counts state that moves on with every draw; render-only and asynchronous writes
  are filtered).

**Window size (hidden) and Reset Window Size**
- The window size is always remembered; there is no "remember" toggle. Written on
  `AURORA_WINDOW_RESIZED` (`A:include/aurora/event.h:22`) while not fullscreen; forward that event
  type from `pumpEvents` (`main.cpp:29-48`, which today forwards only `AURORA_SDL_EVENT`).
- Startup: `AuroraConfig.windowWidth/Height` (`aurora.h:98-99`). `0` leaves Aurora's default.
- Button: `VISetWindowFullscreen(false)`, `VISetWindowSize(1280, 960)`, `VICenterWindow()`
  (`vi.h:36-40`), then resets both vars.
- Help: "Return to a 1280x960 window in the middle of the screen."

**Internal Resolution**
- Control: stepper showing "Auto (matches window)" or "Nx (WxH)", e.g. "3x (2389x1344)" at 16:9.
- Help: "The resolution the game is drawn at. Auto matches your window and looks sharpest. Lower
  values run faster on slow graphics hardware; 1x is the original GameCube resolution."
- Hook: `VISetFrameBufferScale((float)n)` (`vi.h:46`; `0` = follow the window,
  `A:lib/window.cpp:457`). Apply after `aurora_initialize` and on change.
- Notes: uses a live tuner overlay like `D:src/dusk/ui/graphics_tuner.cpp` so the picture stays
  visible while stepping. Verify the shadow-mask copy and 2D particles at 1x, 2x, 3x and Auto before
  release (see commit `164ada6ee`).

**Output Scaling**
- Choices: "Smooth (Bilinear)", "Sharp Downscale (Area)".
- Help: "How the picture is resized to fit the window. Smooth suits most setups. Sharp Downscale
  looks cleaner when Internal Resolution is higher than your window."
- Hook: `aurora_set_resampler(SAMPLER_BILINEAR / SAMPLER_AREA)` (`aurora.h:138`, enum `aurora.h:15-18`).

**Texture Filtering**
- Choices: "Original", "2x", "4x", "8x", "16x".
- Help: "Keeps ground and wall textures sharp at shallow angles. Costs almost nothing on modern
  graphics cards."
- Hook: `AuroraConfig.maxTextureAnisotropy` (`aurora.h:89`) at startup, then
  `aurora_set_texture_anisotropy` (port/patches/aurora/0006) from `video::apply()`: the samplers
  built from the next frame on use the new value, in game too.

**FPS Counter**
- Choices: "Off", "Top Left", "Top Right", "Bottom Left", "Bottom Right".
- Help: "Show the current frame rate in a corner of the screen."
- Hook: the passive overlay document reads `aurora_get_fps()` (`gfx.h:32`). One enum replaces
  Dusklight's bool + corner pair (`D:src/dusk/ui/settings.cpp:721-770`).

**Allow Software Rendering** (Adv)
- Help: "Let mppm run on a CPU renderer when no graphics card is available. Very slow; meant for
  testing."
- Hook: `AuroraConfig.allowCpuAdapter` (`aurora.h:95`); replaces `main.cpp:146-149`.

**Anti-Aliasing (MSAA)** (Exp)
- Choices: "Off", "4x". 2x and 8x are not supported by every adapter for the frame buffer format
  and a WebGPU error is fatal once Aurora runs; a stored 2 reads as Off, 8 as 4x.
- Help: "Smooths jagged edges. Character shadows may render incorrectly with it on. Raising
  Internal Resolution is the safer way to get a cleaner picture."
- Hook: `AuroraConfig.msaa` (`aurora.h:88`). MSAA above 1 disables Aurora's normal buffer and the
  EFB-alpha shadow copy needs re-testing (research notes: `A:lib/webgpu/gpu.cpp:428,1078`). Promote
  out of Experimental once shadows are confirmed at 4x. On the start menu a change applies at once
  through `aurora_set_msaa` (0006: the next surface refresh rebuilds the frame buffers and drops the
  caches keyed on the sample count); in game it keeps the restart badge and banner.

Not offered: Deflicker (`M:src/sysGCU/commonSaveData.cpp:175`, no visible effect on PC), a
"remember window size" toggle, shadow resolution, bloom.

---

## 4. INPUT

| Group | Label | Key | Type | Default | Applies | Net | Phase |
|---|---|---|---|---|---|---|---|
| Controllers | Configure Controllers | (opens window, section 8) | | | RT | L | v1 |
| Controllers | Rumble | `input.rumble` | bool | `true` | RT | L | v1.1 |
| Controllers | Player Colour Light | `input.playerLed` | bool | `true` | RT | L | v1 |
| General | Input While in Background | `input.backgroundInput` | bool | `true` | RT | L (X off while a menu is typing) | v1 |
| General | Menu Button | (info row) | | F1 / Back / R+Start | | | v1 |

**Configure Controllers**
- Help: "Choose which controller or keyboard each player uses, and change every button."
- Stored by Aurora, not in `config.json` (section 8).

**Rumble**
- Help: "Controller vibration when a captain takes damage and during big moments. This is the same
  option as in the game's own Options screen."
- Hook: game-owned option `sys->mPlayData->mIsRumble` (`M:include/Game/Data.h:178`). See section 5.1
  for the two-way sync. Runtime effect: `rumbleMgr->mIsRumbleActive`, which is only refreshed in
  `RumbleMgr::init` (`M:src/plugProjectNishimuraU/RumbleMgr.cpp:53-59`); the apply function sets
  both fields.

**Player Colour Light**
- Help: "Light up controllers that have a light bar in the player's colour: red for Player 1, blue
  for Player 2."
- Hook: `PADSetColor(port, r, g, b)` (`A:include/dolphin/pad.h:253`) when `PADHasLED(port)`
  (`pad.h:272`), on connect (`AURORA_CONTROLLER_ADDED`, `event.h:23`) and on change. Colours: port 0
  `#E8322B`, port 1 `#2F7BEA`, port 2 `#FFD23F`, port 3 `#8A4FC8`.

**Input While in Background**
- Help: "Keep reading your controller when the mppm window is not focused. Keyboard input always
  stops when you switch away."
- Hook: startup `AuroraConfig.allowJoystickBackgroundEvents` (`aurora.h:92`); runtime
  `aurora_set_background_input` (`aurora.h:137`).

**Menu Button** (read-only in v1)
- Text: "Open the mppm menu with F1 on a keyboard, or Back (or R + Start) on a controller."
- Source: `M:port/launcher/ui/input.cpp:16` (not configurable yet). Reserved keys for later:
  `input.menuKey` (scancode name), `input.menuButton` (per port, in the controller window).

Not offered: camera invert or sensitivity. Pikmin 2's camera is driven by buttons (L, R, Z), and the
C-stick steers the Pikmin group, so there is nothing to invert. Gyro, mouse aim and touch controls
have no target in this game.

---

## 5. AUDIO

| Label | Key | Type | Default | Range | Applies | Net | Phase |
|---|---|---|---|---|---|---|---|
| Master Volume | `audio.masterVolume` | int | `80` | 0-100, step 5 | RT | L | v1 |
| Music Volume | `audio.musicVolume` | int | `100` | 0-100, step 5 | RT | L | v1.1 |
| Effects Volume | `audio.effectsVolume` | int | `100` | 0-100, step 5 | RT | L | v1.1 |
| Sound Output | `audio.outputMode` | enum | `stereo` | `stereo`, `mono` | RT | L | v1.1 |
| Mute in Background | `audio.muteInBackground` | bool | `false` | | RT | L | v1 |
| Menu Sounds | `audio.menuSounds` | bool | `true` | | RT | L | v1 |
| Enable Audio | `audio.enabled` | bool | `true` | | RS | L | v1, Adv |
| Output Device | `audio.outputDevice` | string | `""` (system default) | | RS | L | later |

**Master Volume**
- Help: "Overall loudness of everything mppm plays."
- Hook (new): `extern "C" void p2_audio_set_master_gain(float)` in `audio.cpp`, storing a
  `std::atomic<float>`. Multiply it into `gain` at `audio.cpp:608`. Curve: `gain = (v / 100)^2`, so
  the slider feels even. Works before a disc exists and for launcher sounds.

**Music Volume**, **Effects Volume**
- Help (music): "Loudness of the background music."
- Help (effects): "Loudness of sound effects and Pikmin voices."
- Hook: `Mgr::setBgmVolume(f32)` (`commonSaveData.cpp:249`) and `Mgr::setSeVolume(f32)` (`:274`).
  Both assert `0 <= volume <= 1` and do nothing until `PSSystem::spSysIF` exists (`:262`, `:287`),
  so the apply function runs from the sync hook in 5.1, never earlier.

**Sound Output**
- Choices: "Stereo", "Mono".
- Help: "Stereo for speakers and headphones. Mono plays the same sound on both sides."
- Hook: `Mgr::setSoundModeStereo()` / `setSoundModeMono()` (`commonSaveData.cpp:216-238`).
  Surround is not offered: the mixer has no rear channels (`audio.cpp:610-613`). A card that holds
  Surround is read as `stereo`.

**Mute in Background**
- Help: "Silence the game while you are using another window."
- Hook: on `SDL_EVENT_WINDOW_FOCUS_LOST / _GAINED` in `launcher::handle_event`, call
  `p2_audio_set_master_gain(0)` / restore. Always muted while Aurora is paused
  (`AURORA_PAUSED`, `event.h:25`), whatever this setting says.

**Menu Sounds**
- Help: "Play sounds when you move through mppm's menus."
- Hook: gate in `ui::play_sound` (`sound.hpp:27`). The handler is installed once the game's sound
  system is up (`sound.hpp:2-3`); before that the launcher is silent.

**Enable Audio** (Adv)
- Help: "Turn the whole sound system off. Only useful for troubleshooting."
- Hook: `p2_audio_enabled()` (`audio.cpp:40-48`) returns `gP2Config.audioEnabled` and keeps its
  one-time latch, which is what makes this RS. Also read at `M:src/JSystem/JAudio/JAI/JAISeMgr.cpp:640`,
  `JAISequenceMgr.cpp:816`, `JAIStreamMgr.cpp:83`, `M:src/plugProjectHikinoU/PSSeq.cpp:463,576`.

### 5.1 Two-way sync with the game's Options screen

Four values exist both in mppm's config and in the game's save options (`Data.h:175-178`):
`mSoundMode`, `mMusicVol`, `mSeVol`, `mIsRumble`. The rule: **config.json wins; the card follows.**

- **Config to game.** New `extern "C" void p2_host_apply_game_options(void)` (launcher side,
  declared in `p2_host.h`). Called through `PC_ONLY(...)` at the end of `Mgr::setup()`
  (`commonSaveData.cpp:115-146`), which is where the game applies options it just read from the
  card. It calls back into a small game-side function, `p2_game_set_options(int soundMode, float bgm,
  float se, int rumble)`, that uses the `Mgr` setters above and writes `mIsRumble` plus
  `rumbleMgr->mIsRumbleActive`. The same function runs when the user changes any of the four vars.
- **Game to config.** `PC_ONLY(p2_host_game_options_changed(mSoundMode, mMusicVol, mSeVol,
  mIsRumble);)` at the end of `TOptionParameter::saveRam()`
  (`M:src/plugProjectEbisawaU/ebiScreenOption.cpp:74-`), which is where the Options screen commits.
  The launcher writes the four vars (`round(v * 100 / 255)`), guarded against re-entry.
- No `mDoSaveOptions` handling is needed: whatever the card stores is overwritten at the next boot.
- A locked var (environment or netplay overlay) is not written back; the game's Options screen
  change is then reverted by the next `p2_host_apply_game_options`.

---

## 6. GAMEPLAY

Deliberately short: these change how the game behaves without making it easier.

| Group | Label | Key | Type | Default | Choices | Applies | Net | Phase |
|---|---|---|---|---|---|---|---|---|
| General | Pause While Menu Is Open | `game.pauseInMenu` | bool | `true` | | RT | X (off) | v1.1 |
| General | Pause in Background | `game.pauseInBackground` | bool | `false` | | RT | X (off) | v1 |
| Startup | Skip Boot Logos | `game.skipBootLogos` | bool | `true` | | NL | L | v1.1 |
| Title Screen | Demo Movies When Idle | `game.idleDemo` | bool | `true` | | RT | L | v1.1 |
| Title Screen | Season | `game.titleSeason` | enum | `original` | `original`, `spring`, `summer`, `autumn`, `winter` | RT (next visit) | L | v1.1 |
| Experimental | Fast Forward (hold key) | `game.fastForward` | bool | `false` | | RT | X (off) | later |

**Pause While Menu Is Open**
- Help: "Freeze the game while the mppm menu is open, so nothing happens to your Pikmin while you
  change settings."
- Hook (new, host side): while the menu bar or a window it opened is visible and this is on,
  `p2_host_present` (`main.cpp:90-116`) does not return to the game. It loops
  `aurora_end_frame` / `beginHostFrame` / `launcher::update` itself. Master gain ramps to 0 while
  held.
- Risk to verify first: a frame with no GX draws must still show the last game frame under the UI.
  If Aurora clears instead, keep input-only blocking (today's behaviour through `PADBlockInput`,
  `pad.h:223`) and ship this option disabled with the note "Not available yet".
- The start menu never uses this path: the live scene must keep animating behind it.

**Pause in Background**
- Help: "Pause the game when you switch to another window."
- Hook: startup `AuroraConfig.pauseOnFocusLost` (`aurora.h:93`); runtime
  `aurora_set_pause_on_focus_lost` (`aurora.h:136`, `A:lib/window.cpp:499-504`).

**Skip Boot Logos**
- Help: "Go straight to the title screen instead of showing the Nintendo and Dolby logos each time.
  Only matters when the start menu is skipped."
- Hook: `PC_ONLY` in `BootSection` state handling (`M:src/sysGCU/bootSection.cpp:785-925`): jump
  from `SID_InitNintendoLogo` to `SID_EndState`. `SID_LoadResourceFirst` and `SID_LoadMemoryCard`
  (`:786-790`) must still run. Launching through the start menu always skips them, whatever the
  setting says. This is the same code as SCENE.md hook H3: one condition,
  `gP2Config.skipBootLogos || p2_launcher_scene_pending()`.

**Demo Movies When Idle**
- Help: "After a while on the title screen, the game plays its demo movies. Turn this off to stay
  on the title screen."
- Hook: at `M:src/sysGCU/titleSection.cpp:436`, extend the condition with
  `PC_ONLY(&& gP2Config.idleDemo)`; when off, fall into the existing `mGoToDemoTimer = 0.0f` branch
  (`:446`). Separately and unconditionally: `PC_ONLY(if (gP2Config.launcherOpen) mGoToDemoTimer =
  0.0f;)` after `:351`, fed by `launcher::prelaunch_open()` (`launcher.cpp:129`). Not part of v1.1: with the boot flow of SCENE.md 4.3 the title section never runs behind the start menu. Add it only if an in-process "Start Menu" return (section 13) is built.

**Season**
- Choices: "As Original", "Spring", "Summer", "Autumn", "Winter".
- Help: "The title screen changes with the seasons. Pick a favourite, or leave it to the game."
- Hook: `PC_ONLY(if (gP2Config.titleSeason >= 0) sSeasonIndex = gP2Config.titleSeason;)` before
  `mMainTitleMgr.setMode(sSeasonIndex)` at `titleSection.cpp:773` (original logic at `:718-768`).

Backlog, not specified here: fast pluck (`M:src/plugProjectKandoU/naviState.cpp:1655`), autosave
control (`M:src/plugProjectOgawaU/ogObjSave.cpp:65`), HUD scale.

---

## 7. CHEATS

Rules for the whole tab:

- A red notice at the top: "Cheats change how the game plays. Progress you make with cheats on is
  saved like any other progress."
- While any `cheat.*` var differs from its default, the overlay shows a small "Cheats on" badge
  next to the FPS counter position, and the start menu's status line says so.
- Online: all **S**. In ranked or public lobbies the netplay overlay forces defaults; private
  lobbies may later let the host choose.
- No cheat writes anything to the card that the game would not write itself.

| Group | Label | Key | Type | Default | Range | Applies | Net | Phase |
|---|---|---|---|---|---|---|---|---|
| Modes | Unlock Challenge Mode | `cheat.unlockChallenge` | bool | `false` | | RT (title menu) | S | v1 |
| Time | Day Speed | `cheat.daySpeed` | int | `100` | UI: 0, 25-400 step 25 (%); file: 0-10000 | RT | S | v1 |
| Time | Allow Sunset on Day 1 | `cheat.sunsetOnDayOne` | bool | `false` | | RT | S | v1 |
| Captains | Invincible Captains | `cheat.invincibleCaptains` | bool | `false` | | RT | S | v1.1 |
| Captains | Unlimited Sprays | `cheat.infiniteSprays` | bool | `false` | | RT | S | v1.1 |
| Experimental | More Than 100 Pikmin | `cheat.pikminLimit` | int | `100` | 100-200 | next area | S | later |

**Unlock Challenge Mode**
- Help: "Offer Challenge Mode on the title menu right away, without finding the key in story mode.
  Your save is not changed."
- Hook: `gamePlayCommonData.cpp:188`, `PC_ONLY(if (gP2Config.unlockChallenge) return true;)`.

**Day Speed**
- Control: stepper. 100% reads "Normal"; 0% reads "Sun stopped".
- Help: "How fast the day passes. Below 100% gives you longer days; above makes them shorter. At
  0% the sun never sets, so end the day yourself from the pause menu."
- Hook: `timeMgr.cpp:167`, `PC_ONLY(* gP2Config.daySpeed)`. The file range goes to 10000 so test
  scripts can still use `MPPM_TIME_SCALE=50`.

**Allow Sunset on Day 1**
- Help: "Let you choose Go to Sunset from the pause menu on the first day, which the game normally
  forbids. Skipping the first day's tutorial steps can leave the story in an odd state."
- Hook: `singleGameSection.cpp:1048`, `PC_ONLY(&& !gP2Config.sunsetOnDayOne)`.

**Invincible Captains**
- Help: "Olimar, Louie and the President take no damage from enemies and hazards. Falling into a
  pit still counts."
- Hook: early return in `Navi::startDamage` (`M:src/plugProjectKandoU/navi.cpp:1671-1675`) and in
  `Navi::addDamage` (`:1704-1712`, next to the existing `invincible()` test). Do not change
  `Navi::invincible()` itself (`:1641`): it also gates non-damage behaviour.

**Unlimited Sprays**
- Help: "Using a spray does not use it up. You still need to have found at least one of that
  spray."
- Hook: skip the decrement in `PlayData::useDope` (`M:src/plugProjectKandoU/gamePlayData.cpp:1139-1144`).
  `hasDope` (`:1100-1104`) stays as it is, so the count on the card is never inflated.

Left out on purpose: adding Pokos and granting Exploration Kit items (both rewrite story progress
flags), debug shortcuts in `gGameConfig` (`M:include/Game/GameConfig.h:66-70`).

---

## 8. Controller and keyboard configuration window

A separate window (`ControllerConfigWindow`), opened from INPUT and from the in-game menu. Port of
`D:src/dusk/ui/controller_config.cpp` without the Twilight Princess action list
(`D:src/dusk/settings.h:353-360`).

**Layout:** one tab per port, "Player 1" to "Player 4". Tabs for ports 3 and 4 are shown but carry
the note "Pikmin 2 uses two controllers."

**Device row** (dropdown): "None", "Keyboard", then each connected controller by name.
- List: `PADCount()`, `PADGetNameForControllerIndex(i)` (`pad.h:180-182`).
- Assign: `PADSetPortForIndex(i, port)` (`pad.h:183`), `PADClearPort(port)` (`pad.h:186`),
  `PADSetKeyboardActive(port, TRUE/FALSE)` (`pad.h:203`).
- Current: `PADGetIndexForPort(port)` (`pad.h:184`), `PADGetName(port)` (`pad.h:187`).
- Hot-plug: rebuild on `AURORA_CONTROLLER_ADDED / _REMOVED` (`event.h:23-24`).
- Extras shown when available: battery (`PADGetBatteryState`, `pad.h:290`), "GameCube adapter"
  tag (`PADIsGCAdapter`, `pad.h:212`).

**Pages** (left list; right pane shows a controller diagram and the game action):

| Page | Rows | API |
|---|---|---|
| Buttons | A, B, X, Y, Start, Z, L, R, D-pad Up/Down/Left/Right (`PAD_BUTTON_COUNT` = 12, `pad.h:62`) | `PADGetButtonMappings` / `PADSetButtonMapping` (`pad.h:188-190`) |
| Sticks | Control Stick and C-Stick: Up, Down, Left, Right (axes 0-7, `pad.h:65-72`) | `PADGetAxisMappings` / `PADSetAxisMapping` (`pad.h:191-193`) |
| Triggers | L and R analog (axes 8-9, `pad.h:73-74`), press thresholds | same, plus `PADGetDeadZones` (`pad.h:205`) |
| Dead Zones | Control Stick, C-Stick: 0-100%; "Use dead zones" toggle | `PADGetDeadZones(port)` returns a mutable struct (`pad.h:154-161,205`) |
| Rumble | Strength low/high 0-100%, "Test", "Use controller's own rumble" | `PADSetRumbleIntensity`, `PADSupportsRumbleIntensity`, `PADSetForceDeviceRumble` (`pad.h:274-279`) |

Each button row shows the GameCube button, the bound physical input (`PADGetNativeButtonName`,
`PADGetNativeAxisName`, `pad.h:207,210`) and what it does in Pikmin 2:

| GameCube input | In-game action (shown as a hint) |
|---|---|
| Control Stick | Move |
| C-Stick | Direct your Pikmin group |
| A | Throw / pluck / punch |
| B | Whistle |
| X | Dismiss |
| Y | Switch captain |
| Start | Pause menu |
| Z | Camera angle |
| L | Camera behind captain |
| R | Zoom |
| D-pad Up / Down | Ultra-spicy / ultra-bitter spray |
| D-pad Left / Right | Choose which Pikmin to throw |

(Check the hint column against the game manual before shipping.)

**Press-to-bind flow**
1. Confirm on a row: the row reads "Press a button..." and a 5-second ring counts down.
2. While capturing, UI navigation is suspended. Each `ui::update`:
   - Controller: poll `PADGetNativeButtonPressed(port)` (`pad.h:219`; confirm its "nothing pressed" value in `A:lib/dolphin/pad/pad.cpp`) or
     `PADGetNativeAxisPulled(port)` (`pad.h:221`) depending on the row type.
   - Keyboard device: take the next `SDL_EVENT_KEY_DOWN` scancode in `launcher::handle_event`
     before RmlUi sees it.
3. Escape (keyboard) or the 5-second timeout cancels. Delete / Backspace clears the binding.
4. Write: `PADSetButtonMapping`, `PADSetAxisMapping`, `PADSetKeyButtonBinding` (`pad.h:196`) or
   `PADSetKeyAxisBinding` (`pad.h:199`). Then `PADSerializeMappings()` (`pad.h:194`) and
   `kSoundBindingChanged` (`sound.hpp:12`).
5. If the input was already bound on the same port, swap the two bindings and say so in a toast:
   "Swapped with <other action>."

**Keyboard**
- Bindings per port: `PADGetKeyButtonBindings` / `PADGetKeyAxisBindings` (`pad.h:198,201`).
- The default layout moves out of `main.cpp:60-87` into `M:port/launcher/input_defaults.cpp` as
  `launcher::apply_default_keyboard(port, force)`. `main.cpp:185` calls it with `force = false`
  (keep saved bindings); "Restore Defaults" calls it with `force = true` after
  `PADClearKeyBindings(port)` (`pad.h:202`).
- Defaults (unchanged): WASD stick, IJKL C-stick, Space A, Left Shift B, E X, Q Y, Enter Start,
  Tab Z, F L, R R, arrows D-pad, 1/2 analog L/R.
- Reserved keys that can never be bound: F1 (menu), F11 (fullscreen), Escape (cancel).

**Footer buttons:** "Restore Defaults" (`PADRestoreDefaultMapping(port)`, `pad.h:222`, or the
keyboard path above; asks for confirmation), "Done".

**Storage:** Aurora's own files in the data folder (`*.controller`, `keyboard_bindings.dat`,
`controller_ports.dat`), written by `PADSerializeMappings`. Nothing goes in `config.json`.

**Netplay:** all **L**. Bindings map physical inputs to GameCube inputs; only the resulting
`PADStatus` enters the simulation.

---

## 9. INTERFACE and TOOLS

### 9.1 INTERFACE

| Group | Label | Key | Type | Default | Range | Applies | Net | Phase |
|---|---|---|---|---|---|---|---|---|
| mppm | Menu Size | `interface.uiScale` | int | `100` | UI: Automatic (0), then 75-130 step 5 (%), as DESIGN.md 9.3; stored 135-200 reads as 133 | RT | L | v1 |
| mppm | Reduce Motion | `interface.reduceMotion` | bool | `false` | | RT | L | v1 |
| mppm | High Contrast | `interface.highContrast` | bool | `false` | | RT | L | v1 |
| mppm | Glass Effects | `interface.glassEffects` | enum | `blur` | `off`, `blur`, `full` | RT | L | v1 |
| mppm | Skip Start Menu | `backend.skipLauncher` | bool | `false` | | NL | L | v1 |
| mppm | Live Start Menu | `interface.liveBackground` | bool | `true` | | NL | L | v1 |
| Start Menu | Location | `interface.sceneArea` | enum | `forest` | `forest`, `tutorial`, `yakushima`, `last`, `season` | NL | L | v1 (`forest` only until SCENE.md phase 6) |
| Start Menu | Time of Day | `interface.sceneTime` | enum | `morning` | `morning`, `day`, `evening`, `night`, `clock` | NL | L | v1 |
| Start Menu | Music | `interface.sceneMusic` | enum | `piklopedia` | `piklopedia`, `worldmap`, `challenge`, `hiscore`, `off` | NL | L | v1 |
| Start Menu | Take-off | `interface.launchAnimation` | enum | `full` | `full` (On), `off`; stored `auto` and `short` read as `full` | RT | L | v1 |
| mppm | Check for Updates | `interface.checkForUpdates` | bool | `true` (asked on first run) | | RT | L | v1 |
| mppm | Check Now | (button) | | | | | | v1 |
| Notifications | Controller Messages | `interface.controllerToasts` | bool | `true` | | RT | L | v1 (exists) |
| Notifications | Menu Reminder | `interface.menuHint` | bool | `true` | | RT | L | v1 |
| Notifications | Shader Activity | `interface.shaderIndicator` | bool | `true` | | RT | L | v1 |
| Game | Hide HUD | `interface.hideHud` | bool | `false` | | RT | L | v1.1 |
| Game | HUD Layout | `interface.hudLayout` | enum | `centered`; becomes `edges` with ASPECT.md phase 2 | `centered`, `edges`, `stretched` | RT | L | with ASPECT.md phase 1 |
| Game | HUD Width Limit | `interface.hudMaxAspect` | enum | `16:9` | `16:9`, `21:9`, `unlimited` | RT | L | with ASPECT.md phase 2 |
| Advanced | Advanced Settings | `interface.advancedSettings` | bool | `false` | | RT | L | v1 |
| (hidden) | | `interface.presetChosen` | bool | `false` | | | L | v1 |
| (hidden) | | `interface.lastSeenVersion` | string | `""` | | | L | v1 |

On the start menu Live Start Menu, Location, Time of Day and Music are live (`MenuApply::Live`,
section 1.2): the running scene follows them with the menu and Settings left open, and their rows
carry no "Next launch" badge there. Skip Start Menu and Skip Boot Logos show no badge on the menu
("Used each time Moonsprout starts."). In game the NL badges stay as they are.

**Menu Size**
- Help: "Size of mppm's own menus. This does not change the game's HUD."
- Hook: `ui::apply_scale()` (`M:port/launcher/ui/ui.cpp:601-610`) to `aurora::rmlui::set_ui_scale`
  (`A:include/aurora/rmlui.hpp:30`). Already wired; add `on_change`.

**Reduce Motion**
- Help: "Replace sliding and bouncing menu animations with quick fades."
- Hook: toggles a `reduce-motion` class on every document body; the theme sheet collapses
  transitions to an 80 ms opacity fade. The take-off keeps its full timeline with still cameras, plain UI fades and a navy flash (DESIGN.md 7.3).

**Skip Start Menu**
- Help: "Start the game as soon as mppm opens. You can still reach every setting with F1 in game."
- Hook: `launcher.cpp:96`. It lives here, not in PRELAUNCH, so it can be turned back off in game.

**Live Start Menu**
- Help: "Show Olimar and his Pikmin behind the start menu. Turn this off for a still backdrop,
  which starts faster on slow computers."
- Hook: read by the boot flow when deciding whether to boot the game behind the menu
  (`launcher::wants_prelaunch` / `show_prelaunch`, `launcher.cpp:92-106`). Off = the pre-disc
  backdrop is used and the game boots on Play.

**High Contrast**, **Glass Effects**: behaviour in DESIGN.md 9.1 and 5.16 / 8.2. Help: "Solid
panels and stronger outlines for easier reading." / "How see-through mppm's panels are. Off is
fastest."

**Location**, **Time of Day**, **Music**, **Take-off** (group "Start Menu", disabled while Live
Start Menu is off): meaning in SCENE.md section 12 and DESIGN.md 7.3. `port/launcher/scene_bridge.cpp`
turns them into `P2SceneConfig` (SCENE.md 4.2) when the scene section is created; it resolves
`season` and `clock` to a concrete area and time on the host side.

Take-off is a choice of On and Off. Help: "The take-off when you press Play. Press any button to
skip it. Reduce Motion keeps the cameras still." `VarOptions::sanitize` maps the removed `auto` and
`short` to `full`, and the enum table keeps all four names so old files parse. The hidden
`interface.fullLaunchSeen` is gone; a leftover key in an old `config.json` is kept as an unknown
key and ignored. Reduce Motion no longer changes `effective_launch_animation()`.

**HUD Layout**, **HUD Width Limit**: ASPECT.md sections 4 and 7.

**Check for Updates** and **Check Now**
- Help: "Look for a new mppm release when the start menu opens. mppm only tells you about it; it
  never downloads or installs anything. The check contacts GitHub and sends no personal
  information."
- Hook: `borealis::update::check_latest_github_release` (`B:include/borealis/update.hpp:74`,
  linked at `M:CMakeLists.txt:174-175`) against `owallzz/moonsprout`. Once per process, from the start
  menu. Result: a line in the start menu's version corner and one toast; "View Release" opens the
  release page with `SDL_OpenURL`.
- "Check Now" shows one of: "You're up to date (vX).", "mppm vY is available.", "Couldn't reach
  GitHub. Try again later."

**Controller Messages**: "Tell you when a controller connects or disconnects."
(`ui.cpp:182,216`.)

**Menu Reminder**: "Show 'Press F1 for the menu' for a moment when the game starts."

**Shader Activity**
- Help: "Show a small spinner while the graphics card prepares new effects. Short stutters while it
  is visible are normal the first time you see something."
- Hook: `aurora_get_stats()->queuedPipelines > 0` (`gfx.h:19-31`), with a 300 ms minimum display.

**Hide HUD**
- Help: "Hide the game's on-screen display: Pikmin counts, the sun meter and health. Handy for
  screenshots."
- Hook: `PC_ONLY(if (!gP2Config.hideHud))` around `Screen::gGame2DMgr->draw(gfx)`
  (`M:src/plugProjectKandoU/baseGameSection.cpp:1465`) and `drawIndirect` (`:1497`). Pause and
  result screens must stay visible; verify before shipping.

**Advanced Settings**
- Help: "Show extra options meant for troubleshooting and testing, including experimental
  features that may not work properly."
- Effect: reveals Adv and Exp rows and the Developer group in TOOLS. Turning it off hides them but
  keeps their values, and a line at the bottom of each affected tab says "N advanced options are
  changed" with a "Show" link.

### 9.2 TOOLS

| Group | Label | Key | Type | Default | Applies | Net | Phase |
|---|---|---|---|---|---|---|---|
| Saves | Save Manager | (button; launcher only) | | | | | v1 |
| Maintenance | Data Folder | (button; same row as PRELAUNCH's) | | | | | v1 |
| Maintenance | Reset All Settings | (button) | | | | | v1 |
| Maintenance | Reset All Controls | (button) | | | | | v1 |
| Maintenance | Choose Preset | (button, section 12) | | | | | v1 |
| Developer (Adv) | Performance Overlay | `tools.perfOverlay` | bool | `false` | RT | L | v1 |
| Developer (Adv) | Log Detail | `tools.logLevel` | enum `debug` / `info` / `warning` / `error` | `info` | RT | L | v1 |
| Developer (Adv) | Show Crash Screen | `tools.panicScreen` | bool | `false` | RT | L | v1 |
| Developer (Adv) | Log Frame Rate | `tools.logFps` | bool | `false` | RT | L | v1 |
| Developer (Adv) | Scheduler Log | `tools.schedDebug` | bool | `false` | RT | L | v1 |
| Developer (Adv) | Audio Log | `tools.audioDebug` | bool | `false` | RT | L | v1 |
| Developer (Adv) | Audio Voice Log | `tools.audioVoiceLog` | bool | `false` | RT | L | v1 |
| (no UI) | | `tools.audioDumpPath` | string, `persist = false` | `""` | RS | L | v1 |

- **Save Manager.** In game the button is disabled with: "Return to the start menu to manage
  saves."
- **Reset All Settings.** Confirmation: "Reset every mppm setting to its default? Your saves,
  disc and controller layouts are kept." Calls `reset()` on every var except `backend.discPath`,
  `backend.discVerification`, `backend.discFingerprint`, `interface.presetChosen`.
- **Reset All Controls.** Confirmation, then `PADRestoreDefaultMapping` for each port,
  `apply_default_keyboard(0, true)`, `PADSerializeMappings()`.
- **Performance Overlay.** "Show draw calls, shader counts and frame timing." Reads
  `aurora_get_stats()` (`gfx.h:31`).
- **Log Detail.** "How much mppm writes to its log." `aurora_set_log_level` (`aurora.h:135`);
  startup `AuroraConfig.logLevel` (`aurora.h:104`).
- **Show Crash Screen.** "When the game hits a fatal error, show its original error screen instead
  of closing." `system.cpp:129` and `JUTException.cpp:166` both become
  `if (!gP2Config.panicScreen) abort();`.
- **Log Frame Rate.** "Print the frame rate to the terminal once a second." Replaces the static at
  `main.cpp:93` with a per-frame read of the var (the launcher exposes
  `bool launcher::log_fps()`).
- **Scheduler Log / Audio Log / Audio Voice Log.** One sentence each on what is printed to the
  terminal (thread states every two seconds; sound system activity; every voice start/stop). Atomics with setters in
  `os_thread.cpp:282` and `audio.cpp:153,389,589`; the function-local statics there go away.

---

## 10. Save manager

Window `SavesWindow`, a port of `D:src/dusk/ui/saves_window.cpp:868-960`. Launcher only.

**What it shows:** the Pikmin 2 card files found for `GPVE` / `01` in the active save format, each
with file name, size and modified time; then a "Backups" list.

**Where the files are:** `aurora_card_get_path("GPVE", type, 0, buf, cap)`
(`A:include/aurora/card.h:35`); active type from `aurora_card_get_type(0)` (`card.h:22`) once the
game has booted, else from `backend.cardFileType`.

| Action | Behaviour | API |
|---|---|---|
| Import | Pick a `.gci` file, or a Dolphin `.raw` card image (its Pikmin 2 files are listed to choose from). An existing save is replaced only after confirmation | GCI folder: copy the file in under `aurora_card_gci_filename` (`card.h:55`). Card image: `aurora_card_raw_insert` (`card.h:49`). From an image: `aurora_card_raw_list` / `aurora_card_raw_extract` (`card.h:41-46`) |
| Export | Write the selected save as `.gci` to a chosen folder | copy, or `aurora_card_raw_extract` |
| Back Up Now | Copy all Pikmin 2 saves to `<data>/backups/<YYYY-MM-DD_HHMMSS>/` | filesystem |
| Restore | Replace current saves with a backup, after confirmation | filesystem / `aurora_card_raw_insert(replace = true)` |
| Delete | Remove a save or a backup, after confirmation | filesystem / `aurora_card_raw_delete` (`card.h:52`) |
| Open Save Folder | Show the folder in the file manager | `SDL_OpenURL` |

Rules:
- An automatic backup is taken before every Import, Restore and Delete. The newest 10 automatic
  backups are kept; manual backups are never pruned.
- If the game is already booted behind the start menu, it has read the card during boot
  (`bootSection.cpp:789`). After any change, call `aurora_card_remount(0)` (`card.h:38`) **and**
  mark a restart as pending (section 11) so Play starts from a clean boot.
- Never available while playing. No per-slot editing in v1 (Pikmin 2 keeps its three story slots
  inside one card file).
- Copy for the destructive confirmations:
  - Delete: "Delete this save? A backup is kept in the Backups list."
  - Restore: "Replace your current saves with the backup from <date>? Your current saves are backed
    up first."
  - Import over existing: "A Pikmin 2 save already exists. Replace it with the imported one? The
    current save is backed up first."

---

## 11. Restart-required handling

**Keys that need a restart (Apply::Restart):** `backend.discPath` (once a game is booted),
`backend.graphicsBackend`, `backend.cardFileType`, `video.msaa`, `video.allowCpuAdapter`,
`audio.enabled`, `audio.outputDevice`, `tools.audioDumpPath`. Plus two non-setting sources: saves
changed by the Save Manager while a game is booted (section 10), and a mod list changed in the Mods
window that no longer matches the mods the boot mounted (section 17, `ui::mods_restart_pending()`).
On the start menu these follow their
`MenuApply` class (1.2) instead; what is described below is the in-game behaviour.

**Detection.** At the end of `preinit`, store `to_json()` (`config.hpp:240`) of every
`Apply::Restart` var as the boot snapshot. `settings_restart_pending()` returns the list of vars
whose current value differs. Changing a value back clears it. Model:
`D:src/dusk/ui/prelaunch.cpp:803-823`.

**What the user sees.**
- The row gets a "Restart required" badge; its help pane adds "Takes effect after mppm restarts."
- The settings window shows a footer banner while anything is pending:
  "Some changes take effect after a restart." with **Restart Now** and **Later**.
- **In the start menu** (live_options_design 3.5, 3.8, 3.9) there is no banner, no "Restart
  Needed" modal and no "Restart required" or "Next launch" badge. `live::update()`
  (port/launcher/live_apply.cpp) runs every frame while the menu is open: it logs each change as
  `mppm: live: <key> -> live|relaunch|nothing`, re-bases every pending var whose menu action is
  not Relaunch (and a backend that resolves to the running one), leaves a disc that does not
  inspect as Ready alone, and schedules one relaunch for whatever is left. 350 ms after the change
  (cancelled if it is changed back), once the menu is idle (top document the start menu or
  Settings, no launch phase, no file dialog), a navy veil closes over the window in 150 ms and the
  music is ducked; the process then re-executes with `MPPM_SEAMLESS_RESTART=1` and
  `MPPM_RESUME=tab=<title>;key=<settings key>;scroll=<px>;x=<px>;y=<px>;max=<0|1>;t=<ms>`. The new
  process restores the window position (X11 and Windows), reopens Settings on the same tab and row
  with no entrance and no sound, fades the veil out from its third frame over 200 ms and logs
  `mppm: relaunch: resumed in N ms`. A relaunch row shows "Applying…" from the change to the exec
  (Live Start Menu and Location while the scene is busy), and its help pane says "Moonsprout
  restarts for a moment to apply this. You come straight back here." None rows say "Used each
  time Moonsprout starts." Pressing Play counts only a relaunch still waiting (and the Save
  Manager's and the Mods window's restart) as pending.
- **In game:** Restart Now asks once: "Restart mppm?" / "Progress since your last save will be
  lost." with "Restart" / "Not now".
- `Apply::NextLaunch` keys only get a quiet "Next launch" badge. No banner.

**Restart mechanics.** `launcher::request_restart(RestartTarget target)` with `StartMenu`, `Play`
or `SameAsNow`. It flushes config (`Registry::flush`, `config.hpp:429`), runs the same teardown as
`AURORA_EXIT` (`main.cpp:32-42`), then re-executes the binary (`execv` on Linux and macOS,
`CreateProcessW` then exit on Windows; model: `D:src/dusk/main.cpp:35-113`). Arguments: the
original argv, with `--launcher` or `--play` replacing any earlier one. A disc given on the command
line stays on the command line, unless a different disc that inspects as Ready is pending: then
the positional disc is dropped and `MPPM_DISC` unset. A `--cvar` whose var a choice in Settings has
since released from the command-line layer is dropped too, so the next process does not revert it.

---

## 12. First-run preset

Shown once, when `interface.presetChosen` is false, on top of the start menu and before anything
else can be focused (model: `D:src/dusk/ui/preset.cpp:62-118`). Skipped entirely when the start
menu is skipped (headless runs), in which case defaults (= Original for video) apply.

Heading: "Welcome to mppm" / "How would you like Pikmin 2 to look? You can change everything later
in Settings."

| | **Original** | **Enhanced** (focused by default, marked "Recommended") |
|---|---|---|
| Blurb | "The way it looked on a GameCube." | "Sharper and cleaner on modern screens. Plays exactly the same." |
| `video.internalResolution` | `1` | `0` (Auto) |
| `video.resampler` | `bilinear` | `area` |
| `video.anisotropy` | `1` | `8` |
| `video.aspectRatio` | `original` | `original` (`auto` once ASPECT.md phase 1 ships) |
| `video.vsync` | `true` | `true` |
| `video.msaa` | `1` | `1` |

Below the cards, one toggle bound to `interface.checkForUpdates`:
"Tell me when a new version of mppm is available. (Contacts GitHub when mppm starts.)"

Mechanics:
- A preset is a list of `(var, value)` pairs applied with `set()`. It touches only the keys in the
  table, never experimental ones, never gameplay or cheats.
- Texture Filtering applies at runtime and MSAA is live on the start menu, so the first-run preset
  never restarts by itself any more: anything else restart-only it left different is relaunched by
  the start-menu applier (section 11).
- TOOLS > "Choose Preset" reopens the same screen later; then the normal restart banner applies.
- The presets differ only in video. If section 6 or 5 later gains options with an "original vs
  modern" flavour, add them here.

---

## 13. In-game menu

Opened and closed with F1, the controller Back button, or R + Start (`input.cpp:16,223-353`;
all arrive as `KI_F1`). Escape or B closes the topmost layer. Not available while the start menu is
up (`Prelaunch::obscures_game()`, `M:port/launcher/prelaunch.hpp:22`).

A top bar (port of `D:src/dusk/ui/menu_bar.cpp:73-170`) with these entries, left to right:

| Entry | Action | State |
|---|---|---|
| **Resume** | Close the menu | v1 |
| **Settings** | `SettingsWindow(prelaunch = false)`: VIDEO, INPUT, AUDIO, GAMEPLAY, CHEATS, INTERFACE, TOOLS | v1 |
| **Controllers** | `ControllerConfigWindow` directly (the most common mid-game need) | v1 |
| **Mods** | The Mods window, read-only (section 17). Shown over a running game only | v1 (M5.2) |
| **Online** | The Online window: members, ping, input delay, link, Leave game (section 18). Shown while a lobby or a match exists (`mppm::online::active()`) | v1 (M5.3b) |
| **Reset Game** | Back to the title screen. Confirmation: "Return to the title screen? Progress since your last save will be lost." | v1.1 |
| **Start Menu** | Back to the mppm start menu. Same confirmation text, "Return to the start menu?" | v1 |
| **Quit** | Close mppm. Confirmation: "Quit mppm? Progress since your last save will be lost." | v1 |

- **Tab registry.** The bar is built from a list of `{id, label, order, visible(), open()}` so Mods
  and Online register themselves later without touching the bar. Orders: Resume 0, Settings 10,
  Controllers 20, Mods 30, Online 40, Reset Game 80, Start Menu 90, Quit 100.
- **Input.** While any menu document is visible, `PADBlockInput(true)` (`pad.h:223`) keeps input
  from the game; on close, held buttons stay suppressed until released.
- **Pausing.** Follows `game.pauseInMenu` (section 6). Online: never pauses; the bar shows
  "The game is still running" under the title (the tag that reads "Paused" offline).
- **Online.** Reset Game is hidden during an online match (`mppm::online::in_game()`): every machine
  would have to reset together. Start Menu and Quit leave the match like Leave game.
- **Reset Game.** Uses the game's own soft-reset path: set
  `JUTGamePad::C3ButtonReset::sResetOccurred = true` through a small `PC_ONLY` export (handled in
  `M:src/sysGCU/reset.cpp:104-126`). Ship it only after it survives 20 resets in a row from
  gameplay, a cave, and a cutscene; until then the entry is hidden.
- **Start Menu.** v1 = `request_restart(RestartTarget::StartMenu)` (process re-exec, section 11):
  always correct, about as fast as a boot. A no-restart version through the soft reset can replace
  it once Reset Game is proven.
- **Quit.** Posts the same shutdown as `AURORA_EXIT` (`main.cpp:32-42`).
- **Confirmations are skipped** when the game is sitting on the title screen or file select
  (nothing to lose). `GameFlow::mActiveSectionFlag` cannot tell (it names the *next* section and
  reads `SN_MainTitle` during gameplay), so `p2_game_progress_at_risk()` (`M:port/game/menu_query.cpp`)
  looks at the RootMenuSection's current child instead: `Title::Section`, or `SingleGameSection`
  in its `SGS_File` state, means nothing to lose. No game file is edited.
- **First time in a session** the menu reminder toast appears once (`interface.menuHint`).

---

## 14. Netplay readiness

Online PvP will run Pikmin 2's two-player modes with both machines simulating. Classes:

- **L, local.** Presentation and input mapping. Free to differ per player. All of `video.*` except
  `video.smoothMotion`; all of `audio.*`, `input.*`, `interface.*`, `tools.*`, `backend.*`.
  `video.aspectRatio` is local since M5.1b: the cull planes that decide LOD (simulation) use a
  session-constant widescreen factor while `p2_net_active()` (`p2_net_sim_k`, default 3.6, stored in
  the replay header), whatever the window's shape. Offline they still follow the window.
- **S, synced.** Part of the simulation. The host's values are sent in the lobby handshake and
  forced on every peer: all of `cheat.*` (six keys, `cheat.pikminLimit` included).
- **X, forced.** Fixed while online: `game.pauseInMenu = false`, `game.pauseInBackground = false`,
  `game.fastForward = false`, `video.smoothMotion = false`. Also not settings but forced: Aurora
  time scale 1.0 (`aurora.h:140`) and the retrace period (`os_thread.cpp:264`).

Enforcement, with no netplay knowledge in settings code (`port/launcher/settings.hpp`, M5.1b):

```cpp
borealis::config::Overlay& net_overlay();  // {"netplay", {.priority = 1000, .releaseOnUserSet = false}}
bool net_lock_settings(const nlohmann::json& hostSynced); // Forced -> Meta::netValue; Synced -> hostSynced[key], else its default
void net_unlock_settings();                                // overlay.clear()
bool net_settings_locked();
nlohmann::json net_synced_settings();                      // {key: effective to_json()} of every Synced var
uint32_t net_synced_hash(const nlohmann::json& synced);   // FNV-1a 32 over "key=<value.dump()>\n", keys sorted
```

- `net_lock_settings` checks every value first (a host value becomes text the way
  `VarBase::to_string` writes it: a string raw, anything else as JSON, then the var's `--cvar`
  parser). A value that does not parse, or a key that is not a Synced var, prints the reason, locks
  nothing and returns false.
- The overlay outranks `--cvar`, the environment and `config.json`, and locks its vars, so
  `var.get()`, the mirrors into `gP2Config` and the settings window all follow with no further code.
  Nothing of it is saved.
- **Harness runs** (`--replay`, `--record`, `--checksum-log`, `--checksum-verify` or
  `--vs-stage`, the flags that make `p2_net_active()` true; `--headless-ticks` alone does not) lock with an empty object, so every Synced var takes its default and every
  Forced var its net value: `launcher::preinit`, after the environment layer and before
  `bind_game_config`, prints `mppm: net: settings locked (4 forced, 6 synced)` and reports
  `net_synced_hash(net_synced_settings())` to the seam (`p2_net_set_synced_hash`, the replay
  header). A cheat given by `--cvar`, `MPPM_TIME_SCALE` or `config.json` therefore cannot change a
  replay's checksums. Without the harness nothing is locked and those layers apply as before.
- **Online matches** (M5.3b, section 18). The host sends `net_synced_settings()` in the session's
  `SessionConfig`; at START every peer (the host too) locks with those values through the launcher
  hook `p2_launcher_net_lock(json)` (the JSON parsed without exceptions, then `net_lock_settings`;
  0 = refused, and the peer leaves with "settings differ"). The session ends with
  `p2_launcher_net_unlock()`: `net_unlock_settings()`, then, when harness flags still make
  `p2_net_active()` true, the harness lock again with an empty object. The host reads its own
  values with `p2_launcher_net_synced_json()`.
- Locked rows are disabled. Their help pane says "Locked while a replay or harness session runs.",
  or, during an online match (`mppm::online::in_game()`), "Set by the host of this online game."
- The handshake payload is exactly `net_synced_settings()` (the `Net::Synced` keys and their JSON
  values), plus `net_synced_hash` so mismatched builds are rejected with a clear message.
- `audio.muteInBackground` stays local, but `input.backgroundInput` is treated as on while online
  so an unfocused window cannot stall the other player's inputs (decide with the netplay design).
- Lobby rule for cheats: public and ranked games force every `cheat.*` default; a private lobby may
  later expose a "House rules" panel that edits the host's synced values.
- Mods: `mods.enabled.*` will be **S** for mods that touch gameplay and **L** for cosmetic ones;
  the mod manifest must declare which.

---

## 15. Implementation order

1. **Model.** Extend `Settings` with all keys in this document, the enum tables, `Meta`, the
   environment overlay and `--cvar` parsing. Fill `AuroraConfig` in `preinit`
   (`desiredBackend`, `msaa`, `maxTextureAnisotropy`, `vsync`, `startFullscreen`,
   `allowJoystickBackgroundEvents`, `pauseOnFocusLost`, `allowCpuAdapter`, `windowWidth/Height`,
   `logLevel`). Done when: values survive a restart and every `MPPM_*` variable still overrides.
2. **Bridge.** `p2_config.h`, `gP2Config`, `bind_game_config()`; move the three `p2_env_*` call
   sites and the two panic sites; delete `p2_env_*`; atomics for audio and scheduler debug flags.
   Done when: the MWCC build is still byte-identical and the headless scripts pass unchanged.
3. **Window.** `config_bind.hpp`, `SettingsWindow` with all tabs and every v1 row, badges, help
   pane, Reset. Runtime appliers for VIDEO, master volume, background input, UI scale.
4. **Restart flow.** Snapshot, banner, modal, `request_restart`.
5. **In-game menu** with Resume, Settings, Controllers, Start Menu, Quit; overlay document with
   FPS counter, cheats badge, shader indicator, toasts.
6. **Controller window** and `input_defaults.cpp`.
7. **Preset screen** and update check.
8. **Save manager.**
9. **v1.1 game hooks**, one commit each: options sync (5.1), idle demo and launcher gate, title
   season, skip boot logos, hide HUD, invincible captains, unlimited sprays, pause in menu,
   Reset Game.

Every game-source edit is a `PC_ONLY(...)` on an existing line or a `#if TARGET_PC` block, per the
project rule that the matching build stays identical.

---

## 16. Open questions

1. **Pause while the menu is open.** Does a frame with no GX draws keep the last game frame under
   the UI in Aurora? This decides whether `game.pauseInMenu` ships in v1.1 or stays input-blocking
   only. Needs a five-minute test.
2. **Who owns volume and rumble.** This spec makes `config.json` the owner and the memory card a
   mirror (5.1). The alternative is the reverse (the card owns them, as on a GameCube), which makes
   the launcher unable to show or change them before a save is loaded. Confirm the choice.
3. **Default master volume.** 80 is proposed (Dusklight uses 60). Needs a listening check against
   the mixer's soft clip (`audio.cpp:610-613`).
4. **Enhanced preset anisotropy** (8x) and **Area resampler with Auto resolution**: confirm there
   is no HUD or font softening; otherwise the Enhanced preset keeps `bilinear`.
5. **MSAA.** Stays Experimental until the EFB-alpha shadow copy is verified. Is it worth exposing
   at all given Internal Resolution up to 8x?
6. **Update check while the repository is private.** `owallzz/moonsprout` returned 404 to anonymous
   requests today. Until it is public (or a public releases mirror exists) the check must fail
   quietly and "Check Now" should say "Updates are not available for this build."
7. **Environment variables lock their setting** (cannot be changed in the UI until the variable is
   removed). Good for reproducible tests; confirm this is also what you want for everyday use, or
   whether a user edit should win for the rest of the session.
8. **`MPPM_FPS=0` and friends** now mean "off" (they used to mean "on" because only presence was
   checked). Confirm no script relies on the old behaviour.
9. **"Start Menu" from in game = process restart** in v1. Acceptable, or is an in-process return
   (through the game's soft reset) required for the first release?
10. **Save Format** is shown only with Advanced Settings on. Should it be hidden completely (GCI
    folder only), with card images handled purely through Save Manager import?
11. **Day Speed and Allow Sunset on Day 1 are filed under CHEATS.** Day Speed below 100% is also an
    accessibility aid (longer days); should it move to GAMEPLAY as "Day Length" with the cheats
    badge only above or below 100%?
12. **Controller hint text** (section 8) is written from memory of the game's controls; check it
    against the manual. Ports 3 and 4: hide the tabs instead of showing a note?

---

## 17. Mods window

Window `ui::ModsWindow` (`port/launcher/mods_window.{hpp,cpp}`, `res/rml/mods.rcss`), milestone
M5.2 (`docs/online/M5_2_MODS.md`, `docs/mods/README.md`). It reads the mods folder through the
core's C++ view (`port/include/p2_mods.hpp`: `scan`, `mounted`, `write_load_order`, `ensure_dir`,
`content_hash`, `locked_reason`); it never parses a manifest itself.

**Where.** Start menu item **Mods** (`start_order::kMods`, before Settings: the menu reads Play,
Multiplayer, Mods, Settings, Quit since M5.3b) and the in-game bar entry **Mods** (`bar_order::kMods`, visible only over
a running game; it replaces the hidden slot of section 13). Both are registered by
`integration.cpp`. `open_mods_window(inGame)` pushes the window over the top document, like the
Save Manager; Esc / B closes it and the focus returns to the item that opened it.

**Mods folder.** `<data>/mods/` (`<data>` = `SDL_GetPrefPath(nullptr, "mppm")`, e.g.
`~/.local/share/mppm/mods/`), or `--mods-dir` / `MPPM_MODS_DIR`. `--no-mods` / `MPPM_NO_MODS=1`
turns mods off for the run. Enable states and the order live only in `<mods>/load_order.txt` (top =
lowest priority, `-<id>` = off); there is no `mods.*` setting.

**Layout** (the Save Manager's shell, section 10): no tab bar; a list pane on the left and the
selected item on the right, the selection kept across rebuilds. Selecting another item scrolls the
detail pane back to its top. Theme tokens only (`saves.rcss`
plus the help-note styles of `settings.rcss`), no new colours. Button prompts at the bottom edge
(DESIGN 5.17), in the shared `<button-prompts>` markup of `global.rcss`: "Enter Select", "Esc Back",
or "A" / "B" after a pad was used last; hidden after a mouse click or wheel
(`input::last_input_pointer`) until the next key or pad button.

- **List:** "Overview"; a "Mods" header with an Open Mods Folder icon button (creates the folder,
  then opens it in the file manager); one entry per mod in load order, then the folders that are
  not listed yet (sorted by id). Each entry has two lines (`mod-name`, `mod-state` in `mods.rcss`):
  the name across the whole row, ending in an ellipsis only where the row is too narrow (names
  longer than 34 characters are also clipped in C++), and below it the state word `On|Off|New|
  Problem|Missing` in small text, so the state never takes room from the name. With no mod: "No mods yet."; with mods off: "Mods are off for this run.";
  before the first scan: "Reading the mods folder…".
- **Overview:** header "Mods" / "<n> on · content <8 hex>" ("Stock content · <8 hex>" with none on;
  "…" while the folder is read). In game, the count and the hash are those of the running game
  (`p2_mods_count()`, `p2_mods_content_hash()`, the hash computed on the scan's worker because the
  first call reads the executable for the build id; "…" until it is ready), and the hash's help
  reads "The content hash this game was started with: …".
  Body: "Mods change the game's data files: arenas, caves, tuning and more. They load when the game
  starts." Section **Content hash**: the 64 hex digits and "Everyone in an online game needs the
  same content hash. It covers this build of Moonsprout and the mods that are on, in their order."
  Section **Mods folder**: the path, **Open Mods Folder**, then the folder's own problems
  (`load_order.txt` lines that were skipped, limits) as help notes.
- **Mod:** header name (two lines at most, in a smaller size when it has a run of more than 14
  characters without a space, such as a bare id) / "v<version> · <id>" (plus " · by <author>"). A box with
  the state line ("On", "Off", "New: turn it on to use it.", "This mod has problems and won't load.",
  "load_order.txt lists this mod, but its folder is gone."), "<n> files · <size>" and the
  description. **Load order**: "Mods lower in the list win when two of them change the same file.",
  "Position <i> of <n>." (counting the mods whose folder exists), then **Turn On** / **Turn Off**, **Move Up**, **Move Down**. **Conflicts**
  (only when there are any): "<Winner> overrides <Loser>: <key without the leading slash>", at most
  20 lines, then "and N more". For a file inside an archive that another mod replaces as a whole,
  the line ends in " (whole archive)" when that mod wins, or " (on top of the whole archive)" when
  the inner file goes on top of its archive (`Conflict::archiveMod`; README 5). **Problems**: each issue as a help note, an error or a warning glyph,
  with its manifest path under the sentence. **Files**: **Open Mod Folder** and the path.
- **Notes** at the top of the detail pane when they apply: "Locked while a replay or harness
  session runs." (`locked_reason()`), "Mods are off for this run (--no-mods).", "Return to the
  start menu to change mods." (in game), "The mods folder has changed since the game started. The
  list shows the folder; the changes apply after a restart from the start menu." (in game, when the
  folder's mods hash differs from the one the boot read), "Changes apply when you press Play: Moonsprout
  restarts first." (pending, start menu only), and "<Name> didn't load: <problem>" for each problem the mount
  found that the folder scan cannot (disc-dependent checks, from `mounted()`).

**Buttons.** Turn On is disabled for a mod with errors (Turn Off of such a mod stays available),
the moves for a mod with errors, at the ends of the list and for a missing folder, and all three in
game, while locked and with mods off. A listed mod whose folder is gone shows **Remove From List**
in place of Turn On / Off: it rewrites `load_order.txt` without that line (its warning is the state
line, not repeated under Problems). Disabled buttons still take the focus (the detail pane uses
`Pane::set_focus_disabled`); the focused one shows why under the buttons ("It is already at the
top.", "Fix the problems below to turn it on.", the lock reason, ...) and pressing it plays the
warning cue.

**Reading the folder.** `mods::scan()` opens, reads and hashes every listed file of every mod that
is on, and of the mods that are off up to 512 MiB in all (a larger off mod is checked by name and
size only; the 512 MiB limit for the mods that are on applies as before), so it never runs on the
UI thread: the window starts a scan
on a worker (a detached thread that fills a promise, one at a time) when it opens and after every
change, and computes the content hash there too. The worker gets the folder and the `--no-mods`
flag from `mods::scan_args()` on the UI thread and touches no other state, and nothing ever joins
it, so Quit never waits for a scan. Meanwhile the window shows the last finished scan (kept between
openings, so an unchanged folder shows at once and is not rebuilt), "…" for the hash, and the
load-order buttons are disabled with the note "Reading the mods folder…". Once the game has booted,
`mods_restart_pending()` waits only for a scan that follows a load-order change made in the window
(so Play right after a change still restarts); otherwise the last finished scan answers at once.

**Changes.** Turn On / Off and the moves build the new order and call `write_load_order` at once
(atomic: temporary file, then rename), then start a scan and rebuild the window when it is done;
the selected mod stays selected and the focus returns to the same button (Move Up stays on Move Up
of the moved mod). Move Up / Down swap with the nearest mod whose folder exists: a listed mod whose
folder is gone has no place to move past.
Listed mods keep their lines; the acted mod and a mod it swapped places with get a line (a "New"
mod becomes "Off" once it has a place); a listed mod whose folder is gone is dropped. A change plays
`kSoundItemChange`; a write error opens a message ("load_order.txt couldn't be written.").

**Restart rule** (D16 of the M5.2 design; mirrors the Save Manager). The mount reads
`load_order.txt` once, when the game boots (`p2_mods_mount`, before the start menu shows).
- Standalone start menu (no game booted yet): nothing is pending; Play boots with the new list.
- Live start menu (game booted behind it): `ui::mods_restart_pending()` is true when the mods hash
  of the window's last scan differs from `mounted().folderHash`, the folder's mods hash as the boot
  read it before the mount dropped any mod. A mod the mount dropped for a disc-dependent reason the
  scan cannot see is therefore no change, while any change to the hash's inputs (a file's bytes, a
  version, the order, a mod turned on or off, the dropped mod included) is.
  `launcher::restart_pending()` counts it, so Play restarts first, and the pending note shows.
- In game: read-only. The list still shows the folder as it is now; the count and the content hash
  are the running game's, and a changed folder gets its note.

**Lock.** Read-only while `p2_net_active()` (replay, record, checksum and `--vs-stage` harness runs:
"Locked while a replay or harness session runs.") and with `--no-mods`. An online match (M5.3b)
locks it the same way; the room shows a member whose content hash differs (section 18). Mods marked `"affects": "cosmetic"` will become local, the others synced
(section 14).

**Version label.** The start menu's version line gains " · N mod(s)" when `p2_mods_count() > 0`
(mods were mounted); it is unchanged otherwise.

**Untrusted text.** Names, authors, descriptions, ids and problem messages come from the mods. They
go into text nodes, or through `ui::escape` when they are part of RML (help notes); a name or a
description with markup shows as typed.

## 18. Online

Milestone M5.3b (`docs/ONLINE_MULTIPLAYER.md` 2.3-2.11, the M5.3b design section 10). The launcher
only shows and drives an online session; the session itself (lobby server, connection, lockstep)
is `mppm::online` in `port/net/session`, declared in `port/include/p2_online.hpp`. Every call
returns at once (the work runs on the net thread), so the windows poll `snapshot()`, `stall()` and
`take_notices()` every frame. Files: `port/launcher/online_ui.hpp` (shared helpers),
`online_window.{hpp,cpp}` (Multiplayer), `online_overlay.{hpp,cpp}` (wait pill),
`online_status_window.{hpp,cpp}` (in-game Online window), `online_hooks.cpp` (hooks, mirror,
notices, end messages), `res/rml/online.rcss`.

### 18.1 Settings (ONLINE tab, after INTERFACE)

| Key | Type | Default | Row | Help | Class |
|---|---|---|---|---|---|
| `online.playerName` | string, 0-24 code points (control characters removed, trimmed) | `""` (shown to others as "Player") | **Player Name** | "Shown to the other players in online games." | L, RT |
| `online.serverUrl` (alias `online.server_url`, the plan's spelling) | string (trimmed); `http(s)://`, `ws(s)://` | the CMake cache string `MPPM_DEFAULT_SERVER_URL`, a compile definition of `mppm_launcher`; `https://moonsprout-lobby.fly.dev` | **Lobby Server** | "The server that runs lobbies and join codes. Every relay address comes from it. Use your own to play on a self-hosted server." plus "Default: none yet" while the build has no default | L, RT; env `MPPM_SERVER_URL` |

- Both rows are text rows (`ui::TextField`: a placeholder while empty, "Player" and "None"); Y (or
  the key bound to it) resets them, as every row. The help's "Default: ..." line shows once a row
  differs from its default.
- `--cvar online.server_url=...` works too (`find_meta` also matches aliases).
- Mirror: `launcher::integration::init` → `online_ui::init` calls
  `mppm::online::configure({serverUrl, playerName})` at start and on every change of either var.
  The command line of an online run (`--server`, `--name`) wins over them (session side).
- Plaintext `ws://`/`http://` servers are accepted only on loopback (127.0.0.0/8, `::1`,
  `localhost`) or with `--net-allow-plaintext` (session side, D27).

### 18.2 Entry points

- Start menu item **Multiplayer** (`start_order::kMultiplayer`, between Play and Mods: the menu reads
  Play, Multiplayer, Mods, Settings, Quit), registered in `integration.cpp`
  `register_menu_entries()`; it opens the Multiplayer window over the menu. Like Mods and
  Settings it shows no glyph (only Play has one on the start menu); `group` (`ea21`, DESIGN 3.7) stays
  its glyph wherever a menu shows icons.
- In-game bar entry **Online** (`bar_order::kOnline`, id `online`, which replaces the hidden slot of
  section 13), visible while `mppm::online::active()` and the start menu is not up.
- Settings: the ONLINE tab (start menu and in game).

### 18.3 Multiplayer window (`ui::OnlineWindow`)

The Mods window's shell (section 17): no tab bar, a list pane on the left, the selected item on the
right (which takes the rest of the window), button prompts "Enter Select" / "Esc Back" at the
bottom. Esc from the right pane goes back to the list; from the list it closes the window.

- **Home.** List: section heading "Online", **Host a game**, **Join with a code**, and under them a
  muted line "Lobby server: <host>" (the URL's host and port, never credentials) or "No lobby server
  is set. Choose one in Settings › Online.". Without a server the two panes show that sentence and
  **Open Settings** (the settings window over this one, on the ONLINE tab).
- **Host a game.** "Pick the match. The others join with the code you get." **Mode** (stepper:
  Battle, Battle 2 vs 2, Challenge), **Stage** ("Stage 1" … "Stage 10") or **Course** ("Course 1" …
  "Course 30"), for Battle a **Handicaps** section with **Red team** and **Blue team** ("Normal", 1-10;
  team markers in the label), a **Lobby** section with **Password** (shown as dots, placeholder
  "None", "Optional. Players need it to join."), and the primary **Create lobby**
  (`mppm::online::host`; its error, e.g. no server, shows as an error note above the button).
- **Join with a code.** "Ask the host for the code of their lobby." **Code** (placeholder
  "ABCD-EFGH", monospace; shown as `ABCD-EFGH` once `normalize_code` accepts it; any case, spaces and
  hyphens; while it does not: the error ring and "A code has 8 letters and numbers, like
  ABCD-EFGH."), **Watch as a spectator** (toggle), and the primary **Join**, disabled (but focusable,
  with the warning cue) until the code is valid. A lobby that asks for a password (an Ended
  snapshot with `kEndRefused` whose message mentions the password) brings the pane back with a
  **Password** field and the note "This lobby has a password."
- **Connecting.** The right pane: "Connecting…", "Opening a lobby on <host>." or "Joining
  <code>.", the bead loader (DESIGN 5.15) and **Cancel** (`leave()`); Esc does the same; the list
  entries are disabled.
- **Room** (`Phase::Lobby`, `Starting`). Left: "Lobby", the code pill and a Copy icon button
  (clipboard, toast "Code copied"), the match line ("Battle · Stage 3", "Battle 2 vs 2 · Stage 1",
  "Challenge · Course 12"); section "Players" with one row per seat: the team marker (team = seat &
  1: team 0 a vermillion square, team 1 a blue square in a white frame, the M5.3a captain markers),
  "P<n>", the name, a star for the host, a "You" tag, then the ping badge ("23 ms"), the link glyph
  (direct; relay in sun yellow) and a "Ready" / "Not ready" pill ("Connecting…" while the member's
  transport is not up). An open seat reads "Open seat", with "Take seat" for this machine when it
  has no seat or another one (Enter takes it). A member whose content differs gets a warning
  outline and the session's note on a second line ("Louie has different mods: heave-arenas 1.2.0
  (host 1.1.0)."). Enter on this machine's row toggles Ready; the host's Enter on another member
  asks "Remove <name>?" / "They go back to their start menu." (danger, **Remove** → `kick`). Under
  the seats "Watching: <names>" for the members without a seat, then **Close lobby** (host) or
  **Leave lobby** (danger). Right, host: section "Match" with Mode, Stage/Course and, for Battle,
  Red / Blue handicap steppers (`set_config` on every step), the status line (`Snapshot::status`,
  a check glyph once `canStart`), **Ready** and the primary **Start game**, disabled (focusable,
  warning cue) until `canStart`. Right, member: the same match as read-only rows, the status line
  and the **Ready** toggle (the same row as the host's); without a seat "Take an open seat to play." (spectators: "You're
  watching this match. Take an open seat to play instead."). This machine's own content mismatch
  shows as a warning note (in place of the status line, which would repeat it) and keeps Ready
  disabled. The room opens with the focus on Ready.
- **Leaving.** **Close lobby** / **Leave lobby**, and Esc / the close button in the room, ask first:
  host "Close the lobby?" / "Everyone in it will be sent back." (**Close lobby**), member "Leave the
  lobby?" / "You can join again with the code while the lobby is open." (**Leave**); danger modals,
  focus on Cancel. A lobby is never left behind a closed window.
- **Starting.** The window closes itself; the session calls `p2_launcher_online_started()` (18.6),
  which runs the take-off.
- **Ended** in the lobby: a modal with the title of 18.5 (or "Couldn't join" / "Couldn't reach the
  lobby server" with the session's sentence) and **OK** (`leave()`), then the home panes.
- Rebuilds keep the focus on the same control (by key); member updates (ping, ready, joins) only
  refresh the rows. Notices become toasts (18.5).

### 18.4 In-game wait overlay (`ui::OnlineOverlay`)

A document in the focus stack just above the in-game bar that draws a glass pill at the top centre
(`--surface-bar`, rim, shine) while `stall().waiting`: the turning leaf (DESIGN 5.15) and "Waiting
for Louie (P2)…" (several: "Waiting for Louie and Navi…"; a client whose host is late: "Waiting for
the host…"; a load barrier: "Waiting for Louie to load…", or "Waiting for players to load…" when the
names are not known, as on a client), plus the seconds once the wait passes
3 s ("… 6 s"). Below the bar (and its tag) while the bar is open.

- `canDrop` (host, after 5 s; at a load barrier after 10 s, since loads differ between machines):
  **Drop Louie** (danger) → "Drop Louie?" / "Their captain will stand
  still for the rest of the match." → `drop_slot`. `canLeave` (client, after 10 s): **Leave game** →
  "Leave the game?" / "Your captain will stand still for the rest of the match." → leave and back to
  the start menu.
- The overlay counts as visible and active (`Document::visible()/active()`) only while it offers a
  button and nothing else is open: it then takes the focus (Enter / A presses, Esc / B gives the
  focus back to the game until the button changes, F1 opens the bar over it) and the game sees no
  pad input. Otherwise it is only drawn and never covers or blocks anything. It is permanent
  (survives `close_all_documents`).
- The session's hold loop keeps calling `launcher::update()`, so the pill animates and takes input
  while the game waits.

### 18.5 Notices and end messages

- Every frame `online_ui::update` turns `take_notices()` into toasts (DESIGN 5.14; info, success,
  warning, error; the strings go in as escaped RML).
- When the snapshot turns `Ended` after `Starting` / `InGame`: a modal over the frozen picture,
  **Back to start menu** = `leave()` then `request_restart(RestartTarget::StartMenu)` (the session
  is over, so `p2_net_active()` is 0 and the restart relaunches, `restart.cpp`); Esc does nothing.

| Reason | Title | Body |
|---|---|---|
| `kEndDesync` | "The games stopped matching" | "Desync at tick 4530. The match was stopped on every machine. A report was saved to <path>." |
| `kEndHostLeft` | "The host left the game" | "The match can't go on without the host." (lobby: "The host closed the lobby.") |
| `kEndConnectionLost` | "Connection lost" | "You were disconnected from the game." (lobby: "… from the lobby.") |
| `kEndKicked` | "Removed from the game" | "The host removed you from the game." (lobby: "… from the lobby.") |
| others | "Couldn't join" / "Couldn't reach the lobby server" / "The online game ended" | the session's sentence |

### 18.6 Hooks (`online_hooks.cpp`, declared in `p2_online.hpp`)

| Hook | Does |
|---|---|
| `p2_launcher_net_synced_json()` | `net_synced_settings().dump()` into a static string |
| `p2_launcher_net_lock(json)` | parse (no exceptions), `net_lock_settings`; 1 locked, 0 refused |
| `p2_launcher_net_unlock()` | `net_unlock_settings()`; re-locks with `{}` while harness flags keep `p2_net_active()` |
| `p2_launcher_online_started()` | with the start menu up: `Prelaunch::start_online()` (closes what is open over the menu, then the Play path without disc picking or a restart: standalone fades and boots, live runs the take-off); nothing on the command line |

During the take-off a press on any start menu item skips it (as Play does) instead of opening that
item's window.

### 18.7 In-game Online window (`ui::OnlineStatusWindow`)

From the bar's **Online**: "Online" with the code pill and the match line; section "Players" with
one read-only row per member (marker, P<n> or "Watching", name, star, "You", "Lagging" while the
match waits for that seat, "Dropped", ping, "Delay 2 ticks", link); section "Match": "The game is
still running. Online games never pause.", the waiting and status notes, "Lobby server: <host> ·
code <code>", and **Leave game** (danger confirmation, then back to the start menu). It closes when
the session ends (its end message takes over).

### 18.8 Test driver

`tools/net/m53b_ui_drive.sh <display> <command>` drives one window with the keyboard only
(`wait-menu`, `open-multiplayer`, `host [battle|2v2|challenge] [stage]`, `join <CODE> [spectate]`,
`ready`, `start`, `open-settings-online`, `close`, `key`, `type`, `shot`); the key sequences are in
its header. The caller gives each game a home whose `config.json` has `backend.discPath`,
`interface.presetChosen = true` and `online.serverUrl`.
