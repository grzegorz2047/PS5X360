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

To change one: assemble a tree, commit it in a scratch repository (the title and the core
separately), edit, and regenerate the patch with `git diff` (for the core with
`--src-prefix=a/.deps/xenia-canary/ --dst-prefix=b/.deps/xenia-canary/`). A later patch may
depend on an earlier one (12 changes the code 8 and 8b add).
