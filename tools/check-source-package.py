"""Host check of tools/source-1.0.2/package.py, the packager of a release built from source.

    python3 -I tools/check-source-package.py

Runs on a small made-up base PPSA50011.zip (the official one is 86 MB and pinned by SHA-256;
the check swaps the pinned hash for the fixture's to run the command line end to end) that
carries the official sce_sys/param.json verbatim. It checks:
  * the layout is the base's, moved from PPSA50011/ to PPSA83600/ (PS5X360Reloaded's title id),
    with eboot.bin replaced byte for byte (not patched, not re-signed);
  * sce_sys/param.json differs from the base in exactly titleId PPSA83600, conceptId 83600,
    contentId UP9000-PPSA83600_00-PS5X360RELOADED0 and titleName PS5X360Reloaded in every
    language, keeping the base's key order, indent and CRLF; a base param that is not the
    official app's is refused;
  * sce_sys/icon0.png, pic0.png and pic1.png are the reviewed PS5X360Reloaded art (variant A3,
    by SHA-256) from tools/source-1.0.2/package-art, at the official sizes;
  * every assets/lang/*.txt is the build's English-keyed file, pt-BR.txt is added, and nothing of
    the Portuguese-keyed base files or assets/lang-overrides is merged in;
  * presets, configs and patches come from the repository; readback-auto.off and the binary
    release's OPT notice are absent;
  * VERSION, RELEASE.json, the GPL notice and the added licence texts (verbatim, by SHA-256);
  * MANIFEST.json lists every other file, the stage matches the zip, the outputs are
    PPSA83600.zip, PPSA83600.zip.sha256 and stage/PPSA83600, and two runs give the same zip byte
    for byte;
  * the home-screen label package.py wants is check_release_elf.py's ("PS5X360Reloaded v");
  * it refuses: a candidate or malformed version, an eboot with another or a candidate label, with
    the official app's "PS5X360 v" label or without the SELF magic, base files outside
    PPSA50011/, a base without param.json, art that is not the reviewed files, skipped fixes, a SOURCES.json of another version, a base language
    without an English-keyed file, a Portuguese-keyed file, a source URL without a commit and a
    base zip other than the official one.
"""
import hashlib
import io
import json
import re
import sys
import tempfile
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools/source-1.0.2'))
import package  # noqa: E402
import check_release_elf  # noqa: E402

B = package.BASE_PREFIX
P = package.PREFIX
assert B == 'PPSA50011/' and P == 'PPSA83600/', (B, P)
VERSION = '1.0.3'
# sce_sys/param.json of the official v1.0.2 PPSA50011.zip, verbatim (881 bytes, CRLF).
OFFICIAL_PARAM = ('{\r\n  "ageLevel": {\r\n    "default": 0\r\n  },\r\n  "applicationCategoryType": 0,\r\n'
                  '  "applicationDrmType": "free",\r\n  "attribute": 0,\r\n  "attribute2": 0,\r\n'
                  '  "attribute3": 0,\r\n  "conceptId": "50011",\r\n  "contentBadgeType": 1,\r\n'
                  '  "contentId": "UP9000-PPSA50011_00-XBOX360PS5M80001",\r\n  "contentVersion": "00.001.000",\r\n'
                  '  "downloadDataSize": 256,\r\n  "gameIntent": {\r\n    "permittedIntents": [\r\n      {\r\n'
                  '        "intentType": "launchActivity"\r\n      }\r\n    ]\r\n  },\r\n'
                  '  "localizedParameters": {\r\n    "defaultLanguage": "en-US",\r\n    "en-US": {\r\n'
                  '      "titleName": "PS5X360"\r\n    }\r\n  },\r\n  "masterVersion": "00.01",\r\n'
                  '  "pubtools": {\r\n    "creationDate": "2026-08-22 00:00:00",\r\n'
                  '    "loudnessSnd0": "-28.00",\r\n    "toolVersion": "2.00"\r\n  },\r\n'
                  '  "requiredSystemSoftwareVersion": "0x0000000000000000",\r\n'
                  '  "sdkVersion": "0x0000000000000000",\r\n  "titleId": "PPSA50011",\r\n'
                  '  "versionFileUri": ""\r\n}\r\n').encode()
