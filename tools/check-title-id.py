"""Host check of fix 46 (PS5X360Reloaded's own title ID, folder and name) on an assembled tree.

    python3 -I tools/check-title-id.py --tree build/src102

PS5X360Reloaded is PPSA83600 and keeps its data in /data/homebrew/PPSA83600; the official PS5X360
(PPSA50011) stays installed beside it and its folder is never written. Checks, on the title's code
(include/, platform/, src/, port/, reconstructed/, canary/) and the core's (.deps/xenia-canary/src,
when the tree has the core):
  (a) "PPSA50011" appears nowhere but in ALLOWED (one line: the official title ID in
      include/xbox360ps5/app_identity.hpp), and each ALLOWED line is there;
  (b) app_identity.hpp: the title ID is PPSA plus five digits and not the official one, the data
      root is "/data/homebrew/" + the title ID, the product name is PS5X360Reloaded; each of its
      macros is defined once in the whole tree;
  (c) no string literal names a /data/homebrew folder outside app_identity.hpp (every path goes
      through XBOX360PS5_DATA_ROOT), and the official folder (kOfficialDataRoot) is used only by
      READERS: the first start's import and the official Kinect tracker's key;
  (d) the product name: each RENAMED text is gone and its new form is there (window, log, guide,
      notifications, web pages, phone camera, version label, User-Agent); the phone camera's
      instructions name the certificate the title makes; KEPT texts are still there (the save
      folder on drives, fix 27; the session log's header, which tools/autolog.py reads);
  (e) the import runs before the saves are prepared and before the settings are read;
  (f) english/lang: no "PPSA50011", and no key that fix 46 renames (english/rekey.py RENAMES).
"""
import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGLISH = REPO / 'tools/source-1.0.2/english'
sys.path.insert(0, str(ENGLISH))
import rekey  # noqa: E402  (its C/C++ lexer and RENAMES)

