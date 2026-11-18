"""Builds the optimized PPSA50011 package from the official PS5X360 v1.0.2 release.

The C++ in this repository is older than the v1.0.2 executable (see README.md),
so a release keeps the official v1.0.2 eboot.bin, applies the binary patches in
patches.py, signs it again and replaces the settings files this repository
maintains (assets/presets.txt, assets/config/*.toml, assets/patches/*.patch.toml).

    python3 tools/binary-release/build_package.py --base-zip PPSA50011-v1.0.2.zip

Writes <out-dir>/PPSA50011.zip, <out-dir>/PPSA50011.zip.sha256 and the unpacked
folder <out-dir>/stage/PPSA50011 that the .ffpkg/.ffpfsc images are made from.
"""
import argparse
import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path

import fself
import patches

ROOT = Path(__file__).resolve().parents[2]
PREFIX = "PPSA50011/"
# The official v1.0.2 PPSA50011.zip (github.com/BrinooTk/PS5X360/releases/tag/v1.0.2).
BASE_ZIP_SHA256 = "230359c2638f99add3972147e1690e62d8984a37c00181501680bdda7c1096c8"
BASE_VERSION = "1.0.2"
UPSTREAM = "https://github.com/BrinooTk/PS5X360/releases"
# Entries written by this script get a fixed time, so a rebuild of the same
# inputs gives the same zip.
ZIP_TIME = (2026, 10, 22, 20, 26, 0)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def notice(version: str, source: str, eboot_sha: str) -> bytes:
    return f"""PS5X360 {version} - optimized build of the official PS5X360 {BASE_VERSION} package

What was changed
    eboot.bin is the official {BASE_VERSION} executable with the binary patches listed in
    tools/binary-release/patches.py of {source}, signed again (sha256 {eboot_sha}).
    assets/presets.txt, assets/config/*.toml and assets/patches/*.patch.toml come from
    the same repository. Every other file is the official {BASE_VERSION} package unchanged.

Source code
    The source of the official {BASE_VERSION} executable is published by its author with
    the PS5X360 releases ({UPSTREAM}): the source-*-changes.zip assets of
    v0.6.0, v1.0.0 and v1.0.2 and the full source archives of the earlier releases.
    The modifications in this build, and the script that makes it, are in {source}
    (tools/binary-release). Licences of the parts in eboot.bin are in this folder.
""".encode()


def main() -> int:
    if sys.flags.optimize:
        raise SystemExit("Run without -O: the patch checks use assert")
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-zip", type=Path, required=True, help="official v1.0.2 PPSA50011.zip")
    parser.add_argument("--out-dir", type=Path, default=ROOT / "dist/binary-release")
    parser.add_argument("--version", default=BASE_VERSION + "-opt", help="version written to RELEASE.json")
    parser.add_argument("--source", default="the PS5X360 fork this script belongs to",
                        help="where this build's source is (repository URL and commit)")
    args = parser.parse_args()

    base = args.base_zip.read_bytes()
    if sha256(base) != BASE_ZIP_SHA256:
        raise SystemExit(f"{args.base_zip} is not the official v{BASE_VERSION} PPSA50011.zip")
    with zipfile.ZipFile(args.base_zip) as zin:
        files = {i.filename: zin.read(i) for i in zin.infolist() if not i.is_dir()}
    if files.get(PREFIX + "VERSION") != (BASE_VERSION + "\n").encode():
        raise SystemExit("Base package VERSION is not " + BASE_VERSION)

    original = files[PREFIX + "eboot.bin"]
    elf = fself.extract_fself(original)
    if sha256(elf) != patches.BASE_ELF_SHA256:
        raise SystemExit("Base eboot.bin ELF differs from the one patches.py was written for")
    if fself.sign_from_template(elf, original) != original:
        raise SystemExit("FSELF round trip of the unmodified eboot.bin is not exact")
    print("[OK] exact FSELF round trip of the official eboot.bin")
    eboot = fself.sign_from_template(patches.apply(elf), original)
    eboot_sha = sha256(eboot)
    print(f"[OK] patched eboot.bin signed, sha256 {eboot_sha}")

    files[PREFIX + "eboot.bin"] = eboot
    files[PREFIX + "assets/presets.txt"] = (ROOT / "assets/presets.txt").read_bytes()
    for path in sorted((ROOT / "assets/config").glob("*.toml")):
        files[PREFIX + "assets/config/" + path.name] = path.read_bytes()
    for path in sorted((ROOT / "assets/patches").glob("*.patch.toml")):
        files[PREFIX + "assets/patches/" + path.name] = path.read_bytes()
    # v1.0.2 keeps resolves up to 512 KiB copied back when readback is off; this
    # marker would switch that off (it darkens Battlefield: Bad Company).
    files.pop(PREFIX + "readback-auto.off", None)
    files[PREFIX + "licenses/PS5X360-OPT-NOTICE.txt"] = notice(args.version, args.source, eboot_sha)
    files[PREFIX + "RELEASE.json"] = (json.dumps({
        "version": args.version,
        "executable_sha256": eboot_sha,
        "candidate": False,
        "published": True,
        "candidate_id": f"opt-{eboot_sha[:8]}",
        "base_candidate": "online-phone-folder-b1d1b3cb",
    }, indent=2) + "\n").encode()
    files.pop(PREFIX + "MANIFEST.json", None)
    manifest = {name[len(PREFIX):]: {"bytes": len(data), "sha256": sha256(data)}
                for name, data in sorted(files.items())}
    files[PREFIX + "MANIFEST.json"] = (json.dumps(manifest, indent=2) + "\n").encode()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    out_zip = args.out_dir / "PPSA50011.zip"
    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as zout:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            zout.writestr(info, data)
    with zipfile.ZipFile(out_zip) as zcheck:
        if zcheck.testzip() is not None:
            raise SystemExit("ZIP CRC check failed")
        for name, record in manifest.items():
            data = zcheck.read(PREFIX + name)
            if len(data) != record["bytes"] or sha256(data) != record["sha256"]:
                raise SystemExit(f"MANIFEST check failed: {name}")
        stage = args.out_dir / "stage"
        shutil.rmtree(stage, ignore_errors=True)
        zcheck.extractall(stage)
    digest = sha256(out_zip.read_bytes())
    (args.out_dir / "PPSA50011.zip.sha256").write_text(f"{digest}  PPSA50011.zip\n", encoding="utf-8")
    print(json.dumps({"version": args.version, "package": str(out_zip), "files": len(files),
                      "bytes": out_zip.stat().st_size, "sha256": digest, "eboot_sha256": eboot_sha}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