# What package.py must make of it: these four lines differ, nothing else.
RELOADED_PARAM = (OFFICIAL_PARAM.replace(b'"conceptId": "50011"', b'"conceptId": "83600"')
                  .replace(b'"UP9000-PPSA50011_00-XBOX360PS5M80001"', b'"UP9000-PPSA83600_00-PS5X360RELOADED0"')
                  .replace(b'"titleName": "PS5X360"', b'"titleName": "PS5X360Reloaded"')
                  .replace(b'"titleId": "PPSA50011"', b'"titleId": "PPSA83600"'))
# The reviewed PS5X360Reloaded art (variant A3): file, (width, height), SHA-256.
ART = {'icon0.png': ((512, 512), '0ea06e8927b1cf3a284a56e3440fd51017bc3e3efd1fc28b8015c4e6c662d50c'),
       'pic0.png': ((3840, 2160), '279db3b467fed42d9f5213c725a221e92ac919a8673ead61cb3b8755d7db1054'),
       'pic1.png': ((1920, 1080), 'a44a30407639a3859a6efbffb9ec555a9a220f86c9b886e09e2e48bd8776e377')}
COMMIT = 'f9140f1' + '0' * 33
SOURCE_URL = f'https://github.com/example/PS5X360Reloaded/tree/{COMMIT}'
DATE = '2026-11-20'
MAGIC = (0x1D3D154F).to_bytes(4, 'little')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def eboot(label=VERSION, extra=b''):
    # Mesa's own strings contain "candidate_" and must not count as a candidate label.
    return (MAGIC + b'\0' * 60 + b'intersection_candidate_aabb_opaque\0candidate_infos\0'
            + f'PS5X360Reloaded v{label}\0\0\0BOOT version {label}\0'.encode()
            # session_log.hpp's header "PS5X360 " is no label.
            + b'PS5X360 \0' + extra)


def english(code, body='Settings = Ustawienia\n'):
    return (f'# PS5X360 interface text: {code}. Keys are English (fix 25); generated by\n'
            f'# tools/source-1.0.2/english/rekey.py. Do not edit by hand.\n{body}').encode()


LANG = {'en.txt': english('en', 'Settings = Settings\n'), 'pl.txt': english('pl'),
        'pt.txt': english('pt', 'Settings = Configurações\n'),
        'pt-BR.txt': english('pt-BR', 'Settings = Configurações\n')}
PORTUGUESE = {'en.txt': b'# PS5X360 UI translations: en\nConfiguracoes = Settings\n',
              'pl.txt': b'# PS5X360 UI translations: pl\nConfiguracoes = Ustawienia\n',
              'pt.txt': b'# PS5X360 UI translations: pt\nConfiguracoes = Configuracoes\n'}
SOURCES = {
    'version': VERSION,
    'release': True,
    'drops': {'v1.0.2': {'asset': 'PS5X360-source-1.0.2-changes.zip', 'sha256': 'd9bd' + '0' * 60}},
    'core': {'base': 'https://github.com/AdrianCassar/xenia-canary.git d2cf527aa' + '0' * 31},
    'online': {'net_libs': {'third_party/wolfssl': '3c5eead4' + '0' * 32},
               'mbedtls': {'url': 'https://example.invalid/mbedtls-3.6.7.tar.bz2', 'sha256': 'a7e8' + '0' * 60}},
    'fixes': [{'patch': 'tools/source-1.0.2/fixes/01-jit-reserved-barriers.patch', 'sha256': '1' * 64},
              {'patch': 'tools/source-1.0.2/fixes/25-english-source-text.patch', 'sha256': '2' * 64}],
    'fixes_skipped': [],
    'files': {},
}
SOURCES_BYTES = (json.dumps(SOURCES, indent=1, sort_keys=True) + '\n').encode()
TOOLCHAIN = package.toolchain_pins(package.WORKFLOW.read_text(encoding='utf-8'))


