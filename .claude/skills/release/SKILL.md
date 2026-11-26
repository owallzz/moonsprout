---
name: release
description: Cut a Moonsprout release or prerelease. Picks the next version, tags it, lets the Release workflow build Windows (installer + portable zip), Linux (AppImage + tar.gz) and macOS (arm64 + Intel dmg) on GitHub-hosted runners, then checks the published release, its download links and assets. Use when the user says "release", "prerelease", "cut a release", "ship a build", "make a test build", "publish vX.Y.Z", or asks to rebuild or fix a release.
---

# Releasing Moonsprout

Releases are built by `.github/workflows/release.yml` on GitHub's free hosted runners, which run
when a `v*` tag is pushed. You choose the version, write the tag, and check the result. Never build
release files on your own machine, and never upload files by hand: everything published must come
from the workflow, so every release is reproducible from its tag.

| Tag | What it makes |
|---|---|
| `vX.Y.Z` | A full release, marked **Latest**. The in-game update check offers it to players. |
| `vX.Y.Z-rc.N`, `-beta.N`, `-alpha.N`, `-test.N` | A **prerelease**. GitHub's "latest release" API skips it, so players aren't told about it. |

The version comes only from the tag: CMake reads `git describe`, so the build shows `X.Y.Z` (or
`X.Y.Z-rc.N`) in the start menu, and the workflow fails if it doesn't. You don't need to edit any
file.

## 1. Check that you can release

```bash
git fetch origin --tags
git status --short --branch          # clean, on main, not ahead of or behind origin/main
gh run list --workflow ci.yml --branch main --limit 3   # the commit to release should be green
git tag --list 'v*' --sort=-v:refname | head -5        # the existing versions
```

Release only a commit that is already on `origin/main` and has passed CI. If CI is red or still
running, say so and stop, unless the user wants a test prerelease of that commit anyway.

## 2. Pick the version

Follow semver, counting from the newest tag:

- **Prerelease for testing**: the next version plus `-rc.N` (or what the user asks for). Raise `N`
  if that version already has prereleases, for example `v0.2.0-rc.1` → `v0.2.0-rc.2`.
- **Release**: drop the suffix from the last prerelease (`v0.2.0-rc.3` → `v0.2.0`), or raise the
  patch number for fixes, the minor number for features. Stay below 1.0.0 until the user decides
  the game is complete.

Tell the user the version you picked and why, in one line, unless they gave you one.

## 3. Write the notes and tag

The workflow writes the top of the release notes itself: download links grouped by platform, the
prerelease warning, the "bring your own disc" note, GitHub's list of merged changes since the
previous tag, and the SHA-256 checksums (`tools/release/notes.sh`). If the message of an annotated
tag has a body, it goes in after the download links, so put a short summary there of what changed for players
(what to test, for a prerelease). Write it for players, not developers, and keep it short.

```bash
git tag -a --cleanup=verbatim v0.2.0-rc.1 -m "Moonsprout 0.2.0-rc.1" -m "$(cat <<'EOF'
## What's new
- Windows builds: an installer and a portable zip.

## Please test
- Online 2-Player Battle between Windows and Linux.
EOF
)"
git push origin v0.2.0-rc.1
```

`--cleanup=verbatim` matters: without it git drops every line that starts with `#`, so the
`##` headings would vanish from the notes. Create every tag on the tip of `origin/main` unless the
user names another commit. Never move or
delete a tag that has been pushed. If a release is broken, make a new version instead.

## 4. Watch the build

```bash
gh run list --workflow release.yml --limit 1
gh run watch <run id> --exit-status
```

The run takes about an hour, mostly the Windows and macOS builds. Both macOS builds run on the
Intel runner (`macos-15-intel`); arm64 is cross-compiled there, because GitHub's arm64 macOS pool
often never picks up jobs. A macOS job that sits in "queued" for over 20 minutes, or ends with
"The job was not acquired by Runner", is that capacity problem, not a build error: retry it with
`gh run rerun <id> --failed`. If a job fails, read its log
(`gh run view <id> --log-failed`), fix the cause on `main` through the normal commit and CI process,
then cut the **next** prerelease number. A failure that a retry fixes (a runner or network hiccup)
can be retried with `gh run rerun <id> --failed`.

