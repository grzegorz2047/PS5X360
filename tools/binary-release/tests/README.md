# Tests of the binary patches

These tests check what a release actually ships: `patches.py` applied to the
official v1.0.2 `eboot.bin` ELF. They run in GitHub Actions on every push
(`.github/workflows/tests.yml`) and before packaging in `release-binary.yml`.

## Run them locally

Python 3.10+ and the official v1.0.2 `PPSA50011.zip` (not in the repository):

```
curl -LO https://github.com/BrinooTk/PS5X360/releases/download/v1.0.2/PPSA50011.zip
python -m pip install -r tools/binary-release/tests/requirements.txt
PS5X360_BASE_ZIP=PPSA50011.zip python -m pytest tools/binary-release/tests
```

PowerShell: `$env:PS5X360_BASE_ZIP = "PPSA50011.zip"` before the `pytest` line.
`PS5X360_BASE_ELF=<eboot ELF>` works too, if you already took the ELF out of
`eboot.bin` (`fself.extract_fself`); the FSELF round-trip test is then skipped.
Without either variable every test is skipped. The suite takes a few seconds.

`patches.py` is not changed or wrapped: `elfpatch.record_patches` runs
`patches.apply()` under `sys.setprofile` to record each `patch_vaddr` /
`patch_cstring` call (address, expected bytes, replacement, description).

## What is checked

| File | Checks |
| --- | --- |
| `test_apply.py` | Every `expected` matches when its patch is applied; replaying the record gives `apply()`'s output; applying twice fails; no two patches write the same bytes (except the zeroed tail of a string a `patch_cstring` shortened, which patches 11-13 reuse); the output differs from the base only inside the declared ranges; ranges sit in the PT_LOAD file image at `vaddr + 0x4000`; strings are in data; the patched ELF signs and extracts back (FSELF). |
| `test_branches.py` | Capstone, with functions from `.eh_frame_hdr`: code patches decode to whole instructions and start on instruction boundaries (two operand-only patches keep their instruction); code caves are only in int3 padding between functions; every relative branch matches the table `INTENDED_BRANCHES` (target and meaning, from the comments in `patches.py`) and lands on an instruction boundary of its target patch, of the hooked function, or on a function entry; no original branch lands inside replaced code; the cut instruction after code cave 7 is unreachable; RIP-relative data is the intended cvar/flag/string (Options() blocks add exactly the new strings). |
| `test_options_unwind.py` | Patch 13's rebuilt Options() block: LSDA call-site records stay ordered and on instructions, records outside the block are unchanged, every call in the block unwinds into the r15-based cleanup, and the 46-entry vector matches both cleanup loops. |
| `test_caves.py` | Unicorn runs from the hook site on base and patched code: Settings::Apply for `async_shaders` 0..3 (experimental flag = value 2) and draw_resolution_scale = 1; IssueDrawInner's wait rule (patch 8, code caves 1/2) over readback mode, colour mask, RB_BLENDCONTROL0 destblend, index count, depth control and pitch for each `async_shaders` value; ConfigurePipeline's priority boost (patch 8b, caves 3/3b, with the real GetNormalizedDepthControl); per-game FSI at launch (cave 8) for 0..2; the 30 s Vulkan cache exit timeout; the VdSwap 60 FPS cap (caves 4-6). Registers a patch changes beyond its intended output must be dead where the code continues (or junk left by a call in v1.0.2 too). |

`emu.py` maps every PT_LOAD segment of the executable, so constants and cvars
are the real ones; only external calls (steady_clock::now, NanoSleep,
FsiWantedFor) are stubbed, and the stubs scramble the caller-saved registers.

When you add or change a patch, extend `INTENDED_BRANCHES` (and
`INTENDED_DATA` / `STRING_REFS` for new data references) in `test_branches.py`
and, for a decision cave, a truth table in `test_caves.py`.

## Other host checks in CI

`.github/workflows/tests.yml` also runs, without a console:
`server/autolog` (`npm ci && node --test`), `tools/content-installer/test_content_installer.py`
(pyftpdlib), `tools/test_deploy_verify.py`, `tools/test_inspect_game.py`,
`tools/test_mesa_weaks.py`, `tools/check-autolog.py` and `tools/check-log-export.py`
(both mock FTP).

## `tools/check-*` programs

They test C++ sources, not the shipped binary. `.github/workflows/source-checks.yml`
runs them against the v1.0.2 tree assembled from branch `source-1.0.2`; which pass,
and why the others do not, is in `tools/source-checks/README.md`.
