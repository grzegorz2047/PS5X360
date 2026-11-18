# Optimized release from the official v1.0.2 package

The C++ in this repository stops at v0.5.7-fix.1: the official v1.0.2 executable
was built from code its author publishes only as archives with the releases
(`PS5X360-source-*-changes.zip`). An `eboot.bin` compiled from this repository
lacks online play, per-game FSI, frame generation and Kinect, so it is **not**
released. A release instead takes the official v1.0.2 package and changes it:

| Step | Where |
| --- | --- |
| Check the official `PPSA50011.zip` (SHA-256) and its `eboot.bin` ELF | `build_package.py` |
| Patch the ELF (each patch checks the bytes it replaces) and sign it again | `patches.py`, `fself.py` |
| Replace `assets/presets.txt`, `assets/config/*.toml`, `assets/patches/*.patch.toml` from this repository | `build_package.py` |
| Write `RELEASE.json`, `MANIFEST.json`, `licenses/PS5X360-OPT-NOTICE.txt` | `build_package.py` |
| Make `PPSA50011.ffpkg` (UFS2Tool) and `PPSA50011.ffpfsc` (MkPFS, verified) | `make_images.sh` |

GitHub Actions runs it in `.github/workflows/release-binary.yml`: by hand
(*Run workflow*, artifacts only) or by pushing a tag `v*-opt*`, which also
publishes the GitHub release.

Locally (Python 3, plus the .NET 8 SDK for the images):

```
curl -LO https://github.com/BrinooTk/PS5X360/releases/download/v1.0.2/PPSA50011.zip
python3 tools/binary-release/build_package.py --base-zip PPSA50011.zip
bash tools/binary-release/make_images.sh dist/binary-release/stage/PPSA50011 dist/binary-release
```

A change to the C++ reaches a release only when it can be written as a binary
patch in `patches.py`; `.github/workflows/build-ps5.yml` only checks that the
repository's C++ still compiles.
