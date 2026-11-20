<p align="center">
  <img src="docs/images/banner.png" alt="PS5X360 — Xbox 360 emulation on PlayStation 5" width="100%">
</p>

<p align="center">
  <a href="https://github.com/grzegorz2047/PS5X360Reloaded/releases"><strong>Download v1.0.3</strong></a> ·
  <a href="docs/release-notes/v1.0.3.md">Release notes</a> ·
  <a href="docs/INSTALLATION.md">Installation guide</a> ·
  <a href="docs/GAME-CONTENT.md">DLC, updates and discs</a> ·
  <a href="https://github.com/grzegorz2047/PS5X360Reloaded/issues">Report an issue</a>
</p>

# PS5X360 Reloaded

An **Xbox 360 emulator for PlayStation 5**, built on Xenia Canary with native DualSense input,
audio output and RADV Vulkan graphics.

This repository is a fork of [BrinooTk/PS5X360](https://github.com/BrinooTk/PS5X360). Since
**v1.0.3**, its releases are **built from source**: the PS5X360 v1.0.2 sources the original
author published, reassembled, with this fork's fixes on top. The `eboot.bin` is compiled, not
patched; it is **Vulkan only** and released under **GPL-3.0** with its complete source attached
to the release.

The earlier releases of this fork, `v1.0.2-opt` and `v1.0.2-opt-r2`, were the official v1.0.2
executable with binary patches. They are superseded: every patch they carried (and the ones made
after r2) is now a source change in v1.0.3. See [tools/binary-release](tools/binary-release/README.md).

**Games run, but compatibility varies.** Expect crashes, visual errors and uneven performance in
some titles. There is no promise of full-library compatibility or 60 FPS.

## v1.0.3

The home screen shows **PS5X360 v1.0.3**. The full notes, with what was confirmed on the console
and what only by host tests, are in [docs/release-notes/v1.0.3.md](docs/release-notes/v1.0.3.md).

- **Game fixes:** Battlefield 3 works (with its built-in `memexport` preset), Handball 16 starts
  and gets past START, Naruto: The Broken Bond no longer closes at start, and Minecraft runs at
  60 FPS in the world instead of about 1 FPS.
- **The `v1.0.2-opt` patches in source (fixes 01–13):** JIT, memory protection and fault-handler
  speed-ups, the GPU thread on its own core, FSI and frame generation settings that work on image
  installs, the cache folder, selective pipeline waits, the VSync cap, a third background-shader
  mode and a per-game FSI option. Fixes 06–13 were never in a binary release.
- **New in source:** save drive choice (internal, M.2 and USB drives), starting a game straight
  from a PS5 home screen tile (forwarders), the guide's network row, English as the interface's
  source language, a crash notice instead of a silent freeze, title switches, XMP music at any
  sample rate, and smaller gaps to the official 1.0.2 closed (button colours, covers, network
  self-test, language files, the USB Kinect relay).
- **Presets:** Battlefield: Bad Company holds 60 FPS (FSI off), Fable II is back to the official
  preset, Fable III keeps the r2 preset and optional patches.

## Installation

<img src="docs/images/installation.png" alt="Download the ZIP, copy PPSA50011 via FTP, add one folder per game, and launch PS5X360" width="100%">

The [v1.0.3 release](https://github.com/grzegorz2047/PS5X360Reloaded/releases) has:
- `PPSA50011.ffpkg`: a UFS2 image, mounted by etaHEN / ShadowMountPlus.
- `PPSA50011.ffpfsc`: a compressed PFS image, mounted by ShadowMountPlus.
- `PPSA50011.zip`: the `PPSA50011` folder to copy over FTP (checksum in `PPSA50011.zip.sha256`).
- The game patch files (`*.patch.toml`, also inside the packages), the GPL-3.0 source archives
  (see [Licenses](#credits-and-licenses)) and `SHA256SUMS.txt` over all files.

**Image (`.ffpkg` / `.ffpfsc`):** close the emulator, replace the previous PS5X360 image with the
new one in the place ShadowMountPlus mounts it from, and start PS5X360 again. If a game hung on
exit, restart the console first: the old image can stay mounted until then. Settings saved in
`/download0/xbox360ps5` are not part of the image. An image is read-only, so put the games on
another drive and add the folder in **Settings → Game folders**.

**ZIP:**
1. Extract `PPSA50011.zip` on your computer.
2. Using FTP, copy the **whole** `PPSA50011` folder to `/data/homebrew/`, overwriting existing
   files. Keep the `saves`, `cache` and `logs` subfolders of the previous install. The executable
   must end up at `/data/homebrew/PPSA50011/eboot.bin`.
3. Let ShadowMountPlus register the title, then open **PS5X360** from the PS5 home screen.
4. Put each game in its own folder under `PPSA50011/assets/roms/`, or in a folder added in
   **Settings → Game folders**. Reopen the emulator or select **Settings → Refresh game list**.

> **Do not replace only `eboot.bin`** in an existing 1.0.2 or r2 install. The v1.0.3 executable
> reads language files keyed in English (`assets/lang/*.txt`, 16 files including `pt-BR.txt`);
> with the official files the whole interface is in English whatever language is selected, and
> the r2 `eboot.bin` does not match the v1.0.3 language files either. Always install the whole
> package. To go back to r2, install the whole r2 package.

The development setup uses **PS5 firmware 13.60 with homebrew access, etaHEN, ShadowMountPlus
and kstuff**. Other setups have not been verified. You need an already working homebrew
environment. The Helper (autoload ELF) of the official releases updates from the official
releases page, so it may replace this build with the official one (not checked).

[Full installation and usage guide →](docs/INSTALLATION.md)

## Game status on the console

Results on a PS5. Most come from `1.0.3_candidate_N` test builds (source builds labelled that way
on the home screen); rows marked *binary build* were measured on the official v1.0.2 executable
with this fork's binary patches and not again on a source build. FPS values come from the
emulator's own logs.

| Game | Status | Notes |
| --- | --- | --- |
| Battlefield 3 (`45410950`) | works, about 25–30 FPS | Fix 30 with the built-in `memexport=1` preset, and fix 20. Menu and levels without hanging in `candidate_36` and `candidate_37` (with `memexport` set by hand). The preset on its own is host-tested, not yet tried on the console without the manual setting. The official 1.0.2 hangs. |
| Minecraft: Xbox 360 Edition (`584111F7`) | 59–60 FPS in the world in **Online** or **Offline** mode | Fix 31 (`candidate_33`; 1.3 FPS without it). In **System Link** mode the game can crash while loading or leaving a world; fix 45 shows the crash and the guide returns to the library. |
| Handball 16 (`573607E4`) | starts, gets past START, about 50–57 FPS | Fix 26 (`candidate_26`, with extra diagnostics). |
| Naruto: The Broken Bond (`55530825`) | starts, reaches the save-location prompt | Fix 28 (`candidate_23`, before some later fixes; not retested with the full build). |
| Call of Duty: Black Ops | runs; 18.6–18.8 FPS (*binary build*) | Switches from its menu to the campaign (fix 33). Character hair is drawn in wrong colours in every build, including r2 ([#10](https://github.com/grzegorz2047/PS5X360Reloaded/issues/10)). |
| Call of Duty: Modern Warfare 2 | runs; about 20 FPS (*binary build*) | No regressions in the source candidates. |
| Army of Two | works | Regression run of `candidate_47`; started from a forwarder tile in `candidate_34`. |
| Battlefield: Bad Company (`454107F9`) | 60 FPS after the first shader compile (*binary build*) | With the game's 60 FPS patch, the preset (FSI off, `readback=0`, `pipeline_threads=3`) and the VSync cap (fix 10). |
| Fable II (`4D5307F1`) | 30 FPS, the game's own cap (*binary build*) | v1.0.3 uses the official preset again; `fast_locks` on makes the game ignore the controller. |
| Fable III (`4D5308D6`) | about 22–24 FPS (*binary build*) | r2 preset, config and optional patches, unchanged in v1.0.3. |
| Ninja Gaiden II (`544307D5`) | closes about 30 s after the intro videos | [#11](https://github.com/grzegorz2047/PS5X360Reloaded/issues/11) |
| Dragon Ball Z for Kinect (`4E4D084E`) | the only game offered the emulated Kinect camera, as in the official 1.0.2 | Steady 30 FPS with a test relay (`candidate_49`). Other Kinect games (e.g. Kinect Sports) keep asking for the camera and stay on a white screen. |

## Known limitations

- **No OpenGL renderer** ([#6](https://github.com/grzegorz2047/PS5X360Reloaded/issues/6)): its
  sources were never published. Choosing OpenGL in a game's settings falls back to Vulkan.
- **No games from the PS5 disc drive** ([#7](https://github.com/grzegorz2047/PS5X360Reloaded/issues/7)):
  a disc in the drive is not listed, and the `disc-helper.elf` relay does not work.
- **Physical USB Kinect** ([#8](https://github.com/grzegorz2047/PS5X360Reloaded/issues/8)): the
  USB camera and relay client are back (fix 44) and the relay client works on the console with a
  test relay, but nobody has tried it with a real Kinect for Xbox 360 (model 1414/1473) yet.
- **Smaller gaps to the official 1.0.2** ([#9](https://github.com/grzegorz2047/PS5X360Reloaded/issues/9)):
  done in v1.0.3 (fixes 40–43). What still differs does so on purpose: a private Fable II input
  diagnostic is left out, the dark theme is reconstructed, and the debug-output wrapper belongs to
  the missing OpenGL renderer.
- **Compiler flags** ([#2](https://github.com/grzegorz2047/PS5X360Reloaded/issues/2)): the title
  stays at `-O2`; `-O3` titles and ThinLTO gave no measurable gain.
- **Newer upstream JIT work** ([#5](https://github.com/grzegorz2047/PS5X360Reloaded/issues/5)):
  the changes that apply to this core are in (fixes 01 and 38); later upstream series and newer
  PS5_Vulkan / PS5_Mesa are not.
- Not byte-identical to the official 1.0.2: the `eboot.bin` is a reconstruction, about 22 MiB
  smaller, almost all of it the missing OpenGL stack.

## Start a game from the PS5 home screen (forwarders)

A forwarder is a small app with its own home screen tile that starts PS5X360 straight into one
game (fix 21): `--rom <game>` with a full path, a path in the game folders, a folder or a title
ID, and `--exit-after-game` to return to the PS5 home screen when the game closes. Tiles are made
with Forwarder Manager. Close PS5X360 before using a tile: arguments only reach a new PS5X360
process. When the forwarder launcher is already running, ShadowMountPlus may refuse the start
(`0x80940033`, [drakmor/ShadowMountPlus#196](https://github.com/drakmor/ShadowMountPlus/issues/196)).
Setup and workarounds: [docs/FORWARDER.md](docs/FORWARDER.md).

## Organize your games

```text
PPSA50011/
├── eboot.bin
├── sce_module/
├── sce_sys/
└── assets/
    └── roms/
        ├── Game One/
        │   ├── default.xex
        │   └── ...game data...
        ├── Game Two/
        │   └── game.iso
        └── Game Three/
            ├── ...GOD or STFS package...
            └── ...matching .data folder, if required...
```

Keep all extracted game data together. Use **Settings → Game folders** to add more locations,
including external drives. See the
[folder configuration guide](docs/INSTALLATION.md#multiple-locations-and-external-devices-development-v030-preview).

**No games, console BIOS or keys are included.**

## Controls

| DualSense button | Home | Library |
| --- | --- | --- |
| D-pad / stick up and down | Change channel | — |
| D-pad / stick left and right | Choose a slot | Choose a game |
| Cross | Open | Open the game's sheet (Play, Patches, Settings, Achievements, DLCs, Updates) |
| Triangle | — | The game's settings |
| Square | Settings | Settings |
| L1 / R1 | — | Change library filter |
| Circle | — | Back to the home |
| Touchpad, during a game | Open the emulator guide | |

The guide offers resume, the library over the running game, settings, Xbox BACK and exit. To use
the touchpad as Xbox BACK instead, change **Settings → Touchpad click**; the guide then opens with
**OPTIONS + touchpad**.

## What PS5X360 includes

- A home screen and cover library for extracted games (XEX), ISO images and GOD/STFS packages,
  with folders and filters.
- **DLC and title updates**, installed from the game's sheet or automatically when you press Play;
  **games with several discs**. See [DLC, updates and discs](docs/GAME-CONTENT.md).
- Per-game settings and community Xenia Canary patches, matched to the game's executable version.
- Player profiles for up to four local players, saved games and achievements with notifications.
- An in-game guide, opened with the touchpad: resume, the library over the running game, settings.
- Frame generation (FSR3, experimental), FPS display, CAS sharpening and FSR upscaling.
- Kinect through a phone camera (experimental; see the Kinect limits above), settings from a
  phone or computer.
- Online play through Xenia netplay, and System Link (experimental). Use the game's Xbox LIVE
  menu; System Link only finds games on your home network.
- Covers from XboxUnity and Microsoft's image server.
- An interface in **16 languages**, following the PS5 language, and a dark theme.
- Per-game log files, downloadable as a ZIP. The PS5X360 Content Installer for Windows
  ([tools/content-installer](tools/content-installer/README.md)) installs DLC and title updates
  over the network.

## Compatibility and bug reports

Booting into a menu is not the same as completing gameplay. The
[compatibility notes](docs/COMPATIBILITY.md) separate what was seen on a console from host tests.

When you report a bug, include the game, title ID, region and version; the enabled patches; the
firmware and the emulator version (the home screen shows it); the exact point where the game
fails; a short video or screenshot if you can; and the matching game-session logs. Logs are in
`PPSA50011/logs`, or in `download0/xbox360ps5/LOGS` if that folder cannot be written. The
included log downloader can create a ZIP to share.

- **This build (v1.0.3, source builds):** report on
  [this repository](https://github.com/grzegorz2047/PS5X360Reloaded/issues).
- **The official build:** report on [BrinooTk/PS5X360](https://github.com/BrinooTk/PS5X360/issues).

## Branches

| Branch | Contents |
| --- | --- |
| [`source-1.0.2-patches`](https://github.com/grzegorz2047/PS5X360Reloaded/tree/source-1.0.2-patches) | Development: the assembly and build scripts of the source rebuild, the reconstructed files and every fix (`tools/source-1.0.2/fixes`). Fixes are made on `fix/*` branches and merged here. |
| [`release/source-1.0.3`](https://github.com/grzegorz2047/PS5X360Reloaded/tree/release/source-1.0.3) | The v1.0.3 release commit. A push builds the draft release (`release-source.yml`). |
| [`main`](https://github.com/grzegorz2047/PS5X360Reloaded/tree/main) | The landing page, the tools of the superseded binary releases (`tools/binary-release`), and the repository's older C++ (v0.5.7-fix.1, not used by the releases). |

## Building from source

The v1.0.2 sources are not one tree: [`tools/source-1.0.2/assemble.py`](tools/source-1.0.2/assemble.py)
downloads the author's source packages (`0.5.9-fix1`, `0.6.0`, `1.0.0`, `1.0.2`, pinned by
SHA-256) and the Xenia Canary netplay core (`AdrianCassar/xenia-canary` `d2cf527aa`), layers them,
adds the reconstructed headers and applies the fixes. What came from where, and what is missing,
is in [docs/SOURCE-1.0.2.md](docs/SOURCE-1.0.2.md); the fixes are listed in
[tools/source-1.0.2/fixes](tools/source-1.0.2/fixes/README.md).

**Host checks** (no PS5 toolchain; needs Python 3 and clang 18):

```sh
python3 -I tools/source-1.0.2/assemble.py --out build/src102
bash tools/source-1.0.2/run-fix-checks.sh build/src102
```

**The `eboot.bin`:** set up the toolchain as the steps of
[`build-source-1.0.2.yml`](.github/workflows/build-source-1.0.2.yml) do (Ubuntu 24.04,
clang/lld 18, Vulkan SDK 1.4.341.1 shader tools, the `mihawk-99` repositories `PS5_Vulkan`,
`PS5_Mesa` and `PS5_PayloadSDK` at the pinned commits in `.deps/references`, RADV built by
`tools/build-radv-platform.sh`), then:

```sh
bash tools/source-1.0.2/build.sh build/src102 build/src102-out   # -> build/src102-out/eboot.bin
```

**In GitHub Actions:**
- `source-fix-checks.yml` runs the host checks on every push to `source-1.0.2-patches` and the
  `fix/*` branches.
- `build-source-1.0.2.yml` (by hand: `gh workflow run build-source-1.0.2.yml --repo
  grzegorz2047/PS5X360Reloaded --ref source-1.0.2-patches`) runs the host checks, then builds a
  test `eboot.bin`. Its artifact `eboot-1.0.2-from-source-NOT-FOR-RELEASE` is for console tests
  only; test builds are labelled **PS5X360 v1.0.3_candidate_N** on the home screen
  (`tools/source-1.0.2/BUILD_VERSION`).
- `release-source.yml` runs on a push to `release/source-X.Y.Z`: a release build (version label
  `X.Y.Z`, no fix left out), the package (`tools/source-1.0.2/package.py`), the images and a
  **draft** release `vX.Y.Z` with the source archives. Publishing the draft is a manual step.

## Credits and licenses

Based on [PS5X360](https://github.com/BrinooTk/PS5X360) by BrinooTk and on
[Xenia Canary](https://github.com/xenia-canary/xenia-canary). Other components: the
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) RADV work and the native runtime and tooling
listed in [Credits](docs/CREDITS.md); online play from
[Xenia Canary netplay](https://github.com/AdrianCassar/xenia-canary) by AdrianCassar, with
libcurl, wolfSSL and miniupnpc; frame generation from
[ps5-framegen](https://github.com/Swordpdf/ps5-framegen) by Swordpdf; community patches from
[xenia-canary/game-patches](https://github.com/xenia-canary/game-patches); covers from XboxUnity
and Microsoft's image server.

Licenses:
- **The released `eboot.bin` is GPL-3.0** as a whole: it links wolfSSL and ps5-framegen (both
  GPL-3.0-or-later). Each part keeps its own licence too: PS5X360 code is MIT, Xenia is
  BSD-3-Clause. The package's `PPSA50011/licenses/` folder has every licence text and the notice
  `PS5X360-1.0.3-NOTICE.txt`.
- **The source is attached to each release:** `PS5X360-1.0.3-source.tar.xz` (the tree the
  `eboot.bin` was compiled from, with `SOURCES.json`), `PS5X360Reloaded-1.0.3-scripts.tar.gz`
  (this repository at the release commit), `PS5X360-1.0.3-toolchain-source.tar.xz` (the toolchain
  and the statically linked RADV/Mesa and PS5 runtime at the pinned commits) and the build log.
  Anyone who distributes a rebuilt `eboot.bin` must do so under the GPL-3.0, with its complete
  source.
- This repository's own code is MIT ([LICENSE](LICENSE)); bundled third-party components keep
  their licenses ([licenses/](licenses/), the font's [SIL Open Font License](assets/fonts/OFL.txt)).

PS5X360 is an independent community project and is not affiliated with Sony or Microsoft. The
README artwork is illustrative branding, not an emulator screenshot.

## Support the projects

To support the original author, BrinooTk, you can contribute through
[Ko-fi](https://ko-fi.com/brinoblitz). Testing, bug reports and sharing the project also help.

Older M0–M8 milestones and Portuguese research notes are preserved in
[the port history](docs/PORT_HISTORY_PT.md); the upstream changelog is in [CHANGELOG.md](CHANGELOG.md).

## Automatic diagnostic reports

The optional **AutoLog ELF preview** of the official releases reads game logs on the PS5 and
sends them to the developer of the official build, without a PC. It is loaded separately once per
console boot. Before enabling it, read the [reporting notice and instructions](native/autolog/README.md)
(opt-out and limits); for the optional PC fallback see [AutoLog setup](docs/AUTOLOG.md).
