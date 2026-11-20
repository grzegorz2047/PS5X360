"""Builds the PPSA50011 package of a PS5X360 release compiled from source (tools/source-1.0.2).

    python3 -I tools/source-1.0.2/package.py --base-zip PPSA50011-v1.0.2.zip \
        --eboot build/src102-out/eboot.bin --lang-dir build/src102-out/lang \
        --sources-json build/src102/SOURCES.json --version 1.0.3 \
        --source-url https://github.com/<owner>/<repo>/tree/<commit> --out-dir dist/source-release

The base is the official v1.0.2 PPSA50011.zip, checked against its SHA-256 as
tools/binary-release/build_package.py checks it. Unlike that script, nothing is patched or
signed again: eboot.bin is the source build's, already signed by build.sh. This script:

  * replaces eboot.bin, after checking the SELF magic and that the binary carries the release's
    version labels ("PS5X360 v<version>", "BOOT version <version>") and no candidate label;
  * replaces every assets/lang/*.txt with the English-keyed files of the build (fix 25; the
    eboot reads /app0/assets/lang/). It does not merge assets/lang-overrides, because those are
    keyed by Portuguese for the official executable and their texts are already in
    tools/source-1.0.2/english/lang. A base language with no English-keyed file stops the build.
  * takes assets/presets.txt, assets/config/*.toml and assets/patches/*.patch.toml from this
    repository, as the binary release does, and drops readback-auto.off;
  * writes VERSION, RELEASE.json (a source build), licenses/PS5X360-<version>-NOTICE.txt (the
    GPL-3.0 notice), the licence texts the official package lacks (tools/source-1.0.2/licenses),
    and MANIFEST.json;
  * writes <out-dir>/PPSA50011.zip (deterministic), <out-dir>/PPSA50011.zip.sha256 and the
    unpacked <out-dir>/stage/PPSA50011. The images come from it with
    tools/binary-release/make_images.sh <out-dir>/stage/PPSA50011 <out-dir>.

The modification date in the NOTICE is --date, else SOURCE_DATE_EPOCH, else today (UTC).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tools/binary-release'))
import build_package  # noqa: E402  (constants and helpers only; its main() is not run)
import fself  # noqa: E402

PREFIX = build_package.PREFIX
BASE_ZIP_SHA256 = build_package.BASE_ZIP_SHA256
BASE_VERSION = build_package.BASE_VERSION
UPSTREAM = build_package.UPSTREAM
ZIP_TIME = build_package.ZIP_TIME
sha256 = build_package.sha256

VERSION_RE = re.compile(r'[0-9]+\.[0-9]+\.[0-9]+')
# A test build's label (BUILD_VERSION 1.0.3_candidate_N). A bare "candidate_" is not enough:
# Mesa's own strings contain it (intersection_candidate_aabb_opaque, candidate_infos).
CANDIDATE_LABEL_RE = re.compile(rb'[0-9]+\.[0-9]+\.[0-9]+_candidate')
# First line of every file tools/source-1.0.2/english/rekey.py makes.
ENGLISH_HEADER_RE = re.compile(r'# PS5X360 interface text: (\S+)\. Keys are English')
SOURCE_URL_RE = re.compile(r'(https://[^/]+/[^/]+/[^/]+)/(?:tree|commit)/([0-9a-f]{40})/?')
WORKFLOW = ROOT / '.github/workflows/build-source-1.0.2.yml'

# (file in tools/source-1.0.2/licenses, path in the package, SHA-256); see licenses/README.md.
LICENCES = [
    ('GPL-3.0.txt', 'licenses/GPL-3.0.txt',
     '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'),
    ('MINIUPNPC-BSD-3.txt', 'licenses/MINIUPNPC.txt',
     'ae63874b77b3d9c58ab99b2d116d83dca5f8c79d3f7d16d6bf7c23068f640fb5'),
    ('MBEDTLS-3.6.7-LICENSE.txt', 'licenses/MBEDTLS-3.6.7-LICENSE.txt',
     '9b405ef4c89342f5eae1dd828882f931747f71001cfba7d114801039b52ad09b'),
    ('MPL-2.0.txt', 'licenses/CACERT-MPL-2.0.txt',
     '3f3d9e0024b1921b067d6f7f88deb4a60cbe7a78e76c64e3f1d7fc3b779b9d04'),
]


def fail(message):
    raise SystemExit('package.py: ' + message)


def check_version(version):
    if not VERSION_RE.fullmatch(version or ''):
        fail(f'version {version!r} is not X.Y.Z (a release, not a candidate label)')
    return version


def source_names(version):
    """The GPL source assets release-source.yml attaches: the assembled tree that was compiled
    (made by build-source-1.0.2.yml), the toolchain repositories at the build's pinned commits
    and the git archive of this repository at the commit."""
    return {'tarball': f'PS5X360-{version}-source.tar.xz',
            'toolchain': f'PS5X360-{version}-toolchain-source.tar.xz',
            'scripts': f'PS5X360Reloaded-{version}-scripts.tar.gz'}


def parse_source_url(url):
    """(repository URL, 40-hex commit) of https://<host>/<owner>/<repo>/tree|commit/<sha>."""
    match = SOURCE_URL_RE.fullmatch(url or '')
    if not match:
        fail(f'--source-url {url!r} is not https://<host>/<owner>/<repo>/tree/<40-hex commit>')
    return match.group(1), match.group(2)


