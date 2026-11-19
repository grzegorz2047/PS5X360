"""Runs the repository's host checks (tools/check-* and src/memory_contract_test.cpp)
against an assembled PS5X360 v1.0.2 source tree.

    python3 tools/source-1.0.2/assemble.py --out build/src102        (on branch source-1.0.2)
    python3 tools/source-checks/run.py --tree build/src102 --repo <source-1.0.2 checkout>

The checks find their sources relative to their own folder (root = tools/..), so the
repository's tools/ files the tree lacks are copied into <tree>/tools first and each check
runs from there. A .cpp check without its own script is compiled with clang++-18 against
the tree's include/ and reconstructed/ headers and run.

Writes <report> (JSON: name -> status, seconds, log) and a Markdown table to stdout (and to
$GITHUB_STEP_SUMMARY when set). With --expect FILE, exits 1 when a check listed there does
not pass; without it, it only reports.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

CLANG = os.environ.get("CXX_HOST", "clang++-18")
# The core is built for the PS5's Zen 2 (AVX2); its headers use those intrinsics.
ARCH = "-march=znver2"
# Checks that only talk to real systems; never run.
NEVER = {
    "check-native-autolog-console.py": "reads the development PS5 over FTP",
    "check-relay-live.py": "sends a report to the configured live relay",
}
# .cpp checks that their .sh script builds.
BUILT_BY_SCRIPT = {"check-thread-exit.cpp": "check-thread-exit.sh"}


def copy_missing(src: Path, dest: Path):
    for item in src.rglob("*"):
        if item.is_file():
            target = dest / item.relative_to(src)
            if not target.exists():
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(item, target)


def command_for(tree: Path, name: str, out: Path):
    path = f"tools/{name}"
    if name.endswith(".py"):
        return [sys.executable, path]
    if name.endswith(".sh"):
        return ["bash", path]
    exe = out / name[:-4]
    includes = ["-Iinclude", "-Ireconstructed", "-I.deps/xenia-canary/src", "-I.deps/xenia-canary",
                "-I.deps/xenia-canary/third_party/fmt/include"]
    return ["bash", "-c",
            f'{CLANG} -std=c++20 -O1 -g -pthread {ARCH} {" ".join(includes)} {path} -o "{exe}" && "{exe}"']


def memory_contract(tree: Path, out: Path):
    """src/memory_contract_test.cpp against the core's POSIX memory code."""
    core = ".deps/xenia-canary/src/xenia/base"
    exe = out / "memory_contract_test"
    # The whole of xenia/base for Linux (logging, cvars and CPU features come with memory.cc).
    base = (f"$(ls {core}/*.cc | grep -vE '_(win|android|mac|arm64|gnulinux_x11|test[a-z_]*)\\.cc$|/main_|/testing')")
    includes = ["-Iinclude", "-Ireconstructed", "-I.deps/xenia-canary/src", "-I.deps/xenia-canary",
                "-I.deps/xenia-canary/third_party/fmt/include", "-I.deps/xenia-canary/third_party/utfcpp/source",
                "-I.deps/xenia-canary/third_party"]
    return ["bash", "-c",
            f'{CLANG} -std=c++20 -O1 -g -pthread {ARCH} -DFMT_HEADER_ONLY {" ".join(includes)} '
            f'src/memory_contract_test.cpp {base} -ldl -o "{exe}" && "{exe}"']


def run(cmd, cwd, timeout):
    start = time.time()
    try:
        p = subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, errors="replace", timeout=timeout)
        status = "pass" if p.returncode == 0 else "fail"
        log = p.stdout
    except subprocess.TimeoutExpired as e:
        status, log = "timeout", (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
    return status, round(time.time() - start, 1), log


def first_error(log: str) -> str:
    lines = [l.strip() for l in log.splitlines() if l.strip()]
    for key in ("error:", "Error", "Traceback", "FAIL", "undefined reference", "No such file", "ValueError", "assert"):
        for line in lines:
            if key in line:
                return line[:200]
    return lines[-1][:200] if lines else ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tree", type=Path, required=True)
    ap.add_argument("--repo", type=Path, required=True, help="checkout whose tools/ and src/ are used")
    ap.add_argument("--report", type=Path, default=Path("source-checks.json"))
    ap.add_argument("--expect", type=Path)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args()
    tree, repo = args.tree.resolve(), args.repo.resolve()

    copy_missing(repo / "tools", tree / "tools")
    if not (tree / "src/memory_contract_test.cpp").exists():
        shutil.copy2(repo / "src/memory_contract_test.cpp", tree / "src/memory_contract_test.cpp")
    # The 1.0.2 tree keeps Mbed TLS at mbedtls/; check-native-autolog.sh looks in .deps/autolog.
    autolog_tls = tree / ".deps/autolog/mbedtls-3.6.7"
    if (tree / "mbedtls").is_dir() and not autolog_tls.exists():
        autolog_tls.parent.mkdir(parents=True, exist_ok=True)
        autolog_tls.symlink_to(tree / "mbedtls", target_is_directory=True)
    out = tree / "build/source-checks"
    out.mkdir(parents=True, exist_ok=True)

    names = sorted(p.name for p in (repo / "tools").glob("check-*")) + ["memory_contract_test.cpp"]
    if args.only:
        names = [n for n in names if n in args.only]
    results = {}
    for name in names:
        if name in NEVER:
            results[name] = {"status": "not run", "seconds": 0, "log": NEVER[name], "reason": NEVER[name]}
            continue
        if name in BUILT_BY_SCRIPT:
            results[name] = {"status": "see " + BUILT_BY_SCRIPT[name], "seconds": 0, "log": "", "reason": ""}
            continue
        cmd = memory_contract(tree, out) if name == "memory_contract_test.cpp" else command_for(tree, name, out)
        status, seconds, log = run(cmd, tree, args.timeout)
        results[name] = {"status": status, "seconds": seconds, "log": log[-20000:],
                         "reason": "" if status == "pass" else first_error(log)}
        print(f"{status:8s} {seconds:7.1f}s  {name}  {results[name]['reason']}", flush=True)

    args.report.write_text(json.dumps(results, indent=2))
    table = ["| Check | Result | Time | First error |", "| --- | --- | --- | --- |"]
    for name, r in results.items():
        reason = r["reason"].replace("|", "\\|")
        table.append(f"| `{name}` | {r['status']} | {r['seconds']} s | {reason} |")
    text = "\n".join(table) + "\n"
    print(text)
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as f:
            f.write("## Host checks on the assembled v1.0.2 tree\n\n" + text)

    if args.expect:
        expected = [l.split("#")[0].strip() for l in args.expect.read_text().splitlines()]
        broken = [n for n in expected if n and results.get(n, {}).get("status") != "pass"]
        if broken:
            print("expected to pass but did not: " + ", ".join(broken))
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
