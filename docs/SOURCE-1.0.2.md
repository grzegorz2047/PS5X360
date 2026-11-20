# Rebuilding PS5X360 v1.0.2 from the published sources

This page records whether the official PS5X360 v1.0.2 `eboot.bin` (package `PPSA50011.zip`,
`eboot.bin` SHA-256 `b1d1b3cb…`) can be rebuilt from the author's public source drops, what
is missing, and how close a rebuild gets.

Since **v1.0.3**, this repository's releases are built this way: the `eboot.bin` is compiled
from the assembled tree with the fixes below, and the release carries its GPL-3.0 corresponding
source (see [Releases](#releases)). The earlier binary releases (`v1.0.2-opt`,
`v1.0.2-opt-r2`: the official executable with binary patches,
[`tools/binary-release`](../tools/binary-release/README.md)) are superseded.

- Assembly: [`tools/source-1.0.2/assemble.py`](../tools/source-1.0.2/assemble.py)
- Build: [`tools/source-1.0.2/build.sh`](../tools/source-1.0.2/build.sh), workflow
  [`build-source-1.0.2.yml`](../.github/workflows/build-source-1.0.2.yml)
- Release (draft, package and corresponding source):
  [`release-source.yml`](../.github/workflows/release-source.yml) (see [Releases](#releases))
- Comparison: [`tools/source-1.0.2/compare.py`](../tools/source-1.0.2/compare.py)
- The binary patches of the `v1.0.2-opt*` releases as source changes, and the later fixes:
  [`tools/source-1.0.2/fixes`](../tools/source-1.0.2/fixes/README.md) (see
  [Release fixes](#release-fixes-ported-from-the-binary-patches))

## Result

**It builds.** [Run 44497867041](https://github.com/grzegorz2047/PS5X360Reloaded/actions/runs/44497867041)
assembled the tree, compiled every core and title object and linked a signed `eboot.bin`
(37,551,301 bytes, SHA-256 `65e6e9c0…`; `eboot.elf` `9ef24b10…`). It is a Vulkan-only
reconstruction of 1.0.2, about 22 MiB smaller than the official executable, almost all of it
the OpenGL renderer stack that was never published (see
[Comparison](#comparison-with-the-official-executable)).

On the console (user's test of that build): the launcher, Modern Warfare 2, Black Ops and Dragon
Age II run like the official build; the visible difference was the missing dark home theme,
reconstructed since.

Build with the `gameinfo.txt` reader ([run 44502949131](https://github.com/grzegorz2047/PS5X360Reloaded/actions/runs/44502949131))
and the dark theme, before the release fixes: [run 44507850129](https://github.com/grzegorz2047/PS5X360Reloaded/actions/runs/44507850129),
`eboot.bin` 37,574,693 bytes, SHA-256 `b7525acb…` (`eboot.elf` `a251fd04…`).
Its official-only strings drop to 18,074; the dark theme's and `gameinfo.txt`'s are now in both.
That build was not run on a console by itself; the later test builds that contain it, labelled
`1.0.3_candidate_N` on the home screen (`tools/source-1.0.2/BUILD_VERSION`), were (see
[Release fixes](#release-fixes-ported-from-the-binary-patches)).

The workflow keeps the built RADV driver and payload SDK in the Actions cache (keyed by the
pinned reference commits) and uses ccache: with the cache, the RADV build and Mesa's packages are
skipped (dependencies 36 s instead of up to 9 min). It runs by hand (`workflow_dispatch`; GitHub
offers the button because `main` carries a copy of the workflow file) for a test build, and as a
reusable workflow (`workflow_call`) from the source release (see [Releases](#releases)).

Earlier runs, each fixing what the previous one found:
[44482112072](https://github.com/grzegorz2047/PS5X360Reloaded/actions/runs/44482112072) (stale core
headers, missing 1.0.x language code),
[44486101760](https://github.com/grzegorz2047/PS5X360Reloaded/actions/runs/44486101760) (5 files),
[44491870772](https://github.com/grzegorz2047/PS5X360Reloaded/actions/runs/44491870772) (compiled; 4
undefined symbols at the link).

## Short answer

The **emulator core** of 1.0.2 is fully reproducible from public material. The **title**
(launcher, home screen, platform layer and the port's own features) is not: 1.0.2 was never
built from one source tree. The author linked it from objects of earlier private builds plus a
few recompiled ones (`port/build_online.py --link` keeps every object of the base build
`core-e6cd2e4-20261021`, eboot `0854c878…`, and replaces only the ones that changed). The
sources of many of those retained objects were published in earlier drops; some never were.
The author says so in the 0.5.9 Fix 1 drop: *"Historical source recovery does not establish a
fully reproducible clean build of every retained object."*

So a rebuild is possible, but it is a reconstruction: the newest published version of every
file, a handful of reconstructed headers, and no OpenGL renderer.

## Inputs

| Input | Where | Pinned by |
|---|---|---|
| `PS5X360-source-0.5.9-fix1.zip` | release `v0.5.9-fix1` | SHA-256 `56de622c…` |
| `PS5X360-source-0.6.0-changes.zip` | release `v0.6.0` | SHA-256 `315a9df8…` |
| `PS5X360-source-1.0.0-changes.zip` | release `v1.0.0` | SHA-256 `b8854e22…` |
| `PS5X360-source-1.0.2-changes.zip` | release `v1.0.2` | SHA-256 `d9bd0346…` |
| Xenia Canary netplay | `AdrianCassar/xenia-canary` `netplay_canary_experimental` | commit `d2cf527aa` |
| wolfSSL 5.9.4, libcurl 8.23.0-DEV, miniupnpc | netplay's submodules | `3c5eead`, `3163ec0`, `b91c025` (the commits 1.0.0's notice names) |
| Mbed TLS 3.6.7 | GitHub release archive | SHA-256 `a7e8bcbe…` (the archive the author's Helper build records) |
| `PPSA50011.zip` (official package) and its `KINECT-source-and-licenses.zip` (fix 44) | release `v1.0.2` | SHA-256 `230359c2…` (as `tools/binary-release`) and `8cff3386…`; the 1.0.x Kinect headers no drop has (`KINECT_LAYERS` in `assemble.py`) |
| PS5 toolchain, RADV, payload SDK | `mihawk-99/PS5_Vulkan`, `PS5_Mesa`, `PS5_PayloadSDK` | the commits `build-ps5.yml` pins |

## What came from which drop

### Core (`.deps/xenia-canary`) — complete

1. netplay `d2cf527aa` with its submodules.
2. **1.0.0** `src/core/ps5-port-over-netplay.diff`: applies cleanly (149 files; the tree 1.0.0
   names as `TREE-HEAD` `d8e0a65`).
3. **1.0.2** `online-core/commits-since-netplay-merge.diff`: it starts at the netplay merge
   (`5acad1590`), so it overlaps step 2. Applied hunk by hunk
   ([`layer.py`](../tools/source-1.0.2/layer.py)): every hunk is either already present or
   applies; only one is new (`xam_content_aggregate.cc`). Three new files it carries are
   identical to 1.0.0's.
4. **1.0.2** `online-core/uncommitted.diff`: 29 hunks (XLiveAPI, UPnP, curl, sockets), all apply.
5. **1.0.2** `libcurl-3163ec0.diff` in `third_party/libcurl` (non-blocking reads on PS5 sockets).
6. **1.0.2** `1.0.2-package/core/*.cc` and its diffs are 2026-10-20 snapshots of the pre-netplay
   tree; the netplay tree already contains them (`memory.cc` and `vulkan_shared_memory.cc` are
   identical, `vulkan_command_processor.cc` differs only by newer upstream Canary code), so they
   are not applied.
7. Not the author's: [`core-build-adapters.patch`](../tools/source-1.0.2/core-build-adapters.patch),
   the CMake glue this repository already uses for the PS5 (no LTO, POSIX platform sources,
   FFmpeg's config). The author compiled the core with recorded compiler commands instead of
   Canary's CMake files, so these were never needed there. This repository's own JIT change to
   `ppc_emit_memory.cc` is deliberately **not** applied (the official binary does not have it).

### Network libraries (`online/`)

1.0.0's `src/net-libs/CMakeLists.txt` and `ps5_rng.h`, unchanged, over the netplay submodules.

### Title (`include/`, `platform/`, `src/`, `port/`)

Layered oldest to newest; a later file replaces an earlier one. The exact list is
`TITLE_LAYERS` in `assemble.py`, and `SOURCES.json` in the build artifact records the origin
and hash of every file.

| Layer | Files |
|---|---|
| 0.5.9 Fix 1 `source/` | `include/`, `platform/`, `src/` (the complete 0.5.9 working snapshot) |
| 0.5.9 Fix 1 `release-overlays/` | headers of the frozen 0.5.9 builds; `title_switch.cpp`, `multi_disc.cpp` (multi-disc), `phone_camera.hpp` |
| 0.6.0 | `dualsense_input.cpp`, `socket_errno.h` (net fix); `overlay_rates.hpp`; `disc_catalog`, `disc_library`, `readonly_file_pool.hpp` (Bully loading); `web_settings.cpp`, `dlc_manager.hpp`, `phone_camera_*` (Kinect native); ps5-framegen `vendor/` and `source_clock.hpp`, `frame_pacing.hpp`, `shader_stutter_trace.hpp` (framegen.4); `update_manager.hpp`; `framegen_config.hpp`, `pacing_adapter.hpp`, `adaptive_pacing.hpp`, `presenter_sidecar.hpp`, `shared/output_metrics.hpp` (framegen-always, the last framegen folder) |
| 1.0.0 | `canary_game_main.cpp`, `launcher.cpp`, `kinect_*`, `game_art.*`, `xex_package_decoder.*`, `dashboard_mode.hpp`, `ps5_resolver.cpp` |
| 1.0.2 | `ps5_resolver.cpp` (resolver bound in the link), `phone_camera_tls.cpp` (open phone-camera folder) |

Excluded on purpose: `release-overlays/build/kinect-update-20261008/native/include/xenia/` is a
frozen copy of the *older* Canary core's headers; on the include path it hides the netplay
core's headers (the first CI run failed in ~150 core files because of it).

From this repository (build glue no drop contains): `platform/ps5/compat/`,
`tooling/native-game-symbols.map`, `tools/resolve-mesa-weaks.py`, `assets/fonts/`, licences.

## Gaps

| Gap | Used by | Filled how | Effect |
|---|---|---|---|
| OpenGL game renderer: the "glvk" Vulkan-on-OpenGL layer, 1.0.x `radv_dispatch.cpp`'s `SelectOpenGLDriver`, `xbox360ps5/vulkan_driver.hpp`, the namespaced ps5-opengl SDK (`build/dual-renderer/namespaced-gl-sdk-all`, `libPS5OpenGL.a`) | `canary_game_main.cpp`, core `vulkan_presenter.cc`, the 1.0.2 link | **open** (only 0.5.9 `research/dual-renderer/` and `SDK-1.0.1/sources/ps5-opengl.tar` exist, for an older design). Reconstructed stand-ins: `OpenGLDriverSelected()` is false, `SelectOpenGLDriver` only logs, `glvk::PictureState()` reports "only black", so choosing OpenGL falls back to Vulkan through the launcher's own recovery | Vulkan only; ~22 MiB of the official image |
| `cvars::ps5_frame_interpolation` definition | core `vulkan_presenter.cc`, `presenter_sidecar.hpp` | reconstructed `DEFINE_bool`, registered as the official 1.0.2 registers it (fix 40, eboot 0x1230a6-0x1230be): default false, the official description, category `Vulkan`; still set from `framegen.conf` as before, and every code that reads it is published | none: the definition is exact |
| `home_navigation.hpp` (`build/ui-kinect-20261020`) | `launcher.cpp` | reconstructed: `kinect_ui::FaceButton`, empty `HomeNavigation`, and `HintRgb` with the official 1.0.2 colours (fix 40: cross 0x55a329, circle 0xd84639, square 0x287ac3, triangle 0xe3b52d, as `NxeHint` 0x211f82 and `Canvas::Hint` load them) | none: the button-hint colours are exact; `launcher.cpp` uses only the include and the instance |
| `interface_choices.hpp` (same folder) | `launcher.cpp` (its `#include` only) | reconstructed: empty | none: nothing in the title reads it, so there is nothing to recover (issue #9) |
| `disc_scan.hpp` (`build/disc-20261015`) | `launcher.cpp` | reconstructed: finds no disc | games on a disc in the console's drive are not listed (the official's `DISC …` log lines are absent) |
| `disc_relay.hpp` (client of `disc-helper.elf`, port 9302; protocol unknown) | core `mapped_memory_posix.cc` | reconstructed: never connects | discs the title cannot open directly are refused |
| `xbox360ps5/game_info.hpp` (x360db reader) | `launcher.cpp` | reconstructed: reads the official package's own `/app0/assets/gameinfo.txt` (INI by title id); host-tested by `check-game-info.cpp` (fix 43) on a committed excerpt of that file, and on the whole file when `PS5X360_GAMEINFO` names it | game information tab should work like the official one; untested on hardware |
| 1.0.x `i18n.hpp` (17 interface languages) | `launcher.cpp` | 0.5.9's `i18n.hpp` (pt/en/es table) + reconstructed `i18n_ui.hpp` reading the package's `assets/lang/<code>.txt`, hooked in by three recorded edits; fix 25 keys the interface by English, so the rebuild reads the English-keyed files of [`tools/source-1.0.2/english/lang`](../tools/source-1.0.2/fixes/README.md#language-files-fix-25) instead of the package's (keyed by Portuguese). Fix 43 adds the official loader's second load (0x1c1a50): `/app0/lang/<code>.txt` is read after the package's file into the same table, adding or replacing texts (the package has no `lang/` folder, so nothing changes until one is added) | language codes host-tested against the official tables (0x1e63e48, 0x1e63e90) and every language file parsed by `check-ui-lang-overlay.cpp` (fix 43); on the console, switching the interface language works (`1.0.3_candidate_48`) |
| `usb_kinect.hpp` (physical USB Kinect and its relay, `build/kinect-relay-20261015`) | the package's 1.0.x `virtual_depth_camera.hpp`, used by core `xboxkrnl_pscam.cc` | fix 44: the 1.0.x `virtual_depth_camera.hpp`, `factory_calibration.hpp` and `virtual_skeleton.hpp` come from the official package's `KINECT-source-and-licenses.zip` (pinned in `assemble.py`); [`usb_kinect.hpp`](../tools/source-1.0.2/reconstructed/xbox360ps5/usb_kinect.hpp) is reconstructed from the official executable (va 0x746510-0x7481e4) and the package's `kinect_tracker.c`: the relay client (127.0.0.1:9301, `KNCT`), the direct `/dev/ugen` camera (VID 045e, PID 02ae/02bf), `/data/kinect-tracker/calibration.bin`, the official 12 `Kinect USB: …` lines | relay and calibration switching checked on the host against a fake relay, the direct camera against a simulated ugen device; on the console (`1.0.3_candidate_49`, Dragon Ball Z for Kinect) the relay client received a test relay's frames at full rate and survived the relay closing; not yet tried with a real Kinect (issue #8). The `kinect-relay-20261015` folder may have held a newer `virtual_depth_camera.hpp` than the package's (the official build put it first on the include path); the official image's layout matches the package's |
| 1.0.x `canary_audio.hpp` | `canary_audio.cpp`, `launcher.cpp` | 0.5.9's, with `override` dropped from `DestroyDriver` (no longer virtual in the netplay core; it did the same as the base) | none expected |
| 1.0.2 dark home theme (`ui-dark.on`, `assets/skin-dark`) | the home and the Settings sheet | reconstructed from the official executable's code ([`dark_theme.hpp`](../tools/source-1.0.2/reconstructed/dark_theme.hpp) and recorded edits to `launcher.cpp`, `kinect_home.inc`, `kinect_state.inc`, `kinect_press.inc`): read once at start; picture folders `/data/homebrew/PPSA50011/skin`, `/app0/assets/skin-dark`, `/app0/assets/skin`; graphite panel colours (`ApplyTheme(NxeUi() && !dark)`); slot type `#eef0f2`/`#a3a9ae` instead of `#282828`/`#8c8c8c`; a "Tema: CLARO/ESCURO" slot after System Link (message, restart) and a "Tema" Settings row before "Reiniciar o emulador" (rows 40 px apart, as in the official) that remove the marker or write `1\n` to it | reconstructed after the console test of run 44497867041 found it missing; the slot's icon (`kSettings`) is our choice |
| `__wrap_sceKernelDebugOutText` | 1.0.2 link (`--wrap`) | the wrap is left out: it belongs to the OpenGL renderer (glvk, issue #6), whose code is the only reader of the driver message it saves | debug output goes to the system unchanged; nothing without glvk |
| Other title sources changed between 0.6.0 and 1.0.0 (input diagnostics, network self-test, cover download, …) | retained objects of `upstream-c28af11-20261020`, `kinect-home-20261021`, `dash-mode-20261020`, `input-fix-20261021`, … (`port.json`, `title-src/` there) | the newest published version is used, with what the official image shows rebuilt: the covers' Microsoft box-art fallback after XboxUnity (fix 41) and the network self-test's timed lines, resolver report and TCP probe (fix 42). The `InputDiag` lines of `input-fix-20261021`'s `xam_input.cc` are deliberately left out: a private Fable II diagnostic, on only when `/app0/input-diagnostic.once` holds `fable-input-20261009`, for one 30-second window | the input diagnostic is absent; other changes that left no strings may still be missing (see below) |
| Compiler commands of the retained objects | every object the author did not recompile | the flags of 0.5.9's `canary/CMakeLists.txt` (title `-O2 -mavx`, all `-march=znver2 -fno-stack-protector`) and Canary's Release `-O3` (the per-object commands are not published) | code differs byte for byte even where sources match; no behaviour change known. Tuning the flags belongs to issue #2 |

## Release fixes ported from the binary patches

The binary releases `v1.0.2-opt` and `v1.0.2-opt-r2` are the official 1.0.2 executable with
binary patches (`tools/binary-release/patches.py`; r2 has groups 1–5, and groups 6–13 were made on
branch `fix/mmio-gpr-and-ci` after it and never released in binary form). Each group is ported to
source as one patch in [`tools/source-1.0.2/fixes`](../tools/source-1.0.2/fixes/README.md),
which `assemble.py` applies last. The intent is ported, not the machine code: where the binary
patch had to borrow room (repurposed strings, an unused settings field, a diagnostic counter),
the source does it properly. [Run 44507690116](https://github.com/grzegorz2047/PS5X360Reloaded/actions/runs/44507690116)
applied all 14 patches and built `eboot.bin` (37,570,981 bytes, SHA-256 `ff26258c…`; `eboot.elf`
`5bd5b0e8…`). With the dark theme merged in,
[run 44511435853](https://github.com/grzegorz2047/PS5X360Reloaded/actions/runs/44511435853) applied them
after its edits: `eboot.bin` 37,577,765 bytes, SHA-256 `e15468f2…` (`eboot.elf` `e22e1cfb…`).
The groups were not tested one by one on the console. They are in the `1.0.3_candidate_N`
test builds run there since (candidates 19 and 30 were built without `01`, on purpose), next to
the source-only fixes 20 and up; the console results of those builds are in the
[fixes list](../tools/source-1.0.2/fixes/README.md) (for example candidate 26 for Handball 16,
29 for the save drives, 36 for Battlefield 3).

| Group | What | Status | Differences from the binary patch |
|---|---|---|---|
| 1 | JIT: no `mfence` around `ldarx`/`lwarx`/`stdcx.`/`stwcx.`; `stwcx.` reserves a word | ported (`01`) | none |
| 2 | no `clock_gettime` pair around every `mprotect` | ported (`02`) | the calls remain while GPU diagnostics are on, so their protection timings stay meaningful; off, those figures read 0 (the binary skipped them always) |
| 3 | no 256-byte XMM copy for access violations | ported (`03`) | general registers always copied (as in the binary); a handler that changes an XMM register without its state copied is passed on rather than writing zeros (no handler does today) |
| 4 | GPU command thread on its own core (CPU 2) | ported (`04`) | the dedicated core is on from the start instead of two forced branches |
| 5 | FSI choice files in `/download0/xbox360ps5` | ported (`05`) | same file names as the binary release (`fsi.c`, `fsi.1`, `fsi.c.new`), so a choice survives switching builds; the official `/app0/render-target-fsi.conf` is still read when there is no `fsi.c` |
| 6 | caches in `/data/homebrew/PPSA50011/cache` (`/data/homebrew/PPSA83600/cache` since fix 46) | ported (`06`) | `/app0/cache` stays as the second candidate (the binary replaced it) |
| 7 | Vulkan cache exit checkpoint 30 s | ported (`07`) | none |
| 8 | selective pipeline waits (RT0 overwritten, ≤ 6 indices or target ≤ 256 px) | ported (`08`) | none |
| 8b | those pipelines compile first (priority 4) | ported (`08b`) | none |
| 9 | "no new frames" notice after 2 s | ported (`09`) | none; the longer banner text is in the language files (below) |
| 10 | VSync on: `VdSwap` paced on a 16.60 ms grid | ported (`10`) | the deadline has its own variable (the binary kept it in the protection-timing counter of group 2) |
| 11 | `framegen.conf` read and written in `/download0/xbox360ps5` first | ported (`11`) | while there is no such file, the packaged `/app0/framegen.conf` is read (the binary went to `/data/homebrew/PPSA50011` directly; the packaged file says off either way) |
| 12 | third `async_shaders` choice "Ligado (experimental)": invisible stand-ins, depth-tested quads do not wait | ported (`12`) | the flag is a core cvar, `async_shader_invisible_stand_ins` (the binary used a padding byte); the debug-level "Creating graphics pipeline state" log line stays (the binary removed it for room) |
| 13 | per-game `fsi` option (Desligada / Padrão / Ligada) | ported (`13`) | a field of its own, `Settings::fsi` (the binary reused the unread `resolution_scale` and forced the draw resolution scale to 1) |
| 14 | — | not applicable | reverted in the binary release |
| lang overrides | English and Polish texts of the new and changed strings (`assets/lang-overrides`) | not applicable to the executable | they are package data: `tools/binary-release/build_package.py` merges them into the package's `assets/lang/*.txt`, which the official executable reads; for the rebuild, fix 25 carries their texts into the English-keyed files of `tools/source-1.0.2/english/lang` (with the 13 other languages of the fork's overrides) |

## Fixes made in source after the port

Fixes numbered from 20 are not binary patch groups: they are changes made in source for problems
found on the console, each with a host check. [`fixes/README.md`](../tools/source-1.0.2/fixes/README.md)
lists all of them; the ones that change how the emulator behaves when something goes wrong are
also listed here.

| Fix | What | Status |
|---|---|---|
| 45 (`45-guest-crash-notice`) | a game that crashes (Minecraft in System Link mode on the v1.0.3 draft, `research/minecraft-online-hang/VERDICT.md`) no longer leaves a silent picture at 0 FPS: the core's crash handler no longer waits forever in `XmaDecoder::Pause` (an idle XMA worker is woken; without a dedicated XMA thread, as started, there is nothing to wait for), it tells the port first (weak hook `Xbox360PS5GuestCrashed`) and then leaves its own "Uh-oh!" modal out, the overlay says the game crashed and how to close it (all interface languages), the STALL report's first line names the crashed thread and its PC, the exit skips the Vulkan cache checkpoint the paused GPU worker cannot serve, and a guest thread is not suspended while it holds the log's lock | host-checked (`tools/check-guest-crash.cpp`); candidate 51 not yet tested on a console. The game's own crash is not fixed: it is the game's code |
| 46 (`46-ps5x360reloaded-identity`) | PS5X360Reloaded has a title ID (PPSA83600), a data folder (`/data/homebrew/PPSA83600`) and a name of its own (window, guide, notifications, web settings, phone camera certificate, version label `PS5X360Reloaded v<VERSION>`), so it installs beside the official PS5X360 (PPSA50011) and never writes to the official app's folder: every path goes through `include/xbox360ps5/app_identity.hpp`, and `tools/check-title-id.py` fails on any other `PPSA50011`. The first start copies the official app's saves and small settings files once, only when the disk has room for all of it (`official_import.hpp`; the official files are only read; marker `.official-import`); settings kept in `/download0` stay with each app. The official Kinect tracker's key is accepted for its two requests (the motion status, then the motion frames) from the console itself. The save folder on drives stays `<drive>/PS5X360/saves` (fix 27) | host checks (`check-app-identity.cpp`, `check-official-import.cpp`, `check-title-id.py`); candidate 53 on the console installed beside PS5X360 and made its own folder, but read the official folder as missing (its checks used `lstat` and `realpath`) and wrote no marker; `1.0.3_candidate_54` looks with `stat`, as the drive scan does there, logs each error with its errno and writes no marker when a folder could not be checked; **the import not yet tested on a console** |

## Comparison with the official executable

[`compare.py`](../tools/source-1.0.2/compare.py) on the ELF inside each `eboot.bin` (the
official one extracted from `PPSA50011/eboot.bin`, `b1d1b3cb…`; the rebuild's `eboot.elf` of run
44497867041). Neither has section headers, so sizes are per program segment.

| | Official 1.0.2 | Rebuilt |
|---|---|---|
| `eboot.bin` | 60,204,357 B | 37,551,301 B |
| ELF | 61,544,320 B (58.69 MiB) | 38,524,304 B (36.74 MiB) |
| code (execute-only) | 26.08 MiB | 15.77 MiB |
| read-only data | 28.51 MiB | 18.12 MiB |
| data (file / in memory) | 1.35 / 6.64 MiB | 0.90 / 3.02 MiB |
| SCE dynamic-link data | 1.35 MiB | 0.95 MiB |
| printable strings (6+ chars) | 68,489 | 50,684 |
| in both | | 50,383 |
| only in the official | 18,106 | |
| only in the rebuild | | 301 |

What the 18,106 official-only strings are:

- **OpenGL stack, ~17,000**: Mesa/gallium and `opengnm-psbc` paths
  (`/home/runner/work/ps5-opengl/…`), the GLSL compiler's messages and built-in shader sources,
  GL entry point names, `glvk-*.txt` switches and `logs/OpenGL-*.log`. 8,854 match GL/GLSL/Mesa
  patterns outright; most of the other ~8,100 are the same compiler's text (ASTC/BCn decoder
  shaders, SoftFloat, lexer errors).
- **Port features from unpublished sources, about 40 log lines**: `DISC …` (games on the
  drive, 9), `Kinect USB: …` (physical camera and relay, 12; in the rebuild since fix 44),
  `Network test: …` (6; since fix 42), `InputDiag …` (2; left out on purpose, see gaps),
  `Cover {} downloaded` (since fix 41), `/app0/assets/skin-dark` and `ui-dark.on` (since the
  reconstructed dark theme), `/data/kinect-tracker/calibration.bin` (since fix 44),
  `/app0/lang/` (since fix 43). Of these, the `DISC …` and `InputDiag …` lines are still absent.
- `/app0/assets/gameinfo.txt` was also official-only in run 44497867041; the reconstructed
  reader added after it reads that file.

The 301 rebuild-only strings are build paths (`../../src102/.deps/xenia-canary/…` from
`__FILE__`), the build's own version text and reconstructed messages.

Feature markers (`compare.py`; a marker string present, not proof the feature works):

| Feature | Official | Rebuilt |
|---|---|---|
| online play (XLiveAPI, libcurl, wolfSSL, UPnP) | yes | yes |
| frame generation (ps5-framegen) | yes | yes |
| automatic small-resolve readback | yes | yes |
| phone camera (Kinect over HTTPS) | yes | yes |
| title updates / DLC manager | yes | yes |
| per-game accurate render targets (FSI) | yes | yes |
| OpenGL game renderer | yes | **no** |
| disc drive games | yes | **no** (see gaps) |
| physical USB Kinect and its relay | yes | yes since fix 44 (reconstructed client, see gaps) |

A byte-identical rebuild is not possible: the official image was linked from objects compiled
at different times with different trees and flags, and part of its sources is unpublished.

## Releases

A version built from this source is released by
[`release-source.yml`](../.github/workflows/release-source.yml) as a **draft** GitHub release;
publishing it stays a manual step, after the console test. It starts when the release commit is
pushed to a branch `release/source-X.Y.Z` (the version is the branch name's suffix, so
`release/source-1.0.3` gives `1.0.3` and the tag `v1.0.3`), or by hand on such a branch once the
workflow is on the default branch. A tag push does not start it: GitHub makes the tag when the
draft is published, at the commit the draft targets.

1. `build-source-1.0.2.yml` in release mode: the version label is `X.Y.Z` instead of
   `BUILD_VERSION`'s candidate label (`PS5X360 vX.Y.Z` on the home screen, `BOOT version X.Y.Z` in
   `boot.log`), no fix may be left out, and the linked `eboot.elf` is checked for both strings and
   for no candidate label. Artifacts: `eboot-X.Y.Z-from-source` and `source-X.Y.Z`.
2. The package: the official v1.0.2 `PPSA50011.zip` (SHA-256 `230359c2…`) with the source-built
   `eboot.bin` as it was signed, the English-keyed `assets/lang/*.txt` of fix 25 in place of the
   official Portuguese-keyed ones, this repository's `assets/presets.txt`, `assets/config` and
   `assets/patches`, `VERSION`/`RELEASE.json`/`MANIFEST.json` for `X.Y.Z`, the GPL notice and the
   licence texts the official package lacks (`tools/source-1.0.2/package.py`), then `PPSA50011.ffpkg` and `PPSA50011.ffpfsc`
   (`tools/binary-release/make_images.sh`). Because the language files change with the
   executable, the whole `PPSA50011` folder has to be installed, not `eboot.bin` alone.
3. The draft `vX.Y.Z` (notes from `docs/release-notes/vX.Y.Z.md`) with `PPSA50011.zip` (and its
   `.sha256`), `.ffpkg`, `.ffpfsc`, `SOURCES.json`, the build log, the corresponding source
   (`PS5X360-X.Y.Z-source.tar.xz`, the assembled tree that was compiled;
   `PS5X360-X.Y.Z-toolchain-source.tar.xz`, `PS5_Vulkan`, `PS5_Mesa` and `PS5_PayloadSDK` at the
   commits the build pins, whose RADV, `app_crt` and payload SDK runtime are linked in; and
   `PS5X360Reloaded-X.Y.Z-scripts.tar.gz`, this repository at the commit), the game patches
   (`assets/patches/*.patch.toml`) and `SHA256SUMS.txt` over all of them. A later push to the
   branch replaces the draft's notes, target and files while it is still a draft;
   a published release is never changed (`tools/source-1.0.2/release_guard.py`).

The released `eboot.bin` is a new build of the tested candidate's commit: only the version label
differs in the source, but its bytes (and SHA-256) are not the candidate's.

**v1.0.3** is the first release made this way (branch `release/source-1.0.3`, notes in
[`docs/release-notes/v1.0.3.md`](release-notes/v1.0.3.md)). It replaces the binary releases
`v1.0.2-opt` and `v1.0.2-opt-r2`; `release-binary.yml` and `tools/binary-release` stay only to
rebuild those (the image script `make_images.sh` is shared).

## Licensing

PS5X360's own code is MIT and Xenia is BSD-3-Clause, but the executable as a whole is
**GPL-3.0**: it links wolfSSL (GPL-3.0-or-later) and ps5-framegen (GPL-3.0-or-later), and the
author's earlier releases already shipped GPL-3.0 runtime parts. The assembled tree keeps every
licence and notice file of the drops under `licenses/` (`drop-0.5.9`, `drop-1.0.0`,
`drop-1.0.2`, `framegen-COPYING`) next to this repository's `LICENSE` and `licenses/`.

**Anyone who distributes an `eboot.bin` rebuilt this way must distribute it under the GPL-3.0,
with its complete corresponding source** (this repository's `tools/source-1.0.2`, the drops and
the pinned commits it names, and any change made on top), and keep the licence files with it.

The source release does this itself: the draft carries the assembled tree that was compiled
(`PS5X360-X.Y.Z-source.tar.xz`: a deterministic tar of `build/src102` with the drops' files, the
netplay core and its submodules, Mbed TLS, the applied fixes, `licenses/` and `SOURCES.json`)
the toolchain parts linked into it (`PS5X360-X.Y.Z-toolchain-source.tar.xz`: `PS5_Vulkan`,
`PS5_Mesa` and `PS5_PayloadSDK` at the commits `build-source-1.0.2.yml` pins) and this repository
at the release commit (`PS5X360Reloaded-X.Y.Z-scripts.tar.gz`: the assembly, build and packaging
scripts and workflows), next to the binaries, so the source is offered from the same place as
the package. The package's notice names the source tarball, the repository commit and the
toolchain pins. Archives the pinned toolchain scripts download themselves (zlib, checked by
SHA-256) are not attached. CI artifacts expire after 90 days and are not the source offer.

Artifact names tell the two kinds of build apart. A test build (dispatched by hand, or a release
run that failed a check) uploads `eboot-1.0.2-from-source-NOT-FOR-RELEASE`, labelled with
`BUILD_VERSION`'s candidate, and is for console tests only: do not distribute it. Only a release
run that passed every check uploads `eboot-X.Y.Z-from-source` and `source-X.Y.Z`, and only the
draft release made from them is meant for distribution once published.