def modification_date(value=None, environ=os.environ):
    if value:
        try:
            datetime.date.fromisoformat(value)
        except ValueError:
            fail(f'--date {value!r} is not YYYY-MM-DD')
        return value
    epoch = environ.get('SOURCE_DATE_EPOCH')
    if epoch:
        if not epoch.isdigit():
            fail(f'SOURCE_DATE_EPOCH {epoch!r} is not a number of seconds')
        stamp = datetime.datetime.fromtimestamp(int(epoch), datetime.timezone.utc)
    else:
        stamp = datetime.datetime.now(datetime.timezone.utc)
    return stamp.date().isoformat()


def toolchain_pins(workflow_text):
    """The pins of build-source-1.0.2.yml: the three mihawk-99 reference commits (required) and
    the Vulkan SDK and zlib archives (when found)."""
    pins = {}
    for key, name in (('PS5_VULKAN_REV', 'PS5_Vulkan'), ('PS5_MESA_REV', 'PS5_Mesa'),
                      ('PS5_PAYLOAD_SDK_REV', 'PS5_PayloadSDK')):
        match = re.search(rf'^\s*{key}:\s*([0-9a-f]{{40}})\s*$', workflow_text, re.M)
        if not match:
            fail(f'{WORKFLOW.name}: no {key} pin')
        pins[name] = {'url': f'https://github.com/mihawk-99/{name}', 'commit': match.group(1)}
    sdk = re.search(r'\bv=([0-9.]+)\s*$', workflow_text, re.M)
    sdk_sha = re.search(r'"([0-9a-f]{64})  \$RUNNER_TEMP/\$file"', workflow_text)
    if sdk and sdk_sha:
        pins['Vulkan SDK'] = {'version': sdk.group(1), 'sha256': sdk_sha.group(1)}
    zlib = re.search(r'zlib-([0-9.]+)\.tar\.gz"\s', workflow_text)
    zlib_sha = re.search(r'"([0-9a-f]{64})  \$zlib_dir/', workflow_text)
    if zlib and zlib_sha:
        pins['zlib'] = {'version': zlib.group(1), 'sha256': zlib_sha.group(1)}
    runner = re.search(r'^\s*runs-on:\s*(\S+)', workflow_text, re.M)
    pins['host'] = {'runner': runner.group(1) if runner else 'ubuntu-24.04',
                    'compiler': 'clang-18 / lld-18 through the payload SDK prospero-clang wrappers'}
    return pins


