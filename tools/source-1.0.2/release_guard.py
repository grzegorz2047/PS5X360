"""Decisions of the source release workflow (.github/workflows/release-source.yml).

    python3 -I tools/source-1.0.2/release_guard.py version --ref <GITHUB_REF>
    python3 -I tools/source-1.0.2/release_guard.py check-sources --sources-json <SOURCES.json> --version X.Y.Z
    python3 -I tools/source-1.0.2/release_guard.py sums <asset dir>
    python3 -I tools/source-1.0.2/release_guard.py verify <asset dir>
    python3 -I tools/source-1.0.2/release_guard.py decide --tag vX.Y.Z --sha <commit> \
        --view-exit <code> --view-stdout <file> --view-stderr <file> [--remote-tag <file>]
    python3 -I tools/source-1.0.2/release_guard.py stale <asset dir> --view-stdout <file>

version: the release version is the suffix of the branch (release/source-1.0.3 -> 1.0.3); any
other ref, a tag, or a suffix that is not X.Y.Z (1.0.3_candidate_37, 1.0.3-rc1) is refused.

check-sources: the build's SOURCES.json records that version and no fix was left out.

sums / verify: SHA256SUMS.txt over every asset of a flat folder (sorted, sha256sum's format, not
listing itself); verify checks that it lists every file exactly once and that every hash matches.
Asset names are limited to what GitHub keeps as given ([A-Za-z0-9._+-]), and each must stay under
GitHub's 2 GiB asset limit.

decide: given `gh release view <tag> --json tagName,isDraft,isImmutable,targetCommitish,assets`
(its exit code, stdout and stderr) and `git ls-remote --tags <repository>` (all tags), prints
"create" when there is no release, or "update" when the release is still a draft. It refuses
(exit 2) a published release, an answer it cannot read (anything but gh's "release not found"),
and a tag that already exists at another commit. Nothing here talks to GitHub: the workflow runs
gh and git and passes their output in.

stale: names of the assets of an existing draft that this run does not upload (printed, one per
line, for a warning; they are not deleted).
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

# Used with fullmatch only: re's $ also matches before a trailing newline.
# X.Y.Z, or X.Y.Z.W for a hotfix (1.0.4.1; W from 1 to 999), as assemble.py's RELEASE_VERSION.
VERSION_RE = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+(?:\.[1-9][0-9]{0,2})?')
BRANCH_PREFIX = 'release/source-'
SHA_RE = re.compile(r'[0-9a-f]{40}')
ASSET_NAME_RE = re.compile(r'[A-Za-z0-9._+-]+')
SUMS = 'SHA256SUMS.txt'
MAX_ASSET = 2 * 1024 ** 3  # GitHub: each release asset must be under 2 GiB
NOT_FOUND = 'release not found'


class Refused(Exception):
    """The workflow must stop: the message says why."""


def version_from_ref(ref):
    """X.Y.Z from refs/heads/release/source-X.Y.Z (or the bare branch name)."""
    ref = (ref or '').strip()
    if ref.startswith('refs/tags/'):
        raise Refused(f'{ref}: a source release is made from a release/source-X.Y.Z branch, not a tag')
    branch = ref[len('refs/heads/'):] if ref.startswith('refs/heads/') else ref
    if branch.startswith('refs/'):
        raise Refused(f'{ref}: not a branch')
    if not branch.startswith(BRANCH_PREFIX):
        raise Refused(f'{ref}: not a {BRANCH_PREFIX}X.Y.Z branch')
    version = branch[len(BRANCH_PREFIX):]
    check_version(version)
    return version


def check_version(version):
    if not isinstance(version, str) or not VERSION_RE.fullmatch(version):
        raise Refused(f'version {version!r} is not X.Y.Z or X.Y.Z.W (digits only, e.g. 1.0.3 or 1.0.4.1)')
    return version


def check_sources(report, version):
    """The build's SOURCES.json: this version, every fix applied."""
    check_version(version)
    recorded = report.get('version')
    if recorded != version:
        raise Refused(f'SOURCES.json records version {recorded!r}, not {version!r}')
    skipped = report.get('fixes_skipped')
    if skipped is None:
        raise Refused('SOURCES.json has no fixes_skipped list')
    if skipped:
        raise Refused(f'SOURCES.json: fixes left out of a release build: {skipped}')
    if not report.get('fixes'):
        raise Refused('SOURCES.json lists no applied fixes')


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def asset_files(folder):
    """The assets of a flat folder (without SHA256SUMS.txt), sorted by name; checks names and sizes."""
    folder = Path(folder)
    if not folder.is_dir():
        raise Refused(f'{folder}: no such folder')
    names = []
    for entry in sorted(folder.iterdir(), key=lambda p: p.name):
        if entry.name == SUMS:
            continue
        if entry.is_symlink() or not entry.is_file():
            raise Refused(f'{entry.name}: assets must be regular files in one flat folder')
        if not ASSET_NAME_RE.fullmatch(entry.name):
            raise Refused(f'{entry.name!r}: asset names may use only A-Z a-z 0-9 . _ + -')
        size = entry.stat().st_size
        if size == 0:
            raise Refused(f'{entry.name}: empty file')
        if size >= MAX_ASSET:
            raise Refused(f'{entry.name}: {size} bytes, over the 2 GiB GitHub asset limit')
        names.append(entry.name)
    if not names:
        raise Refused(f'{folder}: no assets')
    return names


