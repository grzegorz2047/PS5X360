# Release fixes for the source rebuild

The shipped PS5X360 release is the official v1.0.2 `eboot.bin` with binary patches
(`tools/binary-release/patches.py` on branch `fix/mmio-gpr-and-ci`, groups 1 to 13; group 14
was reverted). Each `.patch` here is one of those groups written as a source change, so the
eboot rebuilt from the published sources behaves like the release and later fixes can be made
in C++ instead of machine code.

`assemble.py` applies them last, in name order, with `git apply` at the root of the assembled
tree: title paths as they are (`src/`, `include/`, `platform/`, `port/`), core paths under
`.deps/xenia-canary/`. `SOURCES.json` lists them under `fixes` and adds each to the `edits` of
the title files it changes. Some drop files have CRLF line endings; the patches keep them byte
for byte (`.gitattributes`: `-text`).

| Patch | Group |
|---|---|
| `01-jit-reserved-barriers` | 1: no `mfence` around `ldarx`/`lwarx`/`stdcx.`/`stwcx.`, word-sized `stwcx.` |
| `02-mprotect-clock-calls` | 2: `clock_gettime` around `mprotect` only with GPU diagnostics on |
| `03-sigsegv-xmm-copy` | 3: no XMM copy for access violations (general registers still copied) |
| `04-gpu-thread-core` | 4: GPU command thread on its own core |
| `05-fsi-files-writable` | 5: FSI choice files in `/download0/xbox360ps5` |
| `06-bulk-cache-folder` | 6: caches in `/data/homebrew/PPSA50011/cache` first |
| `07-cache-exit-checkpoint` | 7: Vulkan cache exit checkpoint waits 30 s |
| `08-selective-pipeline-wait` | 8: draws that overwrite RT0 with a full-screen quad or a small target wait for their pipeline |
| `08b-waited-pipeline-priority` | 8b: those pipelines compile first (priority 4) |
| `09-no-frames-notice` | 9: "no new frames" notice after 2 s |
| `10-vsync-swap-cap` | 10: VSync on caps `VdSwap` on a 16.60 ms grid |
| `11-framegen-conf-writable` | 11: `framegen.conf` read and written in `/download0/xbox360ps5` first |
| `12-async-shaders-experimental` | 12: third `async_shaders` choice, invisible stand-ins |
| `13-per-game-fsi` | 13: per-game `fsi` option |

Fixes made in source after the port (numbered from 20, not binary patch groups):

| Patch | Fix |
|---|---|
| `20-stale-dispatch-header` | `xobject.cc` `GetNativeObject`: a guest dispatch header whose recorded handle now belongs to another object gets a new event, mutant or semaphore instead of none; a thread header whose thread is no longer running (an exited thread) gets an event signalled for good, so waits on it end at once; a running thread that lost its handle is left alone. Logs each stale header's types, signal state and outcome (16 lines), and each type it cannot create once (Battlefield 3, 45410950, hung loading the campaign) |
| `22-guide-network-mode` | the guide's settings page gets a "Rede" row (Desligada / System Link / Online) that switches the network mode while a game runs, on a thread of its own: `XLiveAPI::SelectNetworkMode` (the netplay core's live switch, with the Live connection and link-state notifications), signed-in profiles converted to Live-enabled as at sign-in, a sign-in change notification, and `network.conf`'s mode written as the home's Online channel does. Logs `Network: switched to … while running …`. UPnP stays as at start (`port/guide_network.hpp`, `canary_game_main.cpp`, `launcher.cpp`) |
| `23-english-fallback` | interface text the chosen language's file lacks is shown in English (`assets/lang/en.txt`, then the built-in English table), not in the Portuguese source, which stays the last resort; Spanish (Spain and Latin America) falls back to the built-in Spanish table and Portuguese (Portugal) to the Brazilian source. `Tr()` (`i18n.hpp`), `FramegenText` (`launcher.cpp`), `EnglishUiText` (`reconstructed/xbox360ps5/i18n_ui.hpp`). The default interface language is unchanged: the console's, else English |
| `26-gdfx-win1252-names` | Windows-1252 file names on POSIX: `xe::win1252_to_utf8` / `utf8_to_win1252` (`string.cc`) convert with the code page's table instead of copying bytes, so a GDFX/STFS/SVOD name such as `café` is valid UTF-8 and the path code's utfcpp no longer throws `not_enough_room` ("Not enough space": Handball 16, 573607E4, never got past `DiscImageDevice::Initialize`). Guest ANSI paths (`TranslateAnsiPath`) are decoded the same way so they find those entries, and `XFile::QueryDirectory` gives the guest its cp1252 bytes back. `DiscImageDevice::Initialize` logs `Disc mount: directory ready for … (N entries, K names from Windows-1252)`, and an exception while reading entries fails the mount with the entry count and the name's bytes. Check: `tools/check-gdfx-names.sh` (30,000-entry image; 1.0.2 must fail as on the console) |

To change one: assemble a tree, commit it in a scratch repository (the title and the core
separately), edit, and regenerate the patch with `git diff` (for the core with
`--src-prefix=a/.deps/xenia-canary/ --dst-prefix=b/.deps/xenia-canary/`). A later patch may
depend on an earlier one (12 changes the code 8 and 8b add).