def check_eboot(eboot, version):
    if len(eboot) < 4 or int.from_bytes(eboot[:4], 'little') not in fself.FSELF_MAGICS:
        fail('eboot.bin is not a signed PS5 SELF (build.sh output expected)')
    for label in (b'PS5X360 v' + version.encode(), b'BOOT version ' + version.encode()):
        if label + b'\0' not in eboot:
            fail(f'eboot.bin does not carry the label {label.decode()!r}: built with another version')
    found = CANDIDATE_LABEL_RE.search(eboot)
    if found:
        fail(f'eboot.bin carries a candidate label ({eboot[found.start():found.end() + 8]!r})')


def check_sources(sources, version):
    if sources.get('version') != version:
        fail(f'SOURCES.json version {sources.get("version")!r} is not {version!r}')
    if sources.get('release') is not True:
        fail('SOURCES.json is not from a release assembly (assemble.py --release)')
    if 'fixes_skipped' not in sources:
        fail('SOURCES.json has no fixes_skipped list')
    if sources['fixes_skipped'] != []:
        fail(f'SOURCES.json lists skipped fixes: {sources["fixes_skipped"]}')
    for key in ('drops', 'core', 'fixes'):
        if not sources.get(key):
            fail(f'SOURCES.json has no {key!r} (not a full assembly)')


def english_lang(lang):
    """Checks {name: bytes} are the English-keyed files of tools/source-1.0.2/english/lang."""
    if not lang:
        fail('no language files (*.txt) in --lang-dir')
    for name, data in sorted(lang.items()):
        first = data.split(b'\n', 1)[0].decode('utf-8', 'replace')
        match = ENGLISH_HEADER_RE.match(first)
        if not match or f'{match.group(1)}.txt' != name:
            fail(f'{name} is not an English-keyed language file of tools/source-1.0.2/english/lang')
    return lang


def licence_texts():
    texts = {}
    for name, target, digest in LICENCES:
        data = (HERE / 'licenses' / name).read_bytes()
        if sha256(data) != digest:
            fail(f'tools/source-1.0.2/licenses/{name} is not the verbatim text (SHA-256 differs)')
        texts[target] = data
    return texts


