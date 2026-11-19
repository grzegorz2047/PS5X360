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
| `20-stale-dispatch-header` | `xobject.cc` `GetNativeObject` (decisions in `native_object_resolve.h`): a guest dispatch header whose recorded handle now belongs to another object gets a new event, mutant or semaphore instead of none. A thread header in that state is the KTHREAD of a thread whose handle the game closed: the thread object is found by its guest address (`KernelState::GetThreadByGuestObject`) and used, running or finished, and the header re-stamped with its handle; only when no thread object has that address does it get an event signalled for good. Logs each stale header's types, signal state and outcome (16 lines), and each type it cannot create once (Battlefield 3, 45410950: ClientLoad joining ServerLoad) |
| `22-guide-network-mode` | the guide's settings page gets a "Rede" row (Desligada / System Link / Online) that switches the network mode while a game runs. A press never waits: while a switch runs it only sets the mode wanted next, and the switch thread then runs the last one wanted (`network_switch::Queue`, `Drain`). Each switch: the Xenia server from `network.conf` (else the public one, never the core's 192.168.0.1:36000) for System Link and Online, `XLiveAPI::SelectNetworkMode` (Live connection and link-state notifications), signed-in profiles converted to Live-enabled as at sign-in, a sign-in change notification, and `network.conf`'s mode written as the home's Online channel does. The row shows the mode being switched to ("..."), then the outcome ("Online – conectado", "Online – servidor não alcançado", "System Link", "Desligada"; English and Spanish in the built-in table). Logs a start line (UTC time) and an end line with the time taken, and SelectNetworkMode's own time. At start, System Link also uses that server. UPnP stays as at start (`port/network_switch.hpp`, `port/guide_network.hpp`, `canary_game_main.cpp`, `launcher.cpp`, `i18n.hpp`, core `user_profile.cc`) |
| `23-english-fallback` | interface text the chosen language's file lacks is shown in English (`assets/lang/en.txt`, then the built-in English table), not in the Portuguese source, which stays the last resort; Spanish (Spain and Latin America) falls back to the built-in Spanish table and Portuguese (Portugal) to the Brazilian source. `Tr()` (`i18n.hpp`), `FramegenText` (`launcher.cpp`), `EnglishUiText` (`reconstructed/xbox360ps5/i18n_ui.hpp`). The default interface language is unchanged: the console's, else English |
| `24-stall-code-words` | diagnostics. The STALL report lists a guest thread that has ended but whose object is still referenced as "exited (object still referenced)" with its exit status, and nothing else about it. For each running guest thread it logs its KTHREAD address; 64 bytes of code around `lr` and around the `ctr` target, with the `bl` calls there named after the executable's import thunks (the import table is logged once a process); its stack's back chain (each frame's address, back pointer, saved return address and first 8 words); and, for a thread in no wait, 32 bytes at each register that looks like a guest address, with the memory at r3 and r31 sampled over 0.8 s (changes only). Every kernel object a thread waits on or holds a handle to in a register gets its last 16 waits and releases (thread, operation, timeout or count, result; repeats folded) from a per-object history the core keeps (`object_history.h`, recorded in `XObject::Wait`/`WaitMultiple`/`SignalAndWait`, `XSemaphore::ReleaseSemaphore`, `XEvent::Set`/`Pulse`/`Reset`, `XMutant::ReleaseMutant`). Each guest thread's end is logged with its exit code and the UTC time, with any mutant it still owns. Guest memory is read only where the host can read it (`xe::memory::QueryProtect`, as `guest_crash_dump.hpp`). Logic in `port/stall_dump.hpp` and `xenia/kernel/object_history.h` (`canary_game_main.cpp` `ReportStall`, core `xobject`, `xsemaphore`, `xevent`, `xmutant`, `xthread`) |
| `25-english-source-text` | English is the interface's source language: every Portuguese key in the code (`Tr("…")`, the tables `Tr` is given where they are drawn, `FramegenText` calls, `CoreUiText`) is replaced by its English key, `Tr` looks up English keys (`i18n.hpp`, `reconstructed/xbox360ps5/i18n_ui.hpp`: the chosen language's file, then pt-BR.txt for Portugal's Portuguese and es.txt for Latin America's Spanish, then English), the built-in `{pt, en, es}` table, `FramegenText` and `CoreUiText`'s Portuguese and Spanish move into the language files, and Brazilian Portuguese becomes a language file (`pt-BR.txt`) like the others. Comments that quoted Portuguese text quote the English. **Generated** by `english/rekey.py`, with the language files it needs (see below); not edited by hand |
| `26-gdfx-win1252-names` | Windows-1252 file names on POSIX: `xe::win1252_to_utf8` / `utf8_to_win1252` (`string.cc`) convert with the code page's table instead of copying bytes, so a GDFX/STFS/SVOD name such as `café` is valid UTF-8 and the path code's utfcpp no longer throws `not_enough_room` ("Not enough space": Handball 16, 573607E4, never got past `DiscImageDevice::Initialize`). Guest ANSI paths (`TranslateAnsiPath`) are decoded the same way so they find those entries, and `XFile::QueryDirectory` gives the guest its cp1252 bytes back. `IsValidPath` (1.0.2: a signed `char` compared with 31 and 127, so every byte from 0x80 up gave a silent `OBJECT_NAME_INVALID`) accepts UTF-8 and still refuses control characters and `"+,<>\|`. Each failed guest path of `NtCreateFile`/`NtOpenFile`, `NtQueryFullAttributesFile`, `NtQueryDirectoryFile` and `NtOpenSymbolicLinkObject` is logged once per distinct name (at most 256): `Guest path: <call> <status>, root …, caller <LR> < … < …, N bytes "…" [hex] -> "<converted>"`, with `(empty name)` when the game passed none (`kernel/util/guest_path.h`). `DiscImageDevice::Initialize` logs `Disc mount: directory ready for … (N entries, K names from Windows-1252)`, and an exception while reading entries fails the mount with the entry count and the name's bytes. Check: `tools/check-gdfx-names.sh` (30,000-entry image; 1.0.2 must fail as on the console) `XTLGetLocaleV1` (xam 0x5DB, `XGetLocale`) gives the console's locale (`xam/xam_locale_v1.h`; 1.0.2 left r3 as it was). Diagnostic (candidate_18): `xam/call_trace.h` logs `Calls: <call>[<key>] <answer> (+N ms after the storage prompt)` for input, sign-in, content and notifications, first answers and changes only, and the STALL report's thread dump runs 20 s and 45 s after the storage prompt. Checks: `check-gdfx-names.sh`, `check-xtl-locale.cpp`, `check-call-trace.cpp` candidate_20: the system interface is held per owner (`xam/system_ui.h`: a XAM dialog, the guide), not one bool both wrote; a dialog the UI thread could not show releases it (1.0.2 left it set); each change is logged (`System UI: … -> …; the game's input is discarded/passed on`); in the minute after the storage prompt the input changes are logged with room (`call_trace::For`) and every `NtCreateFile` too (`Files:`). Check also `check-system-ui.cpp` candidate_22 (diagnostic): Handball 16 only, the save manager's flags, the device selection's globals, the save job and the title screen objects (found by vtable 820261C0) are logged 5, 20 and 45 s after the storage prompt (`WatchHandball`, `research/handball16/ANALYSIS.md`) |

## Host checks

`tools/source-1.0.2/run-fix-checks.sh <tree>` builds and runs the checks of the fixes with
the host's clang-18 against an assembled tree, as `tools/source-checks/run.py` builds the
repository's other `tools/check-*.cpp`. The source build runs them before it compiles, and
`.github/workflows/source-fix-checks.yml` runs them alone (no PS5 toolchain) on each push to
`source-1.0.2-patches`. The logic of fixes 20 and 22 lives in header-only templates the
emulator code calls, so a check drives the same code with a fake kernel or emulator.

| Check | Fix | Covers | Console only |
|---|---|---|---|
| `tools/check-stale-dispatch-header.cpp` | 20 | `xbox360ps5::native_object::Resolve` (`native_object_resolve.h`), the decisions of `XObject::GetNativeObject`: a stale event (both kinds), mutant and semaphore header gets a new object, is re-stamped, and a second lookup gives the same object with no stale report; the other object keeps its references (the lookup's is given back once) and the caller holds one; Battlefield 3's case, a running thread whose creation handle went to an event, gives the thread itself (not signalled, re-stamped with its own handle), as does a thread with no handle left or a finished thread object still alive; only with no thread object does the signalled stand-in come; unsupported types and closed handles give no object; first use and valid handles as before | the kernel side (`Create`'s type switch, `XEvent::InitializeNativeExitedThread`, `KernelState::GetThreadByGuestObject`, the log lines) and whether Battlefield 3 loads |
| `tools/check-guide-network.cpp` | 22 | `xbox360ps5::network_switch::Run` and `SignedInToLive` (used by `UserProfile::signin_state`): the mode after each switch (offline, System Link, online, online without the server), profile conversion only when online and only for profiles that are not Live-enabled, registration of the first player once, the notifications in order (Live connection, link state, the conversion's sign-out and sign-in, the sign-in re-read), `network.conf`'s mode, and the sign-in state for modes 0, 1 and 2; the queue (a press while switching is kept, intermediate modes skipped, nothing rerun when the last mode wanted is the one reached), `Drain` running System Link and then the Online pressed meanwhile, the server set for System Link too, and the row's outcome texts | the guide row and its input, `XLiveAPI::SelectNetworkMode` against the real server, the real profile conversion, and how a game (Black Ops III) reacts |
| `tools/check-english-fallback.cpp` | 23, 25 | `Tr()` with English-keyed language files in a temporary folder: Polish selected, then `en.txt`, then the English key without its `\|\|context`; English, Spanish (5; 18 through es.txt), Portuguese (9 from pt-BR.txt; 19 through pt-BR.txt), a language without a file, a text that is no key, a context key's stored English; then, in a second run, the generated `english/lang` files (Brazilian and Portugal's Portuguese, German, French, both Spanish, a key in no official file); the default interface language (the console's, else English) | the look of the screens |
| `tools/check-stall-dump.cpp` | 24 | `port/stall_dump.hpp` with fake memory: the code window (lr's word the ninth of 16), '????????' for unreadable words with no read attempted, the address space's ends, the back chain with saved return addresses and where it stops, which register values are dumped and which look like handles, the thread states (running, exited, not started), `bl` targets decoded (JobManager1's 838D16B8 -> 83ECE554), only changes kept over time; and `object_history.h`: a zero-timeout poll folded into one counted entry, releases kept after it, the ring's size and the text | whether the host can read the console's code pages, the import names, what the history shows, and what Battlefield 3's threads spin on |
| `tools/check-english-source.py` | 25 | `english/rekey.py --check` (fix 25 and the language files are what the generator makes, later fixes reversed first; the `official-lang` snapshot is the one `SOURCES.json` records); no Portuguese key or Portuguese-looking text left in the code; every English key (fix 25's and every literal passed to `Tr`, later fixes' included) in `pt-BR.txt` and the 13 full files; each file's format, printf specifiers and edge spaces; **display equivalence**: for every interface language and every text the code looks up, the old lookup (Portuguese keys, the official files merged with the fork's overrides, the built-in tables, fix 23's fallbacks) and the new one show the same text, except Portuguese that leaked into another language (now English or a translation) and Portugal's Portuguese in the core's windows (now from pt.txt) | text built at run time from fragments, and the console |
| `tools/check-gdfx-names.sh` (`.cpp`) | 26 | a 30,043-entry GDFX image with Windows-1252 names mounted by the tree's `DiscImageDevice` through `pread`: entry count, heap per entry, lookups by UTF-8 and by the game's own cp1252 path as `TranslateAnsiPath`/`IsValidPath` see it (`kernel/util/guest_path.h`), names back as cp1252, all 255 bytes round-trip and never come out empty; the `Guest path:` report (escaping, hex, `(empty name)`, once per name, 256 cap); the same check on the 1.0.2 code must fail with "Not enough space" | the kernel's `ReportGuestPath` callers and stack walk, and the game's own paths |
| `tools/check-xtl-locale.cpp` | 26 | `xam/xam_locale_v1.h`: `XTLGetLocaleV1`'s locale for US, GB, DE, FR, ES, PL, JP and RU, never past 36 for any country byte, the region fallbacks, `XamGetLocaleEx`'s choice unchanged; the export declared for xam 0x5DB in the assembled tree | what Handball 16 does with its locale |
| `tools/check-call-trace.cpp` | 26 (diagnostic) | `xam/call_trace.h`: first answers and changes only, per call and key, 24 lines a key and 600 in all, the time since the storage prompt, the input answer's START and blanking notes; in the assembled tree, the hooks in `XamInputGetState`/`GetCapabilities`/`GetKeystroke`, `XamUserGetSigninState`/`Info`, `XamContentCreate`, `XamContentGetDeviceState`, `XNotifyGetNext`, `BroadcastNotification`, the storage prompt's answer, and the thread dumps 20 s and 45 s after it | what Handball 16 does after the prompt |
| `tools/check-system-ui.cpp` | 26 | `xam/system_ui.h`: Handball 16's two dialogs before the prompt release the interface, the guide over a dialog and a dialog under the guide keep the other's hold, a failed or windowless dialog releases it, the hook hears each real change; in the assembled tree, `XamState`'s member and hook, both dispatchers' failure paths, the guide's own bit | which owner Handball 16 is held by after the prompt |

## Language files (fix 25)

The official package's `assets/lang/<code>.txt` are keyed by Brazilian Portuguese (`Configurações = Settings`),
because the official title's code is. Fix 25 keys the code by English, so the rebuilt eboot needs the
English-keyed files of `english/lang/` (`Settings = Configurações` in `pt-BR.txt`, `Settings = Einstellungen`
in `de.txt`, …, 16 files: the package's 15 and `pt-BR`). **A test package of the source
build must replace `PPSA50011/assets/lang/*.txt` with `english/lang/*.txt`** (the source build's CI artifact
carries them as `src102-out/lang/`); with the package's own files the interface would show English
everywhere. The binary release (`tools/binary-release`, the official eboot) keeps the Portuguese-keyed files
and `assets/lang-overrides`.

`english/rekey.py --tree <tree>` makes `fixes/25-english-source-text.patch` and `english/lang/` from an
assembled tree (fixes up to 24; fix 25 is reversed in a copy when applied), `english/official-lang/` (the
package's files merged with the fork's 15-language `assets/lang-overrides` as `build_package.py` does;
`--zip` and `--overrides` rebuild it; `SOURCES.json` has their SHA-256 and the snapshot files' own, and
`--check` verifies the snapshot against it even without `--zip`) and `english/extra.txt` (English
nobody wrote, the context keys of Portuguese keys whose English is the same, such as `On||feminine` for
`Ligada`, and translations of the keys in no file). `--check` fails when a committed output is out of
date. After a change to an earlier fix or to the drops that touches interface text, regenerate fix 25:
assemble without it (move it aside if it no longer applies), run `rekey.py`, and commit the patch and the
files together.

**Fixes after fix 25** (`26-…` and on) are written against the English-keyed code: new interface text is
`Tr("<English>")`, never Portuguese. `rekey.py` and `check-english-source.py` reverse them in a copy
before reversing fix 25, so fix 25 keeps being checked against the tree it was made for (fixes up to 24)
and a later fix never ends up inside it; do not regenerate fix 25 to absorb a later fix. Their new keys
are translated in `english/extra.txt`, sections `[added <code>]` (`pt-BR` and the other 14 codes, not
`en`: the key is the English), then `rekey.py` writes them into `english/lang/`.
`check-english-source.py` reads every literal the tree passes to `Tr`, so it fails until `pt-BR.txt` and
the 13 full files have each new key.

To change one: assemble a tree, commit it in a scratch repository (the title and the core
separately), edit, and regenerate the patch with `git diff` (for the core with
`--src-prefix=a/.deps/xenia-canary/ --dst-prefix=b/.deps/xenia-canary/`). A later patch may
depend on an earlier one (12 changes the code 8 and 8b add).