OFFICIAL_ID = 'PPSA50011'
TITLE_DIRS = ('include', 'platform', 'src', 'port', 'reconstructed', 'canary')
CORE_DIR = '.deps/xenia-canary/src'
SUFFIXES = ('.cpp', '.cc', '.c', '.h', '.hpp', '.inc', '.txt', '.cmake', '.md')
IDENTITY = 'include/xbox360ps5/app_identity.hpp'
# (file, the whole line, stripped) -> why the official title ID may be there.
ALLOWED = {
    (IDENTITY, '#define XBOX360PS5_OFFICIAL_TITLE_ID "PPSA50011"'):
        'the official PS5X360\'s title ID, named once: its folder is read (import, Kinect key), never written',
}
# Lines that may use the official folder (kOfficialDataRoot), each for reading only.
READERS = {
    ('src/canary_game_main.cpp',
     'const auto imported = xbox360ps5::official_import::Run(xbox360ps5::app_identity::kOfficialDataRoot,'):
        'the first start\'s import: its folder is the source, only read (official_import.hpp)',
    ('platform/ps5/web_settings.cpp',
     'official_key = web_key::Read(fs::path(app_identity::kOfficialDataRoot) / "web-settings.key");'):
        'the official Kinect tracker\'s key, read (web_key.hpp)',
}
MACROS = ('XBOX360PS5_PRODUCT_NAME', 'XBOX360PS5_TITLE_ID', 'XBOX360PS5_DATA_ROOT', 'XBOX360PS5_OFFICIAL_TITLE_ID')
PRODUCT = 'PS5X360Reloaded'
# (file, the official text, PS5X360Reloaded's): the first must be gone and the second there.
RENAMED = [
    ('include/xbox360ps5/build_version.hpp', '"PS5X360 v" XBOX360PS5_VERSION', 'XBOX360PS5_PRODUCT_NAME " v" XBOX360PS5_VERSION'),
    ('include/xbox360ps5/build_version.hpp', '"PS5X360 development build"', 'XBOX360PS5_PRODUCT_NAME " development build"'),
    ('src/canary_game_main.cpp', 'Window(context, "PS5X360"', 'Window(context, XBOX360PS5_PRODUCT_NAME'),
    ('src/canary_game_main.cpp', 'InitializeLogging("PS5X360")', 'InitializeLogging(XBOX360PS5_PRODUCT_NAME)'),
    ('src/canary_game_main.cpp', 'Notify({"PS5X360"', 'Notify({XBOX360PS5_PRODUCT_NAME'),
    ('src/canary_game_main.cpp', 'Tr("PS5X360 Guide")', 'Tr("PS5X360Reloaded Guide")'),
    ('src/launcher.cpp', 'who = "PS5X360";', 'who = XBOX360PS5_PRODUCT_NAME;'),
    ('src/launcher.cpp', 'c.Text("PS5X360", 96', 'c.Text(XBOX360PS5_PRODUCT_NAME, 96'),
    ('src/launcher.cpp', 'std::string("PS5X360  ', 'std::string(XBOX360PS5_PRODUCT_NAME "  '),
    ('src/launcher.cpp', 'Tr("PS5X360: experimental', 'Tr("PS5X360Reloaded: experimental'),
    ('platform/ps5/web_settings.cpp', '<title>PS5X360</title>', '<title>PS5X360Reloaded</title>'),
    ('platform/ps5/web_settings.cpp', '<h1>PS5X360<small', '<h1>PS5X360Reloaded<small'),
    ('platform/ps5/web_settings.cpp', 'filename=\\"PS5X360-logs-', 'filename=\\"" XBOX360PS5_PRODUCT_NAME "-logs-'),
    ('platform/ps5/web_settings.cpp', 'filename=\\"ps5x360-camera.crt', 'filename=\\"ps5x360reloaded-camera.crt'),
    ('port/game_art.cpp', 'User-Agent: PS5X360\\r', 'User-Agent: " XBOX360PS5_PRODUCT_NAME "\\r'),
    ('port/phone_camera.hpp', '<title>PS5X360 · Phone camera</title>', '<title>PS5X360Reloaded · Phone camera</title>'),
    ('port/phone_camera.hpp', "document.title='PS5X360 · '", "document.title='PS5X360Reloaded · '"),
    ('port/phone_camera_console.hpp', '<h1>PS5X360 Kinect</h1>', '<h1>PS5X360Reloaded Kinect</h1>'),
    ('port/phone_camera_console.hpp', '<title>PS5X360 Kinect</title>', '<title>PS5X360Reloaded Kinect</title>'),
    ('port/phone_camera_tls.cpp', '"CN=PS5X360 Kinect camera (this console),O=PS5X360"',
     '"CN=" XBOX360PS5_PRODUCT_NAME " Kinect camera (this console),O=" XBOX360PS5_PRODUCT_NAME'),
    ('port/phone_camera_tls.cpp', '",O=PS5X360"', '",O=" XBOX360PS5_PRODUCT_NAME'),
]
# (file, text, why): must still be there.
KEPT = [
    ('include/xbox360ps5/session_log.hpp', '"PS5X360 " + version',
     'the session log\'s first line, which tools/autolog.py parses (^PS5X360 (.*)$)'),
    ('.deps/xenia-canary/src/xenia/kernel/xam/storage_devices.h', 'folder / "PS5X360" / "saves"',
     'the save folder on a drive (fix 27) keeps its name, so testers\' saves stay where they are'),
]


class Problems(list):
    def add(self, kind, text):
        self.append(f'{kind}: {text}')


def code_files(tree):
    roots = [tree / d for d in TITLE_DIRS] + [tree / CORE_DIR]
    for root in roots:
        if not root.is_dir():
            continue
        for path in sorted(root.rglob('*')):
            if path.is_file() and (path.suffix in SUFFIXES or path.name == 'CMakeLists.txt'):
                rel = path.relative_to(tree).as_posix()
                if rel.startswith('port/vendor/'):
                    continue
                yield rel, path.read_bytes().decode('utf-8', errors='replace').replace('\r\n', '\n')