def notice(version, date, repo, commit, eboot_sha, sources, toolchain, sources_sha):
    release = f'{repo}/releases/tag/v{version}'
    names = source_names(version)
    tarball, toolchain_tar, scripts = names['tarball'], names['toolchain'], names['scripts']
    drops = '\n'.join(f'        {info["asset"]} ({tag})  sha256 {info["sha256"]}'
                      for tag, info in sorted(sources['drops'].items()))
    online = sources.get('online', {})
    net_libs = '\n'.join(f'        {path}  {rev}'
                         for path, rev in sorted(online.get('net_libs', {}).items()))
    mbedtls = online.get('mbedtls', {})
    fixes = '\n'.join(f'        {Path(fix["patch"]).name}  sha256 {fix["sha256"]}' for fix in sources['fixes'])
    pins = []
    for name, pin in toolchain.items():
        if 'commit' in pin:
            pins.append(f'        {name}  {pin["url"]} commit {pin["commit"]}')
        elif 'sha256' in pin:
            pins.append(f'        {name} {pin["version"]}  sha256 {pin["sha256"]}')
        else:
            pins.append(f'        build host  {pin["runner"]}, {pin["compiler"]}')
    pins = '\n'.join(pins)
    return f"""PS5X360 {version} - built from source, distributed under the GNU GPL version 3

Licence of eboot.bin
    eboot.bin as a whole is distributed under the terms of the GNU General Public License
    version 3 (licenses/GPL-3.0.txt). It links wolfSSL 5.9.4 (GPL-3.0-or-later,
    licenses/WOLFSSL-GPL-3.txt), ps5-framegen (GPL-3.0-or-later, licenses/PS5-FRAMEGEN-GPL-3.txt)
    and the GPL-3.0 PS5 runtime of PS5_Vulkan and the payload SDK (licenses/PS5-VULKAN-GPL-3.txt,
    licenses/NATIVE-RUNTIME-GPL-3.txt). Each part keeps its own licence as well: the PS5X360
    code is MIT (LICENSE), Xenia is BSD-3-Clause (licenses/XENIA-BSD.txt,
    licenses/XENIA-NETPLAY-BSD.txt), RADV/Mesa is MIT (licenses/MESA-LICENSE.txt), libcurl is
    under the curl licence (licenses/CURL.txt), miniupnpc is BSD-3-Clause (licenses/MINIUPNPC.txt), Mbed TLS 3.6.7 is
    used under Apache-2.0 (licenses/MBEDTLS-3.6.7-LICENSE.txt), and the embedded Mozilla CA
    certificate bundle (cacert.pem, from curl.se) is MPL-2.0 (licenses/CACERT-MPL-2.0.txt).
    The other licence files in licenses/, kept from the official package, cover further
    third-party code of Xenia Canary and of the package, and apply to the parts they name.
    There is NO WARRANTY, to the extent permitted by law (GPL-3.0 sections 15 and 16).

Modified version (GPL-3.0 section 5)
    This is a modified version of PS5X360 {BASE_VERSION} by BrinooTk ({UPSTREAM}) and of
    Xenia Canary netplay, modified by the contributors of {repo}; modified {date}.
    eboot.bin was compiled from source: the author's public source drops, assembled by
    tools/source-1.0.2/assemble.py, with reconstructed headers for parts that were never
    published and the fixes listed below. It is not the official {BASE_VERSION} executable.

Built from
    Repository   {repo} commit {commit}
    eboot.bin    sha256 {eboot_sha}
    SOURCES.json sha256 {sources_sha} (where every source file came from)
    Source drops (BrinooTk/PS5X360 releases):
{drops}
    Core         {sources['core'].get('base', '')}
    Network libraries (netplay submodules):
{net_libs}
    Mbed TLS     {mbedtls.get('url', '')}  sha256 {mbedtls.get('sha256', '')}
    Fixes applied (tools/source-1.0.2/fixes):
{fixes}
    Toolchain:
{pins}

Corresponding source (GPL-3.0 section 6)
    Attached to the release {release}:
        {tarball}
            the assembled tree build.sh compiled (tools/source-1.0.2/assemble.py): the title,
            the Xenia Canary netplay core with the submodules it uses, the network libraries,
            Mbed TLS, the fixes applied and SOURCES.json
        {scripts}
            the scripts that control compilation and installation: this repository
            ({repo}) at commit {commit}, with tools/source-1.0.2 (assemble.py,
            build.sh, package.py), tools/build-radv-platform.sh,
            tools/binary-release/make_images.sh and .github/workflows
    GitHub's source archive of the tag v{version} on the release page is the same commit.
    Not in {tarball}: the parts linked into eboot.bin from the toolchain repositories, RADV
    (Mesa, from PS5_Mesa, built by tools/build-radv-platform.sh), app_crt.cpp and the AGC import
    stubs (PS5_Vulkan) and the payload SDK runtime (PS5_PayloadSDK), and the tools that link and
    sign it (radv-link.sh and ps5-native-tool from PS5_Vulkan). Their source is those public
    repositories at the commits listed under Toolchain above, attached as {toolchain_tar}
    (PS5_Vulkan, PS5_Mesa and PS5_PayloadSDK without .git, and PINS.txt).

Not linked into this eboot.bin
    The OpenGL renderer stack (Vulkan only), the physical USB Kinect and its relay, and the disc
    helper (disc-helper.elf relay) are NOT part of this executable. licenses/PS5-OpenGL,
    KINECT-source-and-licenses.zip and tools/kinect-tracker.elf are kept from the official
    package unchanged.

Other files
    Every other file is the official {BASE_VERSION} PPSA50011.zip (sha256 {BASE_ZIP_SHA256})
    unchanged, except: assets/lang/*.txt (English-keyed, tools/source-1.0.2/english/lang),
    assets/presets.txt, assets/config/*.toml and assets/patches/*.patch.toml (from the
    repository), VERSION, RELEASE.json, MANIFEST.json, this notice and the licence files above
    (tools/source-1.0.2/licenses/README.md says where each came from).
    licenses/PS5X360-NOTICE.txt is the official 1.0.0 notice, kept as provenance.
""".encode()


