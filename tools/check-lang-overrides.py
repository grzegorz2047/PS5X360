"""Checks assets/lang-overrides/*.txt, the UI text overrides merged into the package's
assets/lang files by tools/binary-release/build_package.py.

Every language must override exactly the keys en.txt overrides (so no language falls back
to the Portuguese source text for a changed string), and every line must use the format
merge_lang and the emulator's loader read: `portuguese key = translation`, spaces written as
\\s, UTF-8 without BOM, LF line endings, no empty values.

    python3 tools/check-lang-overrides.py        (exit status 1 on any problem)
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FOLDER = ROOT / "assets/lang-overrides"


def entries(path, problems):
    raw = path.read_bytes()
    name = path.name
    if raw.startswith(b"\xef\xbb\xbf"):
        problems.append(f"{name}: starts with a UTF-8 BOM")
    if b"\r" in raw:
        problems.append(f"{name}: has CR characters (CRLF line endings)")
    if raw and not raw.endswith(b"\n"):
        problems.append(f"{name}: no newline at the end")
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        problems.append(f"{name}: not UTF-8 ({error})")
        return {}
    found = {}
    for number, line in enumerate(text.split("\n"), 1):
        if not line or line.startswith("#"):
            continue
        key, sep, value = line.partition(" = ")
        where = f"{name}:{number}"
        if not sep:
            problems.append(f"{where}: no ' = ' separator")
            continue
        if " " in key or " " in value:
            problems.append(f"{where}: a raw space (write spaces as \\s)")
        if line != line.rstrip():
            problems.append(f"{where}: trailing whitespace")
        if not value:
            problems.append(f"{where}: empty translation")
        if key in found:
            problems.append(f"{where}: key repeated (first at line {found[key][0]})")
        found[key] = (number, value)
    return found


def main():
    # The keys are Portuguese; print them on any console (Windows defaults to a code page).
    sys.stdout.reconfigure(encoding="utf-8", errors="backslashreplace")
    problems = []
    files = sorted(FOLDER.glob("*.txt"))
    if not files:
        print(f"no override files in {FOLDER}")
        return 1
    reference = entries(FOLDER / "en.txt", problems)
    if not reference:
        problems.append("en.txt: no entries")
    for path in files:
        if path.name == "en.txt":
            continue
        keys = entries(path, problems)
        for key in reference:
            if key not in keys:
                problems.append(f"{path.name}: missing the key {key[:60]}…")
        for key in keys:
            if key not in reference:
                problems.append(f"{path.name}: extra key not in en.txt {key[:60]}…")
    for problem in problems:
        print("FAIL", problem)
    print(f"{len(files)} files, {len(reference)} keys each: "
          f"{'OK' if not problems else str(len(problems)) + ' problem(s)'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
