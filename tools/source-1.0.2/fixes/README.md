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
| `tools/check-english-fallback.cpp` | 23 | `Tr()` with language files in a temporary folder: Polish selected, then `en.txt`, then the built-in English, then the Portuguese source; English, Spanish (5, 18), Portuguese (9, 19) and a language without a file; the default interface language (the console's, else English) | `FramegenText` and the look of the screens |
| `tools/check-stall-dump.cpp` | 24 | `port/stall_dump.hpp` with fake memory: the code window (lr's word the ninth of 16), '????????' for unreadable words with no read attempted, the address space's ends, the back chain with saved return addresses and where it stops, which register values are dumped and which look like handles, the thread states (running, exited, not started), `bl` targets decoded (JobManager1's 838D16B8 -> 83ECE554), only changes kept over time; and `object_history.h`: a zero-timeout poll folded into one counted entry, releases kept after it, the ring's size and the text | whether the host can read the console's code pages, the import names, what the history shows, and what Battlefield 3's threads spin on |

To change one: assemble a tree, commit it in a scratch repository (the title and the core
separately), edit, and regenerate the patch with `git diff` (for the core with
`--src-prefix=a/.deps/xenia-canary/ --dst-prefix=b/.deps/xenia-canary/`). A later patch may
depend on an earlier one (12 changes the code 8 and 8b add).