def build(base_files, eboot, lang, sources, version, source_url, date, toolchain, sources_sha,
          repo_root=ROOT):
    """The package's files ({zip name: bytes}) from the official base's files. `sources` is the
    parsed SOURCES.json and `sources_sha` the SHA-256 of its bytes."""
    check_version(version)
    repo, commit = parse_source_url(source_url)
    check_eboot(eboot, version)
    check_sources(sources, version)
    english_lang(lang)
    files = dict(base_files)
    if files.get(PREFIX + 'VERSION') != (BASE_VERSION + '\n').encode():
        fail('base package VERSION is not ' + BASE_VERSION)
    if PREFIX + 'eboot.bin' not in files:
        fail('base package has no eboot.bin')
    base_lang = sorted(name for name in files
                       if name.startswith(PREFIX + 'assets/lang/') and name.endswith('.txt'))
    if not base_lang:
        fail('base package has no assets/lang/*.txt')
    missing = [name for name in base_lang if name.rsplit('/', 1)[1] not in lang]
    if missing:
        fail(f'no English-keyed file for {", ".join(missing)}')

    eboot_sha = sha256(eboot)
    files[PREFIX + 'eboot.bin'] = eboot
    for name in base_lang:
        del files[name]
    for name, data in sorted(lang.items()):
        files[PREFIX + 'assets/lang/' + name] = data
    files[PREFIX + 'assets/presets.txt'] = (repo_root / 'assets/presets.txt').read_bytes()
    for path in sorted((repo_root / 'assets/config').glob('*.toml')):
        files[PREFIX + 'assets/config/' + path.name] = path.read_bytes()
    for path in sorted((repo_root / 'assets/patches').glob('*.patch.toml')):
        files[PREFIX + 'assets/patches/' + path.name] = path.read_bytes()
    # As in the binary release: this marker would switch off the copy-back of small resolves.
    files.pop(PREFIX + 'readback-auto.off', None)
    files.pop(PREFIX + 'licenses/PS5X360-OPT-NOTICE.txt', None)
    for target, data in licence_texts().items():
        files[PREFIX + target] = data

    files[PREFIX + 'VERSION'] = (version + '\n').encode()
    files[PREFIX + f'licenses/PS5X360-{version}-NOTICE.txt'] = notice(
        version, date, repo, commit, eboot_sha, sources, toolchain, sources_sha)
    files[PREFIX + 'RELEASE.json'] = (json.dumps({
        'version': version,
        'executable_sha256': eboot_sha,
        'candidate': False,
        'published': True,
        'candidate_id': f'src-{eboot_sha[:8]}',
        'build': 'source',
        'base_package': {'name': f'PS5X360 v{BASE_VERSION} PPSA50011.zip', 'sha256': BASE_ZIP_SHA256},
        'source_repository': repo,
        'source_commit': commit,
        'source_url': source_url,
        'source_assets': sorted(source_names(version).values()),
        'drops': {tag: info['sha256'] for tag, info in sorted(sources['drops'].items())},
        'core': sources['core'].get('base', ''),
        'fixes': [Path(fix['patch']).name for fix in sources['fixes']],
        'fixes_skipped': [],
        'sources_json_sha256': sources_sha,
        'toolchain': toolchain,
    }, indent=2) + '\n').encode()
    files.pop(PREFIX + 'MANIFEST.json', None)
    manifest = {name[len(PREFIX):]: {'bytes': len(data), 'sha256': sha256(data)}
                for name, data in sorted(files.items())}
    files[PREFIX + 'MANIFEST.json'] = (json.dumps(manifest, indent=2) + '\n').encode()
    return files


