"""Apply a unified diff hunk by hunk over a tree that may already contain part of it.

The author's 1.0.2 core diffs overlap: online-core/commits-since-netplay-merge.diff starts at
the netplay merge (5acad1590), while 1.0.0's ps5-port-over-netplay.diff already ends at
1.0.0's HEAD (d8e0a65), so the post-merge commits up to 1.0.0 are in both. Each hunk is
applied if it applies, skipped if it is already there (reverse applies), else reported.

    python -I layer.py <tree> <diff> [--report out.json]
"""
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def split(text):
    """[(header, [hunk, ...], is_new_file)] per file of a git diff."""
    files = []
    for block in re.split(r'(?m)^(?=diff --git )', text):
        if not block.startswith('diff --git '):
            continue
        parts = re.split(r'(?m)^(?=@@ )', block)
        header, hunks = parts[0], parts[1:]
        files.append((header, hunks, '\nnew file mode' in '\n' + header))
    return files


def target(header):
    m = re.search(r'(?m)^\+\+\+ b/(.+)$', header)
    return m.group(1) if m else re.search(r'^diff --git a/(\S+)', header).group(1)


def git_apply(tree, patch, *flags):
    with tempfile.NamedTemporaryFile('w', suffix='.diff', delete=False, newline='\n', encoding='utf-8') as f:
        f.write(patch)
        name = f.name
    try:
        return subprocess.run(['git', '-C', str(tree), 'apply', '--whitespace=nowarn', *flags, name],
                              capture_output=True, text=True).returncode == 0
    finally:
        Path(name).unlink()


def new_file_content(hunks):
    lines = []
    for hunk in hunks:
        for line in hunk.splitlines(keepends=True)[1:]:
            if line.startswith('+'):
                lines.append(line[1:])
            elif line.startswith(chr(92)):  # "\ No newline at end of file"
                if lines and lines[-1].endswith('\n'):
                    lines[-1] = lines[-1][:-1]
    return ''.join(lines)


def layer(tree, diff_text):
    tree = Path(tree)
    report = {}
    for header, hunks, is_new in split(diff_text):
        path = target(header)
        if is_new and (tree / path).exists():
            # Created in both diffs: the later diff's file is the whole later file.
            (tree / path).write_text(new_file_content(hunks), encoding='utf-8', newline='')
            report[path] = {'replaced_new_file': True}
            continue
        if not hunks or git_apply(tree, header + ''.join(hunks), '--check'):
            ok = git_apply(tree, header + ''.join(hunks))
            report[path] = {'applied': len(hunks)} if ok else {'failed_whole': True}
            continue
        state = {'applied': 0, 'already': 0, 'conflict': []}
        for i, hunk in enumerate(hunks):
            patch = header + hunk
            if git_apply(tree, patch, '--check'):
                git_apply(tree, patch)
                state['applied'] += 1
            elif git_apply(tree, patch, '-R', '--check'):
                state['already'] += 1
            else:
                state['conflict'].append(i)
        report[path] = state
    return report


if __name__ == '__main__':
    tree, diff = sys.argv[1], sys.argv[2]
    result = layer(tree, Path(diff).read_text(encoding='utf-8'))
    print(json.dumps(result, indent=1))
    if '--report' in sys.argv:
        Path(sys.argv[sys.argv.index('--report') + 1]).write_text(json.dumps(result, indent=1))
    sys.exit(1 if any(v.get('conflict') or v.get('failed_whole') for v in result.values()) else 0)
