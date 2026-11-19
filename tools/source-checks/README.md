# Host checks on the assembled v1.0.2 source tree

`.github/workflows/source-checks.yml` checks out branch `source-1.0.2`, assembles the
v1.0.2 tree with `tools/source-1.0.2/assemble.py` (drops and core clone cached), and runs
`run.py`: every `tools/check-*` program and `src/memory_contract_test.cpp`, built with
the runner's clang-18 against the tree (`-march=znver2`, the core's include roots). No PS5
toolchain or RADV build is needed. The job fails when a check in `expected-pass.txt`
stops passing. The others still run, and the job summary and the
`source-checks-report` artifact (JSON with each log) show their results.

Locally (Linux, clang-18, zbar-tools, pyftpdlib):

```
git worktree add ../src102 source-1.0.2
python3 -I ../src102/tools/source-1.0.2/assemble.py --out build/src102 --cache ../src102/.deps/src102-cache
python3 tools/source-checks/run.py --tree build/src102 --repo ../src102 --expect tools/source-checks/expected-pass.txt
```

## Results (source-1.0.2 at 3547617)

Pass (16, required): `check-autolog.py`, `check-frame-watch.cpp`, `check-game-storage.cpp`,
`check-gpu-diagnostics.cpp`, `check-guest-dump-range.py`, `check-log-export.py`,
`check-protection-batching.py`, `check-ps5-mutex.py`, `check-qr-code.sh`,
`check-readback-barrier.py`, `check-save-storage.cpp`, `check-virtual-depth.cpp`,
`check-virtual-skeleton.cpp`, `check-vsync-pacing.py`, `check-watch-batching.py`,
`check-watch-limit.py`.

Do not pass, and why:

| Check | Reason |
| --- | --- |
| `memory_contract_test.cpp` | Builds and runs (with the core's `xenia/base`): 25 of 39 contract cases pass. The 1.0.2 core's `memory_posix.cc` / `mapped_memory_posix.cc` lack the guarantees the test asks for (uncommitted reserve, query/protect boundaries, release and file-mapping checks). A real difference, worth a look. |
| `check-ui-language.cpp` | Builds; assertion `SupportedUiLanguage(ConsoleGameLanguage(22)) == 1` fails. 1.0.2's language mapping differs from what the check expects. A real difference, worth a look. |
| `check-invalidation-window.py`, `check-shared-memory-cache.py`, `check-unwatched-pages.py` | Test the repository's own `shared_memory.cc` changes (`keep_enabled`, `kept_pending`, `kept_count`); the 1.0.2 core does not have them. |
| `check-multiplayer.cpp` | `DualSenseInput` is abstract in 1.0.2 (the check is written for the older interface). |
| `check-motion.py`, `check-web-settings.py` | 1.0.2's `platform/ps5/web_settings.cpp` includes `phone_camera.hpp`, which the checks' include paths do not cover (it comes from the 1.0.2 port files). |
| `check-autolog-ftp.py` | 1.0.2's `native/autolog` has no `local_files.h`. |
| `check-native-autolog.sh` | Needs the repository's `mbedtls_user_config.h` for AutoLog; 1.0.2's tree has Mbed TLS but not that config. |
| `check-ldi.cpp` | Needs the production LDI decoder and libmspack linked; they come from the Canary host build. |
| `check-mapped-slice.sh`, `check-thread-exit.sh` (+ `.cpp`) | Link against `build/canary-runner/obj/Linux/libxenia-base.a`, the Canary host build. |
| `check-native-autolog-console.py`, `check-relay-live.py` | Talk to the development PS5 / the live relay; never run. |