def check(tree, problems):
    files = dict(code_files(tree))
    has_core = (tree / CORE_DIR).is_dir()
    # (a) the official title ID.
    found = set()
    for rel, text in files.items():
        if OFFICIAL_ID not in text:
            continue
        for number, line in enumerate(text.split('\n'), 1):
            if OFFICIAL_ID in line:
                if (rel, line.strip()) in ALLOWED:
                    found.add((rel, line.strip()))
                else:
                    problems.add('a', f'{rel}:{number}: {line.strip()[:160]}')
    for item in sorted(set(ALLOWED) - found):
        problems.add('a', f'allowed line not found: {item}')
    print(f'(a) {OFFICIAL_ID}: {len(files)} files ({"title and core" if has_core else "title only: the tree has no core"}), '
          f'{len(found)} allowed line(s): ' + '; '.join(ALLOWED.values()))
    # (b) the identity header.
    identity = files.get(IDENTITY)
    if identity is None:
        problems.add('b', f'{IDENTITY} missing')
        return
    defines = {}
    for rel, text in files.items():
        for m in re.finditer(r'^\s*#\s*define\s+(XBOX360PS5_[A-Z_]+)\s+(.*)$', text, re.M):
            if m.group(1) in MACROS:
                defines.setdefault(m.group(1), []).append((rel, m.group(2).strip()))
    values = {}
    for macro in MACROS:
        sites = defines.get(macro, [])
        if len(sites) != 1 or sites[0][0] != IDENTITY:
            problems.add('b', f'{macro} defined {len(sites)} times ({[rel for rel, _ in sites]}), want once in {IDENTITY}')
        elif sites:
            values[macro] = sites[0][1]
    title_id = values.get('XBOX360PS5_TITLE_ID', '').strip('"')
    if not re.fullmatch(r'PPSA\d{5}', title_id) or title_id == OFFICIAL_ID:
        problems.add('b', f'title ID {title_id!r}: want PPSA and five digits, not {OFFICIAL_ID}')
    if values.get('XBOX360PS5_DATA_ROOT') != '"/data/homebrew/" XBOX360PS5_TITLE_ID':
        problems.add('b', f'data root {values.get("XBOX360PS5_DATA_ROOT")!r}')
    if values.get('XBOX360PS5_PRODUCT_NAME') != f'"{PRODUCT}"':
        problems.add('b', f'product name {values.get("XBOX360PS5_PRODUCT_NAME")!r}')
    if values.get('XBOX360PS5_OFFICIAL_TITLE_ID') != f'"{OFFICIAL_ID}"':
        problems.add('b', f'official title ID {values.get("XBOX360PS5_OFFICIAL_TITLE_ID")!r}')
    print(f'(b) identity: {PRODUCT}, {title_id}, data root /data/homebrew/{title_id}; '
          f'{len(MACROS)} macros defined once each')
    # (c) /data/homebrew literals and the official folder's readers.
    literals = hints = 0
    readers = set()
    # Interface text is a literal (a Tr key): the hints that name the folder are fix 46's renamed keys,
    # and they must name this title's folder.
    shown = {rekey.renamed_key(key) for key in rekey.RENAMED}
    for rel, text in files.items():
        if rel.endswith(('.txt', '.cmake', '.md')):
            continue
        if '/data/homebrew' in text:
            for tok in rekey.lex(text, rel):
                if tok.kind == 'str' and '/data/homebrew' in tok.value and rel != IDENTITY:
                    if tok.value in shown and f'/data/homebrew/{title_id}/' in tok.value:
                        hints += 1
                        continue
                    literals += 1
                    problems.add('c', f'{rel}:{rekey.line_of(text, tok.start)}: {tok.value[:120]!r} '
                                      '(use XBOX360PS5_DATA_ROOT)')
        if rel == IDENTITY:
            continue
        for number, line in enumerate(text.split('\n'), 1):
            code = line.split('//', 1)[0]  # a comment may name it
            if re.search(r'\b(kOfficialDataRoot|kOfficialTitleId|XBOX360PS5_OFFICIAL_TITLE_ID)\b', code):
                if (rel, line.strip()) in READERS:
                    readers.add((rel, line.strip()))
                else:
                    problems.add('c', f'{rel}:{number}: the official folder used outside READERS: {line.strip()[:160]}')
    for item in sorted(set(READERS) - readers):
        problems.add('c', f'reader not found: {item}')
    print(f'(c) paths: no /data/homebrew literal outside {IDENTITY} ({literals} found; {hints} interface hints '
          f'name /data/homebrew/{title_id}); the official folder '
          f'read at {len(readers)} place(s): ' + '; '.join(READERS.values()))
    # (d) the name.
    for rel, old, new in RENAMED:
        text = files.get(rel)
        if text is None:
            problems.add('d', f'{rel} missing')
            continue
        if old in text:
            problems.add('d', f'{rel}: still {old!r}')
        if new not in text:
            problems.add('d', f'{rel}: {new!r} missing')
    console = files.get('port/phone_camera_console.hpp', '')
    certificate = f'"{PRODUCT} Kinect camera"'
    if console.count(certificate) != 3:
        problems.add('d', f'port/phone_camera_console.hpp: the instructions (en, pt, es) name {certificate} '
                          f'{console.count(certificate)} times, want 3 (the certificate the title makes)')
    kept = 0
    for rel, text, why in KEPT:
        if rel.startswith('.deps/') and not has_core:
            continue
        kept += 1
        if text not in files.get(rel, ''):
            problems.add('d', f'{rel}: {text!r} gone ({why})')
    print(f'(d) name: {len(RENAMED)} texts renamed, the camera instructions name the certificate, {kept} kept')
    # (e) the import's place in the start.
    main = files.get('src/canary_game_main.cpp', '')
    run = main.find('official_import::Run(')
    for later in ('global.Load();', 'xe::InitializeLogging(', 'PrepareSaveStorage(', 'ApplyNetworkConf();\n'):
        at = main.find(later, main.find('int main(int argc, char** argv) {'))
        if run < 0 or at < 0 or run > at:
            problems.add('e', f'official_import::Run is not before {later.strip()!r} in main')
    print('(e) the import runs before the settings, the log, the network settings and the saves')
    # (f) the language files.
    renamed_keys = 0
    for path in sorted((ENGLISH / 'lang').glob('*.txt')):
        text = path.read_text(encoding='utf-8')
        if OFFICIAL_ID in text:
            problems.add('f', f'english/lang/{path.name} names {OFFICIAL_ID}')
        keys = {rekey.unescape(line.split(' = ', 1)[0]) for line in text.split('\n') if line and not line.startswith('#')}
        for key in rekey.RENAMED:
            if key in keys:
                problems.add('f', f'english/lang/{path.name}: old key {key[:60]!r}')
            if rekey.renamed_key(key) in keys:
                renamed_keys += 1
    print(f'(f) english/lang: no {OFFICIAL_ID}; {renamed_keys} renamed keys in the files, no old one')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--tree', type=Path, required=True, help='assembled 1.0.2 tree with the fixes')
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    problems = Problems()
    check(args.tree.resolve(), problems)
    if problems:
        print(f'FAIL: {len(problems)} problems', file=sys.stderr)
        for line in problems:
            print('  ' + line, file=sys.stderr)
        sys.exit(1)
    print('PASS: PS5X360Reloaded has its own title ID, folder and name; the official folder is only read (fix 46)')


if __name__ == '__main__':
    main()
