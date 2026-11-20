"""The Xenia Canary commit an assembled tree's core is based on, from its SOURCES.json.

    python3 -I tools/source-1.0.2/core_commit.py <tree>/SOURCES.json

build.sh writes this commit into version.h (XE_BUILD_COMMIT). In a tree assembled here it reads
it with git from .deps/xenia-canary; the GPL source tarball (PS5X360-X.Y.Z-source.tar.xz) has no
.git, so build.sh asks this script instead. assemble.py records the core as
core.base = "<netplay repository URL> <40-hex commit>", and the fixes are applied to that commit
without committing, so it is the same HEAD git would report.
"""
import json
import re
import sys

COMMIT_RE = re.compile(r'[0-9a-f]{40}')


def core_commit(sources):
    """The 40-hex commit at the end of sources['core']['base']; ValueError if there is none."""
    core = sources.get('core') if isinstance(sources, dict) else None
    base = core.get('base') if isinstance(core, dict) else None
    if not isinstance(base, str) or not base.split():
        raise ValueError('SOURCES.json has no core.base')
    commit = base.split()[-1]
    if not COMMIT_RE.fullmatch(commit):
        raise ValueError(f'core.base {base!r} does not end in a 40-hex commit')
    return commit


def main(argv):
    if len(argv) != 2:
        print('usage: core_commit.py <tree>/SOURCES.json', file=sys.stderr)
        return 2
    try:
        with open(argv[1], encoding='utf-8') as f:
            commit = core_commit(json.load(f))
    except (OSError, ValueError) as error:
        print(f'core_commit: {error}', file=sys.stderr)
        return 1
    print(commit)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