def write_sums(folder):
    folder = Path(folder)
    lines = [f'{sha256_file(folder / name)}  {name}\n' for name in asset_files(folder)]
    (folder / SUMS).write_bytes(''.join(lines).encode('ascii'))
    return lines


def verify_sums(folder):
    folder = Path(folder)
    sums_path = folder / SUMS
    if not sums_path.is_file():
        raise Refused(f'{folder}: no {SUMS}')
    listed = {}
    try:
        text = sums_path.read_bytes().decode('ascii')
    except UnicodeDecodeError:
        raise Refused(f'{SUMS} is not ASCII')
    if not text.endswith('\n'):
        raise Refused(f'{SUMS} does not end with a newline')
    # Split on \n only (no \r, as sha256sum writes it); the text after the last \n is empty.
    for number, line in enumerate(text[:-1].split('\n'), 1):
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9._+-]+)', line)
        if not match:
            raise Refused(f'{SUMS}:{number}: not "<sha256>  <name>"')
        digest, name = match.groups()
        if name == SUMS:
            raise Refused(f'{SUMS} lists itself')
        if name in listed:
            raise Refused(f'{SUMS} lists {name} twice')
        listed[name] = digest
    present = asset_files(folder)
    missing = sorted(set(present) - set(listed))
    extra = sorted(set(listed) - set(present))
    if missing or extra:
        raise Refused(f'{SUMS} does not match the folder: not listed {missing}, no such file {extra}')
    for name in present:
        if sha256_file(folder / name) != listed[name]:
            raise Refused(f'{name}: SHA-256 differs from {SUMS}')
    return present


def remote_tag_commit(ls_remote, tag):
    """The commit a tag names in `git ls-remote --tags` output (peeled for an annotated tag), or None."""
    direct = peeled = None
    for line in (ls_remote or '').splitlines():
        parts = line.split()
        if len(parts) != 2:
            continue
        sha, ref = parts
        if ref == f'refs/tags/{tag}^{{}}':
            peeled = sha
        elif ref == f'refs/tags/{tag}':
            direct = sha
    return peeled or direct


def decide(tag, sha, view_exit, view_stdout, view_stderr, ls_remote=''):
    """'create' or 'update'; raises Refused when the release must not be touched."""
    if not tag.startswith('v'):
        raise Refused(f'tag {tag!r} does not start with v')
    check_version(tag[1:])
    if not isinstance(sha, str) or not SHA_RE.fullmatch(sha):
        raise Refused(f'{sha!r} is not a full commit SHA')
    tagged = remote_tag_commit(ls_remote, tag)
    if tagged is not None and tagged != sha:
        raise Refused(f'tag {tag} already exists at {tagged}, not at {sha}; '
                      'move or delete it by hand if that is intended')
    if view_exit != 0:
        if NOT_FOUND in (view_stderr or '').lower():
            return 'create'
        raise Refused(f'gh release view {tag} failed (exit {view_exit}): {(view_stderr or "").strip()}')
    try:
        release = json.loads(view_stdout)
    except ValueError as error:
        raise Refused(f'gh release view {tag}: unreadable answer ({error})')
    if not isinstance(release, dict) or release.get('tagName') != tag:
        raise Refused(f'gh release view {tag} answered for another release: {release!r}'[:300])
    if release.get('isDraft') is not True:
        raise Refused(f'release {tag} is published (not a draft); this workflow never changes it')
    if release.get('isImmutable') is True:
        raise Refused(f'release {tag} is immutable')
    return 'update'


def stale_assets(release, folder):
    present = set(asset_files(folder)) | {SUMS}
    return sorted(a['name'] for a in release.get('assets', []) if a.get('name') not in present)


def read(path):
    return Path(path).read_text(encoding='utf-8') if path else ''


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    p = sub.add_parser('version')
    p.add_argument('--ref', required=True)
    p = sub.add_parser('check-sources')
    p.add_argument('--sources-json', required=True, type=Path)
    p.add_argument('--version', required=True)
    for name in ('sums', 'verify'):
        sub.add_parser(name).add_argument('folder', type=Path)
    p = sub.add_parser('decide')
    p.add_argument('--tag', required=True)
    p.add_argument('--sha', required=True)
    p.add_argument('--view-exit', required=True, type=int)
    p.add_argument('--view-stdout', required=True)
    p.add_argument('--view-stderr', required=True)
    p.add_argument('--remote-tag')
    p = sub.add_parser('stale')
    p.add_argument('folder', type=Path)
    p.add_argument('--view-stdout', required=True)
    args = parser.parse_args(argv)

    try:
        if args.command == 'version':
            print(version_from_ref(args.ref))
        elif args.command == 'check-sources':
            check_sources(json.loads(args.sources_json.read_text(encoding='utf-8')), args.version)
            print(f'SOURCES.json: version {args.version}, no fix left out')
        elif args.command == 'sums':
            sys.stdout.write(''.join(write_sums(args.folder)))
        elif args.command == 'verify':
            print(f'{SUMS}: {len(verify_sums(args.folder))} assets match')
        elif args.command == 'decide':
            print(decide(args.tag, args.sha, args.view_exit, read(args.view_stdout),
                         read(args.view_stderr), read(args.remote_tag)))
        elif args.command == 'stale':
            for name in stale_assets(json.loads(read(args.view_stdout)), args.folder):
                print(name)
    except Refused as error:
        print(f'release_guard: refused: {error}', file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