def base_files():
    files = {
        B + 'VERSION': b'1.0.2\n',
        B + 'eboot.bin': MAGIC + b'official 1.0.2 executable',
        B + 'RELEASE.json': b'{"version": "1.0.2"}\n',
        B + 'MANIFEST.json': b'{}\n',
        B + 'LICENSE': b'MIT License\n',
        B + 'readback-auto.off': b'',
        # Not in the official zip; a base made from the binary release's stage would carry it.
        B + 'licenses/PS5X360-OPT-NOTICE.txt': b'eboot.bin is the official 1.0.2 executable\n',
        B + 'assets/presets.txt': b'old presets\n',
        B + 'assets/fonts/font.ttf': b'\0font',
        B + 'licenses/MINIUPNPC.txt': b'../LICENSE',
        B + 'licenses/WOLFSSL-GPL-3.txt': b'GPL-3\r\n',
        B + 'licenses/PS5X360-NOTICE.txt': b'PS5X360 1.0.0 - source of the parts\n',
        B + 'KINECT-source-and-licenses.zip': b'PK official kinect sources',
        B + 'tools/kinect-tracker.elf': b'\x7fELF tracker',
        B + 'sce_sys/param.json': OFFICIAL_PARAM,
        B + 'sce_sys/icon0.png': b'\x89PNG official icon0',
        B + 'sce_sys/pic0.png': b'\x89PNG official pic0',
        B + 'sce_sys/pic1.png': b'\x89PNG official pic1',
    }
    for name, data in PORTUGUESE.items():
        files[B + 'assets/lang/' + name] = data
    return files


def base_zip(path):
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in sorted(base_files().items()):
            z.writestr(zipfile.ZipInfo(name, date_time=(2026, 10, 22, 20, 26, 16)), data)
    return path.read_bytes()


def build(**changes):
    args = dict(base_files=base_files(), eboot=eboot(), lang=dict(LANG), sources=json.loads(SOURCES_BYTES),
                version=VERSION, source_url=SOURCE_URL, date=DATE, toolchain=TOOLCHAIN,
                sources_sha=sha(SOURCES_BYTES))
    args.update(changes)
    return package.build(**args)


def refused(contains, **changes):
    try:
        build(**changes)
    except SystemExit as error:
        assert contains in str(error.code), (contains, error.code)
        return
    raise AssertionError(f'not refused: {contains} ({list(changes)})')