def write_package(files, out_dir):
    """Writes the deterministic PPSA50011.zip (as build_package.py does), checks it, extracts
    <out_dir>/stage and writes PPSA50011.zip.sha256. Returns the zip's SHA-256."""
    out_dir.mkdir(parents=True, exist_ok=True)
    out_zip = out_dir / 'PPSA50011.zip'
    with zipfile.ZipFile(out_zip, 'w', zipfile.ZIP_DEFLATED) as zout:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo(name, date_time=ZIP_TIME)
            info.compress_type = zipfile.ZIP_DEFLATED
            # Unix (3) on every host: zipfile's default is 0 on Windows, which changes the bytes
            # and makes unzip ignore the 0644 mode.
            info.create_system = 3
            info.external_attr = 0o644 << 16
            zout.writestr(info, data)
    manifest = json.loads(files[PREFIX + 'MANIFEST.json'])
    with zipfile.ZipFile(out_zip) as zcheck:
        if zcheck.testzip() is not None:
            fail('ZIP CRC check failed')
        if sorted(zcheck.namelist()) != sorted(files):
            fail('ZIP entries differ from the package files')
        for name, record in manifest.items():
            data = zcheck.read(PREFIX + name)
            if len(data) != record['bytes'] or sha256(data) != record['sha256']:
                fail(f'MANIFEST check failed: {name}')
        stage = out_dir / 'stage'
        shutil.rmtree(stage, ignore_errors=True)
        zcheck.extractall(stage)
    digest = sha256(out_zip.read_bytes())
    (out_dir / 'PPSA50011.zip.sha256').write_text(f'{digest}  PPSA50011.zip\n', encoding='utf-8')
    return digest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--base-zip', type=Path, required=True, help='official v1.0.2 PPSA50011.zip')
    parser.add_argument('--eboot', type=Path, required=True, help='eboot.bin of the source build')
    parser.add_argument('--lang-dir', type=Path, required=True,
                        help='English-keyed *.txt of the build (src102-out/lang)')
    parser.add_argument('--sources-json', type=Path, required=True, help='SOURCES.json of the build')
    parser.add_argument('--version', required=True, help='release version X.Y.Z')
    parser.add_argument('--source-url', required=True,
                        help='https://<host>/<owner>/<repo>/tree/<commit> the build was made from')
    parser.add_argument('--out-dir', type=Path, required=True)
    parser.add_argument('--date', help='modification date YYYY-MM-DD for the notice '
                                       '(default: SOURCE_DATE_EPOCH, else today in UTC)')
    args = parser.parse_args(argv)

    version = check_version(args.version)
    base = args.base_zip.read_bytes()
    if sha256(base) != BASE_ZIP_SHA256:
        fail(f'{args.base_zip} is not the official v{BASE_VERSION} PPSA50011.zip')
    with zipfile.ZipFile(args.base_zip) as zin:
        base_files = {i.filename: zin.read(i) for i in zin.infolist() if not i.is_dir()}
    sources_bytes = args.sources_json.read_bytes()
    sources = json.loads(sources_bytes)
    lang = {path.name: path.read_bytes() for path in sorted(args.lang_dir.glob('*.txt'))}
    files = build(base_files, args.eboot.read_bytes(), lang, sources, version, args.source_url,
                  modification_date(args.date), toolchain_pins(WORKFLOW.read_text(encoding='utf-8')),
                  sha256(sources_bytes))
    digest = write_package(files, args.out_dir)
    out_zip = args.out_dir / 'PPSA50011.zip'
    print(json.dumps({'version': version, 'package': str(out_zip), 'files': len(files),
                      'bytes': out_zip.stat().st_size, 'sha256': digest,
                      'eboot_sha256': sha256(files[PREFIX + 'eboot.bin'])}))
    return 0


if __name__ == '__main__':
    sys.exit(main())
