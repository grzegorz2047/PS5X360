"""Host check of the core commit build.sh writes into version.h (tools/source-1.0.2/core_commit.py).

    python3 -I tools/check-core-commit.py

- core_commit() takes the 40-hex commit at the end of SOURCES.json's core.base and refuses a
  missing core, a missing or blank base, a short, upper-case or trailing-newline commit;
- the command line prints the commit, and exits 1 on a missing file, unreadable JSON or a bad
  base, 2 on wrong arguments;
- build.sh's "core commit" block, run with bash: a tree with .deps/xenia-canary/.git gives git's
  HEAD, an unpacked source tarball (no .git) gives core.base, and a tarball whose SOURCES.json has
  no usable commit stops the build. The bash part is skipped with a notice when no bash and git
  are found (CI has them).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / 'tools/source-1.0.2/core_commit.py'
BUILD = REPO / 'tools/source-1.0.2/build.sh'
sys.dont_write_bytecode = True
sys.path.insert(0, str(SCRIPT.parent))
import core_commit as cc  # noqa: E402

BASE = 'd2cf527aa2bcc7e6720aeeaab9e06bcc231f67fa'
URL = 'https://github.com/AdrianCassar/xenia-canary.git'


def sources(base):
    return {'core': {'base': base, 'changed_files': 1}, 'fixes': []}


def check_function():
    assert cc.core_commit(sources(f'{URL} {BASE}')) == BASE
    assert cc.core_commit(sources(BASE)) == BASE
    for bad in ({}, {'core': None}, {'core': {}}, sources(None), sources(''), sources('   '),
                sources(f'{URL} {BASE[:39]}'), sources(f'{URL} {BASE.upper()}'),
                sources(f'{URL} {BASE}x'), sources(URL), sources(7), [], None):
        try:
            cc.core_commit(bad)
        except ValueError:
            continue
        raise AssertionError(f'accepted {bad!r}')
    # str.split() drops a trailing newline, so a base ending in "\n" still gives the commit, but a
    # commit with a newline inside it does not pass.
    try:
        cc.core_commit(sources(f'{URL} {BASE[:20]}\n{BASE[20:]}'))
    except ValueError:
        pass
    else:
        raise AssertionError('accepted a split commit')
    print('  core_commit: base parsed, malformed bases refused')


def run_cli(*args):
    return subprocess.run([sys.executable, '-I', str(SCRIPT), *args], capture_output=True, text=True)


def check_cli(scratch):
    good = scratch / 'good.json'
    good.write_text(json.dumps(sources(f'{URL} {BASE}')), encoding='utf-8')
    result = run_cli(str(good))
    assert result.returncode == 0 and result.stdout == BASE + '\n', result
    bad = scratch / 'bad.json'
    bad.write_text(json.dumps(sources(URL)), encoding='utf-8')
    broken = scratch / 'broken.json'
    broken.write_text('{"core": ', encoding='utf-8')
    for path in (bad, broken, scratch / 'missing.json'):
        result = run_cli(str(path))
        assert result.returncode == 1 and result.stdout == '' and 'core_commit:' in result.stderr, (path, result)
    assert run_cli().returncode == 2 and run_cli(str(good), str(good)).returncode == 2
    print('  command line: prints the commit, exit 1 on a bad SOURCES.json, 2 on wrong arguments')


def block(text):
    lines = text.splitlines()
    begin = [i for i, line in enumerate(lines) if line.startswith('# core commit (begin)')]
    end = [i for i, line in enumerate(lines) if line.startswith('# core commit (end)')]
    assert len(begin) == 1 and len(end) == 1 and begin[0] < end[0], 'build.sh core commit markers'
    body = '\n'.join(lines[begin[0]:end[0] + 1])
    assert 'core_commit.py' in body and '.deps/xenia-canary/.git' in body and 'rev-parse HEAD' in body
    # version.h is written from this commit right after the block.
    after = '\n'.join(lines[end[0] + 1:end[0] + 6])
    assert 'version.h' in after and 'XE_BUILD_COMMIT "$commit"' in '\n'.join(lines[end[0]:end[0] + 10])
    return body


def find_bash():
    candidates = [shutil.which('bash')]
    if os.name == 'nt':  # Git for Windows' bash, not WSL's C:\Windows\System32\bash.exe
        candidates = [r'C:\Program Files\Git\bin\bash.exe', r'C:\Program Files\Git\usr\bin\bash.exe'] + \
                     [c for c in candidates if c and 'system32' not in c.lower()]
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            probe = subprocess.run([candidate, '-c', 'git --version && python3 -c "print(1)"'],
                                   capture_output=True, text=True)
            if probe.returncode == 0:
                return candidate
    return None


def run_block(bash, body, tree):
    script = 'set -euo pipefail\n' + body + '\necho "commit=$commit"\n'
    env = dict(os.environ, repo=REPO.as_posix(), tree=tree.as_posix(),
               GIT_CEILING_DIRECTORIES=tree.parent.as_posix())
    return subprocess.run([bash, '-c', script], env=env, capture_output=True, text=True)


def git(cwd, *args):
    subprocess.run(['git', '-C', str(cwd), '-c', 'user.name=t', '-c', 'user.email=t@t', '-c',
                    'commit.gpgsign=false', *args], check=True, capture_output=True)
    return subprocess.run(['git', '-C', str(cwd), 'rev-parse', 'HEAD'], capture_output=True,
                          text=True).stdout.strip()


def check_build_block(scratch):
    body = block(BUILD.read_text(encoding='utf-8'))
    bash = find_bash()
    if not bash:
        print('  SKIP build.sh block: no bash with git and python3 found (CI has them)')
        return
    # An unpacked source tarball: SOURCES.json, the core without .git.
    tarball = scratch / 'tarball/PS5X360-1.0.3-source'
    (tarball / '.deps/xenia-canary/src').mkdir(parents=True)
    (tarball / 'SOURCES.json').write_text(json.dumps(sources(f'{URL} {BASE}')), encoding='utf-8')
    result = run_block(bash, body, tarball)
    assert result.returncode == 0 and result.stdout.splitlines()[-1] == f'commit={BASE}', result
    # The same tarball with a SOURCES.json that names no commit stops the build.
    (tarball / 'SOURCES.json').write_text(json.dumps(sources(URL)), encoding='utf-8')
    result = run_block(bash, body, tarball)
    assert result.returncode != 0 and 'commit=' not in result.stdout, result
    (tarball / 'SOURCES.json').unlink()
    result = run_block(bash, body, tarball)
    assert result.returncode != 0 and 'commit=' not in result.stdout, result
    # An assembled tree: git's HEAD wins over SOURCES.json (which names another commit here).
    tree = scratch / 'assembled/src102'
    core = tree / '.deps/xenia-canary'
    core.mkdir(parents=True)
    (core / 'a.cc').write_text('a\n', encoding='utf-8')
    git(core, 'init', '-q')
    git(core, 'add', 'a.cc')
    head = git(core, 'commit', '-q', '-m', 'a')
    assert len(head) == 40 and head != BASE
    (tree / 'SOURCES.json').write_text(json.dumps(sources(f'{URL} {BASE}')), encoding='utf-8')
    result = run_block(bash, body, tree)
    assert result.returncode == 0 and result.stdout.splitlines()[-1] == f'commit={head}', result
    print('  build.sh block: git HEAD in an assembled tree, core.base in a tarball, a bad base stops')


def main():
    check_function()
    with tempfile.TemporaryDirectory() as scratch:
        check_cli(Path(scratch))
        check_build_block(Path(scratch))
    print('PASS: build.sh takes the core commit from git, or from SOURCES.json in the source tarball')


if __name__ == '__main__':
    main()