def check_files(files, base_sha=package.BASE_ZIP_SHA256):
    base = base_files()
    names = {name[len(P):] for name in files}
    repo_assets = {'assets/presets.txt'}
    repo_assets |= {f'assets/config/{p.name}' for p in (REPO / 'assets/config').glob('*.toml')}
    repo_assets |= {f'assets/patches/{p.name}' for p in (REPO / 'assets/patches').glob('*.patch.toml')}
    added = {f'licenses/PS5X360-{VERSION}-NOTICE.txt', 'licenses/GPL-3.0.txt',
             'licenses/MBEDTLS-3.6.7-LICENSE.txt', 'licenses/CACERT-MPL-2.0.txt', 'assets/lang/pt-BR.txt'}
    expected = ({name[len(B):] for name in base} - {'readback-auto.off', 'licenses/PS5X360-OPT-NOTICE.txt'}
                | repo_assets | added)
    assert names == expected, sorted(names ^ expected)
    assert all(name.startswith(P) for name in files)
    assert not any(name.startswith(B) or B.rstrip('/') in name for name in files)

    # eboot.bin byte for byte; untouched files byte for byte.
    assert files[P + 'eboot.bin'] == eboot()
    for name in ('LICENSE', 'assets/fonts/font.ttf', 'licenses/WOLFSSL-GPL-3.txt',
                 'licenses/PS5X360-NOTICE.txt', 'KINECT-source-and-licenses.zip', 'tools/kinect-tracker.elf'):
        assert files[P + name] == base[B + name], name

    # param.json: the official file with the new id, content id, concept id and name, nothing else.
    assert sha(OFFICIAL_PARAM) == '68f56aa52bfdf3b73f8c8f41294b29da9d9f4d0b49fe5da953ea433123178313'
    param_bytes = files[P + 'sce_sys/param.json']
    assert param_bytes == RELOADED_PARAM, param_bytes
    assert param_bytes != base[B + 'sce_sys/param.json']
    assert param_bytes.count(b'\r\n') == OFFICIAL_PARAM.count(b'\r\n') and b'\n' not in param_bytes.replace(b'\r\n', b'')
    param, official = json.loads(param_bytes), json.loads(OFFICIAL_PARAM)
    assert list(param) == list(official)
    assert param['titleId'] == 'PPSA83600' and param['conceptId'] == '83600'
    assert param['contentId'] == 'UP9000-PPSA83600_00-PS5X360RELOADED0'
    assert re.fullmatch(r'UP9000-PPSA[0-9]{5}_00-[A-Z0-9]{16}', param['contentId'])
    assert param['contentId'] != official['contentId'] and param['conceptId'] == param['titleId'][4:]
    assert param['localizedParameters'] == {'defaultLanguage': 'en-US', 'en-US': {'titleName': 'PS5X360Reloaded'}}
    changed = {key for key in official if official[key] != param[key]}
    assert changed == {'titleId', 'conceptId', 'contentId', 'localizedParameters'}, changed

    # The PS5X360Reloaded art (variant A3), never the official images.
    readme = (REPO / 'tools/source-1.0.2/package-art/README.md').read_text(encoding='utf-8')
    for name, (size, digest) in ART.items():
        data = files[P + 'sce_sys/' + name]
        assert data == (REPO / 'tools/source-1.0.2/package-art' / name).read_bytes(), name
        assert sha(data) == digest and data != base[B + 'sce_sys/' + name], name
        assert data[:8] == b'\x89PNG\r\n\x1a\n' and data[12:16] == b'IHDR', name
        assert (int.from_bytes(data[16:20], 'big'), int.from_bytes(data[20:24], 'big')) == size, name
        assert data[24:26] == b'\x08\x02', name  # 8-bit RGB, as the official files
        assert f'`{name}`' in readme and digest in readme, name
    assert [(n, s, d) for n, _, s, d in package.ART] == [(n, s, d) for n, (s, d) in ART.items()]
    # English-keyed language files, as built; nothing Portuguese-keyed merged in.
    for name, data in LANG.items():
        assert files[P + 'assets/lang/' + name] == data, name
    overrides = b''.join(p.read_bytes() for p in (REPO / 'assets/lang-overrides').glob('*.txt'))
    for name in LANG:
        data = files[P + 'assets/lang/' + name]
        assert b'Configuracoes' not in data and b'UI translations' not in data, name
        if overrides:
            assert overrides not in data
    # Repository package data.
    for name in repo_assets:
        assert files[P + name] == (REPO / name).read_bytes(), name
    assert P + 'licenses/PS5X360-OPT-NOTICE.txt' not in files
    assert P + 'readback-auto.off' not in files

    assert files[P + 'VERSION'] == b'1.0.3\n'
    release = json.loads(files[P + 'RELEASE.json'])
    assert release['version'] == VERSION
    assert release['title_id'] == 'PPSA83600' and release['title_name'] == 'PS5X360Reloaded'
    assert release['executable_sha256'] == sha(eboot())
    assert release['candidate'] is False and release['published'] is True
    assert release['candidate_id'] == 'src-' + sha(eboot())[:8]
    assert release['build'] == 'source'
    assert release['source_commit'] == COMMIT and release['source_url'] == SOURCE_URL
    assert release['source_repository'] == 'https://github.com/example/PS5X360Reloaded'
    assert release['base_package']['sha256'] == base_sha
    assert release['drops'] == {'v1.0.2': 'd9bd' + '0' * 60}
    assert release['fixes'] == ['01-jit-reserved-barriers.patch', '25-english-source-text.patch']
    assert release['fixes_skipped'] == []
    assert release['sources_json_sha256'] == sha(SOURCES_BYTES)
    assert release['source_assets'] == [f'PS5X360-{VERSION}-source.tar.xz',
                                        f'PS5X360-{VERSION}-toolchain-source.tar.xz',
                                        f'PS5X360Reloaded-{VERSION}-scripts.tar.gz']
    assert release['toolchain']['PS5_Vulkan']['commit'] == TOOLCHAIN['PS5_Vulkan']['commit']

    text = files[P + f'licenses/PS5X360-{VERSION}-NOTICE.txt'].decode()
    for needle in ('GNU General Public License', 'version 3', 'licenses/GPL-3.0.txt',
                   'Modified version (GPL-3.0 section 5)', f'modified {DATE}',
                   f'https://github.com/example/PS5X360Reloaded commit {COMMIT}',
                   'https://github.com/example/PS5X360Reloaded/releases/tag/v1.0.3',
                   sha(eboot()), sha(SOURCES_BYTES),
                   'PS5X360-source-1.0.2-changes.zip (v1.0.2)  sha256 d9bd' + '0' * 60,
                   'xenia-canary.git d2cf527aa', '01-jit-reserved-barriers.patch  sha256 ' + '1' * 64,
                   f'PS5X360-{VERSION}-source.tar.xz', f'PS5X360Reloaded-{VERSION}-scripts.tar.gz',
                   f'PS5X360-{VERSION}-toolchain-source.tar.xz',
                   'PS5_Vulkan  https://github.com/mihawk-99/PS5_Vulkan commit ' + TOOLCHAIN['PS5_Vulkan']['commit'],
                   'PS5_Mesa', 'PS5_PayloadSDK', 'OpenGL renderer stack', 'USB Kinect', 'disc',
                   'NOT part of this executable', 'wolfSSL', 'ps5-framegen',
                   'licenses/MBEDTLS-3.6.7-LICENSE.txt', 'licenses/CACERT-MPL-2.0.txt', 'MPL-2.0',
                   'Apache-2.0', base_sha,
                   # The toolchain parts linked into eboot.bin are named, with where their source is.
                   'RADV', 'app_crt.cpp', 'payload SDK runtime', 'ps5-native-tool',
                   'at the commits listed under Toolchain',
                   # The new identity, and the upstream attribution kept as it is.
                   'PS5X360Reloaded 1.0.3 (PPSA83600)', 'UP9000-PPSA83600_00-PS5X360RELOADED0',
                   'moved from PPSA50011/ to PPSA83600/', 'tools/source-1.0.2/package-art',
                   'This is a modified version of PS5X360 1.0.2 by BrinooTk (https://github.com/BrinooTk/PS5X360/releases)',
                   'Source drops (BrinooTk/PS5X360 releases)', 'the PS5X360\n    code is MIT (LICENSE)'):
        assert needle in text, needle
    assert 'complete source tree' not in text
    assert 'candidate' not in text.lower()

    # Licence texts verbatim from tools/source-1.0.2/licenses, checked by SHA-256.
    readme = (REPO / 'tools/source-1.0.2/licenses/README.md').read_text(encoding='utf-8')
    for name, target, digest in package.LICENCES:
        data = (REPO / 'tools/source-1.0.2/licenses' / name).read_bytes()
        assert sha(data) == digest and b'\r' not in data, name
        assert files[P + target] == data, target
        assert f'`{name}`' in readme and digest in readme, name
    assert files[P + 'licenses/GPL-3.0.txt'].startswith(b'                    GNU GENERAL PUBLIC LICENSE\n'
                                                       b'                       Version 3, 29 June 2007')
    assert files[P + 'licenses/MINIUPNPC.txt'].startswith(b'BSD 3-Clause License')
    assert b'Apache License' in files[P + 'licenses/MBEDTLS-3.6.7-LICENSE.txt']
    assert files[P + 'licenses/CACERT-MPL-2.0.txt'].startswith(b'Mozilla Public License Version 2.0')

    manifest = json.loads(files[P + 'MANIFEST.json'])
    assert set(manifest) == names - {'MANIFEST.json'}
    for name, record in manifest.items():
        assert record == {'bytes': len(files[P + name]), 'sha256': sha(files[P + name])}, name