To rebuild an existing tag without a new commit (for example after a packaging fix to the workflow
on `main` that doesn't touch the game), run `gh workflow run release.yml -f tag=<tag>`. This builds
the tag's own tree, so it only helps when that tree is fine.

## 5. Check the release

```bash
gh release view <tag> --json isPrerelease,isDraft,assets,url \
  -q '{url, isPrerelease, isDraft, assets: [.assets[] | {name, size}]}'
```

It must have exactly these 7 files, with sizes in the tens of MB (not a few KB):

- `Moonsprout-<version>-windows-x64-setup.exe` and `…-windows-x64-portable.zip`
- `Moonsprout-<version>-linux-x86_64.AppImage` and `…-linux-x86_64.tar.gz`
- `Moonsprout-<version>-macos-arm64.dmg` and `…-macos-x86_64.dmg`
- `SHA256SUMS.txt`

Then:

1. Open the notes (`gh release view <tag>`) and check that the download section is first and that
   every link there names a file in the list above. Fetch one of them, for example
   `curl -sIL <link> | grep -i '^content-length'`.
2. `isPrerelease` must match the tag (true for `-rc.N` and the like, false for a full release).
3. If the machine has Xvfb and a Vulkan driver, smoke-test the Linux tarball headless, with a
   throwaway home so it can't touch real settings, and look at the screenshot. The start menu must
   show the tagged version:

   ```bash
   T=$(mktemp -d); gh release download <tag> -p '*linux-x86_64.tar.gz' -D "$T"
   tar -C "$T" -xzf "$T"/*.tar.gz; mkdir "$T/home"
   env -u WAYLAND_DISPLAY HOME="$T/home" SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy \
     timeout 40 xvfb-run -n 93 -s "-screen 0 1280x720x24" bash -c \
     "$T/Moonsprout/moonsprout & sleep 20; magick import -window root $T/shot.png; kill %1"
   ```
4. Check that both Linux packages really reach the online lobby. A start menu that renders proves
   nothing about online play: v0.1.0-rc.3 rendered fine but its lobby client had no WebSocket
   backend. First `tools/release/check-websocket.sh` on the tarball's `moonsprout` and on the
   AppImage's (`<AppImage> --appimage-extract usr/bin/moonsprout`). Then, if the user has a disc
   image, host a lobby on the official server from each package, with a throwaway home whose
   `.local/share/mppm/config.json` sets `backend.discPath`, `interface.presetChosen: true` and
   `online.serverUrl` (a command-line host waits for the game to boot, so it needs the disc):

   ```bash
   env -u WAYLAND_DISPLAY HOME="$T/home" XDG_DATA_HOME="$T/home/.local/share" MPPM_SKIP_LAUNCHER=1 \
     SDL_VIDEODRIVER=x11 SDL_AUDIODRIVER=dummy timeout 90 xvfb-run -a -s "-screen 0 1280x720x24" \
     "$T/Moonsprout/moonsprout" --host --code-file "$T/code"
   ```

   It passes when the log shows `mppm: lobby: created session XXXX-XXXX` (the server's answer over
   `wss://…/v1/ws`) and `$T/code` holds the code; stop the game then. `failed: unreachable` fails.
5. Check that no file contains game data: they're built from this repository, which has none, but
   make sure nothing unexpected was packed in. `tar tzf` and `unzip -l` should list only the
   program, `res/` (the launcher's own fonts, images and stylesheets), the DLLs next to the
   program on Windows, the license files and `README.txt`.

## 6. Report

Give the user the release URL, whether it's a prerelease, the 7 files with their sizes, and what you
checked. Mention anything you couldn't check. Linux can be run here; the Windows portable zip can be
smoke-tested under Wine (the `org.winehq.Wine` flatpak, headless under `xvfb-run`, with a throwaway
`WINEPREFIX`); the macOS builds can't be run here at all.

## Files

| File | What it does |
|---|---|
| `.github/workflows/release.yml` | The workflow: one job for the version, one build per platform, one job that publishes |
| `tools/release/package-linux.sh` | AppImage (linuxdeploy) and portable tar.gz |
| `tools/release/check-websocket.sh` | Fails unless a Linux binary has libcurl's WebSocket API (online lobby); run by CI and the release |
| `tools/release/package-windows.ps1` | Inno Setup installer and portable zip (with `portable.txt`) |
| `tools/release/package-macos.sh` | `Moonsprout.app` in a dmg (ad-hoc signed, not notarized) |
| `tools/release/notes.sh` | The release notes, with download links first |
| `tools/release/README-release.txt` | The `README.txt` that ships in every package |
| `packaging/` | Windows icon, version resource and installer script; Linux desktop file; macOS `Info.plist` |

Portable mode: a `portable.txt` next to the executable makes the game keep its settings, saves and
mods in a `data` folder beside it (`port/include/p2_data_dir.hpp`). Only the Windows zip ships one.