def check_output(out, files):
    out_zip = out / 'PPSA83600.zip'
    with zipfile.ZipFile(out_zip) as z:
        assert z.testzip() is None
        infos = z.infolist()
        assert [i.filename for i in infos] == sorted(files)
        for info in infos:
            assert info.date_time == package.ZIP_TIME and info.external_attr == 0o644 << 16, info
            assert info.create_system == 3, info  # the same bytes on Windows and Linux
            assert z.read(info) == files[info.filename], info.filename
    stage = out / 'stage'
    staged = {p.relative_to(stage).as_posix(): p.read_bytes() for p in stage.rglob('*') if p.is_file()}
    assert staged == files
    assert (stage / 'PPSA83600' / 'eboot.bin').is_file() and not (stage / 'PPSA50011').exists()
    assert sorted(p.name for p in stage.iterdir()) == ['PPSA83600']
    assert (out / 'PPSA83600.zip.sha256').read_text(encoding='utf-8') == \
        f'{sha(out_zip.read_bytes())}  PPSA83600.zip\n'
    assert not (out / 'PPSA50011.zip').exists() and not (out / 'PPSA50011.zip.sha256').exists()


def main():
    files = build()
    check_files(files)
    print('[OK] layout, eboot, English-keyed lang, repository assets, VERSION, RELEASE.json, NOTICE, licences')

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        first = package.write_package(files, tmp / 'a')
        second = package.write_package(build(), tmp / 'b')
        check_output(tmp / 'a', files)
        assert first == second
        assert (tmp / 'a/PPSA83600.zip').read_bytes() == (tmp / 'b/PPSA83600.zip').read_bytes()
        print('[OK] deterministic zip, stage and .sha256')

        # The command line, end to end, with the fixture's hash in place of the official one.
        zip_path = tmp / 'base.zip'
        digest = sha(base_zip(zip_path))
        (tmp / 'eboot.bin').write_bytes(eboot())
        (tmp / 'SOURCES.json').write_bytes(SOURCES_BYTES)
        (tmp / 'lang').mkdir()
        for name, data in LANG.items():
            (tmp / 'lang' / name).write_bytes(data)
        argv = ['--base-zip', str(zip_path), '--eboot', str(tmp / 'eboot.bin'), '--lang-dir', str(tmp / 'lang'),
                '--sources-json', str(tmp / 'SOURCES.json'), '--version', VERSION,
                '--source-url', SOURCE_URL, '--date', DATE]
        stdout = sys.stdout
        try:
            sys.stdout = io.StringIO()
            try:
                package.main(argv + ['--out-dir', str(tmp / 'cli-official')])
            except SystemExit as error:
                assert 'is not the official v1.0.2 PPSA50011.zip' in str(error.code), error.code
            else:
                raise AssertionError('a base zip other than the official one was accepted')
            official = package.BASE_ZIP_SHA256
            package.BASE_ZIP_SHA256 = digest
            try:
                assert package.main(argv + ['--out-dir', str(tmp / 'cli')]) == 0
                assert package.main(argv + ['--out-dir', str(tmp / 'cli2')]) == 0
            finally:
                package.BASE_ZIP_SHA256 = official
            summary = json.loads(sys.stdout.getvalue().splitlines()[0])
        finally:
            sys.stdout = stdout
        cli_files = {p.relative_to(tmp / 'cli/stage').as_posix(): p.read_bytes()
                     for p in (tmp / 'cli/stage').rglob('*') if p.is_file()}
        check_files(cli_files, base_sha=digest)
        check_output(tmp / 'cli', cli_files)
        assert (tmp / 'cli/PPSA83600.zip').read_bytes() == (tmp / 'cli2/PPSA83600.zip').read_bytes()
        assert summary['version'] == VERSION and summary['eboot_sha256'] == sha(eboot())
        assert summary['title_id'] == 'PPSA83600' and summary['package'].endswith('PPSA83600.zip')
        print('[OK] command line: official zip gate, package, same zip twice')

    for version in ('1.0.3_candidate_37', '', 'v1.0.3', '1.0', '1.0.3.1', '1.0.3 '):
        refused('is not X.Y.Z', version=version)
    refused('carries a candidate label', eboot=eboot(extra=b'1.0.3_candidate_37\0'))
    refused("label 'PS5X360Reloaded v1.0.3'", eboot=eboot('1.0.3_candidate_37'))
    refused("label 'PS5X360Reloaded v1.0.3'", eboot=eboot('1.0.30'))
    # Built without the rename: the official app's label, alone or next to the new one.
    refused("label 'PS5X360Reloaded v1.0.3'", eboot=eboot().replace(b'PS5X360Reloaded v', b'PS5X360 v'))
    refused("official app's label", eboot=eboot(extra=b'PS5X360 v1.0.3\0'))
    refused("official app's label", eboot=eboot(extra=b'PS5X360 v1.0.2\0'))
    refused("label 'BOOT version 1.0.3'", eboot=eboot().replace(b'BOOT version', b'BOOT versio_'))
    refused('not a signed PS5 SELF', eboot=b'\x7fELF' + eboot()[4:])
    refused('lists skipped fixes', sources=dict(SOURCES, fixes_skipped=['tools/source-1.0.2/fixes/01-x.patch']))
    refused("SOURCES.json version '1.0.3_candidate_37'", sources=dict(SOURCES, version='1.0.3_candidate_37'))
    refused('SOURCES.json version None', sources={k: v for k, v in SOURCES.items() if k != 'version'})
    refused('not from a release assembly', sources=dict(SOURCES, release=False))
    refused('not from a release assembly', sources={k: v for k, v in SOURCES.items() if k != 'release'})
    refused('has no fixes_skipped list', sources={k: v for k, v in SOURCES.items() if k != 'fixes_skipped'})
    refused('lists skipped fixes', sources=dict(SOURCES, fixes_skipped=None))
    refused("has no 'fixes'", sources=dict(SOURCES, fixes=[]))
    refused('no English-keyed file for PPSA83600/assets/lang/pl.txt',
            lang={k: v for k, v in LANG.items() if k != 'pl.txt'})
    refused('pl.txt is not an English-keyed', lang=dict(LANG, **{'pl.txt': PORTUGUESE['pl.txt']}))
    refused('de.txt is not an English-keyed', lang=dict(LANG, **{'de.txt': english('pl')}))
    refused('no language files', lang={})
    refused('pl.txt is not an English-keyed', lang=dict(LANG, **{'pl.txt': b'\xff\xfe#\0 \0P\0'}))
    refused('base package VERSION is not 1.0.2', base_files=dict(base_files(), **{B + 'VERSION': b'1.0.3\n'}))
    # The base is read under PPSA50011/ only: a stage already moved, or a stray file, is refused.
    refused('base package files outside PPSA50011/: PPSA83600/VERSION',
            base_files=dict(base_files(), **{P + 'VERSION': b'1.0.2\n'}))
    refused('base package files outside PPSA50011/: README.txt', base_files=dict(base_files(), **{'README.txt': b'x'}))
    refused('base package has no sce_sys/param.json',
            base_files={k: v for k, v in base_files().items() if k != B + 'sce_sys/param.json'})
    # param.json must be the official app's: a param of another title (e.g. a package made by
    # this script) or a malformed one stops the packaging.
    for param, message in ((RELOADED_PARAM, "titleId 'PPSA83600' is not PPSA50011"),
                           (b'{"titleId": "PPSA50011"', 'is not JSON'),
                           (b'[]', 'is not a JSON object'),
                           (OFFICIAL_PARAM.replace(b'  "conceptId": "50011",\r\n', b''), 'has no conceptId'),
                           (OFFICIAL_PARAM.replace(b'"defaultLanguage": "en-US"', b'"defaultLanguage": "fr-FR"'),
                            'no localizedParameters entry for its defaultLanguage'),
                           (b'{"titleId": "PPSA50011", "conceptId": "50011", "contentId": "x"}',
                            'no localizedParameters entry')):
        refused(message, base_files=dict(base_files(), **{B + 'sce_sys/param.json': param}))
    # Every language entry gets the new name; LF files stay LF, other fields stay as they are.
    several = {'titleId': 'PPSA50011', 'conceptId': '50011', 'contentId': 'UP9000-PPSA50011_00-XBOX360PS5M80001',
               'localizedParameters': {'defaultLanguage': 'en-US', 'en-US': {'titleName': 'PS5X360'},
                                       'pl-PL': {'titleName': 'PS5X360', 'titleNameSort': 'x'}, 'ja-JP': {}},
               'downloadDataSize': 256}
    made = package.param_json(json.dumps(several, indent=2).encode() + b'\n')
    assert b'\r' not in made and made.endswith(b'}\n'), made
    made = json.loads(made)
    assert made['localizedParameters'] == {
        'defaultLanguage': 'en-US', 'en-US': {'titleName': 'PS5X360Reloaded'},
        'pl-PL': {'titleName': 'PS5X360Reloaded', 'titleNameSort': 'x'}, 'ja-JP': {'titleName': 'PS5X360Reloaded'}}
    assert made['downloadDataSize'] == 256 and made['titleId'] == 'PPSA83600'
    assert package.param_json(OFFICIAL_PARAM) == RELOADED_PARAM
    # Art that is not the reviewed files (here: wrong pinned digest or size) stops the packaging.
    art = package.ART
    try:
        package.ART = [(n, t, s, '0' * 64 if n == 'pic0.png' else d) for n, t, s, d in art]
        refused('package-art/pic0.png is not the reviewed file')
        package.ART = [(n, t, (256, 256) if n == 'icon0.png' else s, d) for n, t, s, d in art]
        refused('package-art/icon0.png is not a 256x256 PNG')
    finally:
        package.ART = art
    # The label package.py wants is the one check_release_elf.py checks after the link.
    assert package.HOME_LABEL == check_release_elf.LABELS[0] == b'PS5X360Reloaded v'
    assert check_release_elf.OFFICIAL_LABEL == b'PS5X360 v'
    for url in ('https://github.com/example/PS5X360Reloaded', 'https://github.com/example/repo/tree/main',
                f'https://github.com/example/repo/tree/{COMMIT[:7]}', ''):
        refused('is not https://', source_url=url)
    # A licence text that is not the verbatim one (here: a wrong pinned digest) stops the packaging.
    licences = package.LICENCES
    package.LICENCES = [(name, target, '0' * 64) if name == 'GPL-3.0.txt' else (name, target, digest)
                        for name, target, digest in licences]
    try:
        refused('licenses/GPL-3.0.txt is not the verbatim text')
    finally:
        package.LICENCES = licences
    print('[OK] refusals')

    try:
        package.toolchain_pins('env:\n  PS5_MESA_REV: ' + '0' * 40 + '\n')
    except SystemExit as error:
        assert 'PS5_VULKAN_REV' in str(error.code)
    else:
        raise AssertionError('missing toolchain pin accepted')
    for key in ('PS5_Vulkan', 'PS5_Mesa', 'PS5_PayloadSDK'):
        assert len(TOOLCHAIN[key]['commit']) == 40, key
    # The optional pins must still be found in build-source-1.0.2.yml, or the NOTICE loses them.
    for key in ('Vulkan SDK', 'zlib'):
        assert len(TOOLCHAIN[key]['sha256']) == 64 and TOOLCHAIN[key]['version'], key
    try:
        package.modification_date(None, {'SOURCE_DATE_EPOCH': '17950OOOOO'})
    except SystemExit as error:
        assert 'SOURCE_DATE_EPOCH' in str(error.code), error.code
    else:
        raise AssertionError('malformed SOURCE_DATE_EPOCH accepted')
    assert package.modification_date(None, {'SOURCE_DATE_EPOCH': '1795000000'}) == '2026-11-18'
    assert package.modification_date('2026-11-20', {}) == '2026-11-20'
    try:
        package.modification_date('20.11.2026', {})
    except SystemExit:
        pass
    else:
        raise AssertionError('malformed date accepted')
    print('[OK] toolchain pins and modification date')
    print('check-source-package: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
