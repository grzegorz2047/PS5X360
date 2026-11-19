"""Host check of fix 25 (English keys for the 1.0.2 source rebuild's interface).

    python3 -I tools/check-english-source.py --tree build/src102 [--lang tools/source-1.0.2/english/lang]

--tree is an assembled tree (tools/source-1.0.2/assemble.py) with fix 25 applied; the tree before
it is made by reversing fix 25 in a copy. Checks:
  (a) tools/source-1.0.2/english/rekey.py --check: fix 25 and the language files are what the
      generator makes from the tree (fixes after fix 25 reversed in a copy), the official files'
      snapshot and english/extra.txt, and the snapshot is the one SOURCES.json records;
  (b) no Portuguese key is left in the code (any string literal of the title's code, not only
      those passed to Tr), nothing that reads as Portuguese outside the language picker's native
      names, no FramegenText, and the lookup code is the one this check simulates;
  (c) every English key the code uses (fix 25's, extra.txt's [added <code>] ones, and every
      literal the tree passes to Tr, fixes after fix 25 included) is in pt-BR.txt and in each of
      the 13 full language files (MISSING_ALLOWED lists the ones no official file had);
  (d) each language file: UTF-8 without BOM, LF, "\\s" for spaces, "key = value", no duplicate or
      unknown key, no empty value, the printf specifiers of the English in the same order, the
      English's leading and trailing spaces;
  (e) display equivalence: for every interface language and every text the code looks up (each
      Tr key, each FramegenText call, each CoreUiText row), the text the old lookup shows
      (Portuguese keys: the official package's files merged with the fork's overrides, as the
      binary release ships them, english/official-lang; Tr's built-in table; FramegenText's and
      CoreUiText's own English and Spanish; fix 23's fallbacks, exactly as in the tree before fix
      25) is the text the new lookup shows (English keys, english/lang), except Portuguese that
      leaked into another language, which now shows English or a translation.
The old side's data comes from the tree before fix 25 (built-in tables) and the committed snapshot
english/official-lang, so no package download is needed.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
ENGLISH = REPO / 'tools/source-1.0.2/english'
sys.path.insert(0, str(ENGLISH))
import rekey  # noqa: E402

# The lookup code the simulation below reproduces (before fix 25, i.e. fixes 23 and 24 applied).
OLD_CODE = {
    rekey.I18N: [
        '  if (language == 9) return key;\n  if (const char* loaded = LoadedUiText(key)) return loaded;\n',
        '  if (language == 19) return key;\n  const Entry* built_in = nullptr;\n  for (const auto& entry : entries) {\n'
        '    if (!std::strcmp(entry.pt, key)) { built_in = &entry; break; }\n  }\n'
        '  if (built_in && (language == 5 || language == 18)) return built_in->es;\n  if (language != 1) {\n'
        '    if (const char* english = EnglishUiText(key)) return english;\n  }\n'
        '  if (built_in) return built_in->en;\n  return key;',
    ],
    rekey.LAUNCHER: [
        '  if (language == 9) return pt;\n  if (const char* loaded = LoadedUiText(pt)) return loaded;\n'
        '  if (language == 19) return pt;\n  if (language == 5 || language == 18) return es;\n'
        '  // Fix 23: English as assets/lang/en.txt has it, else this function\'s own.\n'
        '  if (const char* english = EnglishUiText(pt)) return english;\n  return en;\n}',
    ],
    rekey.GAME_MAIN: [
        '    if (std::strcmp(entry.en, english)) continue;\n'
        '    // The other languages translate the Portuguese text (assets/lang).\n'
        '    if (language == 9 || language == 19) return entry.pt;\n'
        '    if (const char* loaded = xbox360ps5::LoadedUiText(entry.pt)) return loaded;\n'
        '    return language == 5 || language == 18 ? entry.es : entry.en;\n  }\n  return english;',
    ],
    rekey.I18N_UI: [
        '  if (id != 9) i18n_ui::Read(id, *table);\n',
        '    i18n_ui::Read(1, table);\n',
    ],
}
NEW_CODE = {
    rekey.I18N: [rekey.NEW_TR],
    rekey.I18N_UI: [
        '  table->folder = i18n_ui::Folder();\n  i18n_ui::Read(id, *table);\n',
        '  if (id == 9) {\n    static const i18n_ui::Table brazilian = read_file(9);\n    table = &brazilian;\n'
        '  } else if (id == 5) {\n    static const i18n_ui::Table spanish = read_file(5);\n    table = &spanish;\n'
        '  } else {\n    static const i18n_ui::Table english = read_file(1);\n    table = &english;\n  }\n',
        '  const char* mark = std::strstr(key, "||");\n',
    ],
    rekey.GAME_MAIN: ['    if (!std::strcmp(entry.core, english)) return xbox360ps5::Tr(entry.key);\n  }\n  return english;'],
}
# English keys the code uses that a full language file may lack, with the reason.
MISSING_ALLOWED = {
    'The game asks where to keep its saved data.':
        'CoreUiText only; in no official language file (shown in English, Spanish from CoreUiText, as before)',
}
# Intended differences other than Portuguese leaks: (kind, Portuguese key, language) -> why.
# CoreUiText gave Portugal's Portuguese the Brazilian text although pt.txt translates it; Tr
# reads pt.txt for every text, so the core's windows now do too.
ALLOWED_DIFFERENCES = {
    ('CoreUiText', pt, 'pt'): "pt-PT: the core's windows use pt.txt (CoreUiText showed the Brazilian text)"
    for pt in ('Onde o jogo deve salvar?', 'Controle 1', 'Controle 2', 'Controle 3', 'Controle 4')
}
# Literals that read as Portuguese but are not: the language picker's native names.
PORTUGUESE_LOOK = re.compile(r'[ãõçÃÕÇ]|[áéíóúâêôà]\w|\b(?:não|você|está|jogos?|configura\w*|ligad[oa]s?|'
                             r'desligad[oa]s?|ativad[oa]s?|desativad[oa]s?|opç\w*|também|então|aguarde|baixando|'
                             r'capas?|rede|voltar|fechar|jogar|abrir|nenhum|nenhuma|instalad[oa]s?|novos?|novas?)\b',
                             re.I)
LOOK_ALLOWED = rekey.NATIVE_NAMES
PRINTF = re.compile(r'%[-+ #0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|q|j|z|t)?[diouxXeEfFgGaAcspn%]')
# The zh files end "Busca: " with a full-width colon and no space; the colon carries the space.
EDGE_ALLOWED = re.compile(r'[：]$')


class Problems(list):
    def add(self, kind, text):
        self.append(f'{kind}: {text}')


def check_rekey(tree, problems):
    result = subprocess.run([sys.executable, '-I', str(ENGLISH / 'rekey.py'), '--tree', str(tree), '--check'],
                            capture_output=True, text=True, encoding='utf-8')
    print(result.stdout, end='')
    if result.returncode:
        print(result.stderr, end='', file=sys.stderr)
        problems.add('a', 'rekey.py --check failed')
    return result.returncode == 0


def check_code(before, after, data, problems):
    for files, wanted, label in ((before.files, OLD_CODE, 'before fix 25'), (after, NEW_CODE, 'with fix 25')):
        for rel, snippets in wanted.items():
            for snippet in snippets:
                if snippet not in files[rel].replace('\r\n', '\n'):
                    problems.add('b', f'{rel} {label}: the lookup code this check simulates is gone: {snippet[:70]!r}')
    portuguese = {pt for pt, key in data.key.items() if key != pt}
    left = look = 0
    for rel, text in after.items():
        for tok in rekey.lex(text, rel):
            if tok.kind == 'id' and tok.value == 'FramegenText':
                problems.add('b', f'{rel}:{rekey.line_of(text, tok.start)}: FramegenText is still used')
            if tok.kind != 'str' or tok.raw:
                continue
            if tok.value in portuguese:
                left += 1
                problems.add('b', f'{rel}:{rekey.line_of(text, tok.start)}: Portuguese key {tok.value!r}')
            elif PORTUGUESE_LOOK.search(tok.value) and tok.value not in LOOK_ALLOWED:
                look += 1
                problems.add('b', f'{rel}:{rekey.line_of(text, tok.start)}: reads as Portuguese: {tok.value!r}')
    print(f'(b) code: {len(after)} files, Portuguese keys left {left}, other Portuguese-looking texts {look}')
    # Every English key of fix 25 appears as a literal in the tree fix 25 makes (fixes after it may
    # drop one: it stays in the language files, unused), every [added] key in the whole tree.
    def strings(files):
        found = set()
        for rel, text in files.items():
            found |= {tok.value for tok in rekey.lex(text, rel) if tok.kind == 'str' and not tok.raw}
        return found
    present = strings(after)
    present_25 = strings({**before.files, **rekey.rewrite(data)[0]})
    added = {key for section in data.added.values() for key in section}
    keys = set(data.key.values()) | set(data.core_key.values()) | added
    for key in sorted((keys - added) - present_25) + sorted(added - present):
        problems.add('b', f'English key {key!r} is not in the code')
    dropped = sorted((keys - added) & present_25 - present)
    if dropped:
        print(f'    {len(dropped)} keys of fix 25 no longer in the code (a later fix dropped them; still in '
              f'the language files): {dropped[:5]}')
    # Every literal the tree passes to Tr is a key as well, also those of fixes after fix 25 (or
    # of any text added since), which the Portuguese map does not know: (c) and (d) check them.
    tr_literals = set()
    for rel, text in after.items():
        _, lits = rekey.literals(text, rel)
        tr_literals |= {lit.value for lit in lits if lit.call and lit.call[0] == 'Tr' and not lit.raw}
    print(f'    keys: {len(keys)} from fix 25 and extra.txt [added], {len(tr_literals)} literals passed to Tr, '
          f'{len(tr_literals - keys)} of them not from the map')
    return keys | tr_literals


def check_coverage(keys, lang, problems):
    counts = {}
    for code in ['pt-BR'] + rekey.FULL_CODES:
        lacking = sorted(key for key in keys if key not in lang[code] and key not in MISSING_ALLOWED)
        counts[code] = len(lacking)
        for key in lacking:
            problems.add('c', f'{code}.txt lacks {key!r}')
    allowed = {key: sum(key not in lang[code] for code in rekey.FULL_CODES) for key in MISSING_ALLOWED}
    print(f'(c) coverage: {len(keys)} English keys in pt-BR and {len(rekey.FULL_CODES)} full files; '
          f'missing {sum(counts.values())}; allowed: ' +
          '; '.join(f'{key!r} absent from {n} files ({MISSING_ALLOWED[key]})' for key, n in allowed.items()))


def check_files(folder, keys, problems):
    lang, entries, stats = {}, 0, {'printf': 0, 'edge': 0, 'full-width colon': 0}
    for code in sorted(rekey.LANGUAGES.values()):
        path = folder / f'{code}.txt'
        if not path.exists():
            problems.add('d', f'{code}.txt missing')
            lang[code] = {}
            continue
        data = path.read_bytes()
        if data.startswith(b'\xef\xbb\xbf'):
            problems.add('d', f'{code}.txt has a BOM')
        if b'\r' in data:
            problems.add('d', f'{code}.txt has CR line ends')
        try:
            text = data.decode('utf-8')
        except UnicodeDecodeError as error:
            problems.add('d', f'{code}.txt is not UTF-8: {error}')
            continue
        if not text.endswith('\n'):
            problems.add('d', f'{code}.txt does not end with a new line')
        table = {}
        for number, line in enumerate(text.split('\n')[:-1], 1):
            if line.startswith('#'):
                continue
            where = f'{code}.txt:{number}'
            if line.count(' ') != 2 or ' = ' not in line:
                problems.add('d', f'{where}: not "key = value" with \\s for spaces')
                continue
            raw_key, raw_value = line.split(' = ')
            key, value = rekey.unescape(raw_key), rekey.unescape(raw_value)
            if rekey.escape(key) != raw_key or rekey.escape(value) != raw_value:
                problems.add('d', f'{where}: not in the canonical escaping')
            if key in table:
                problems.add('d', f'{where}: {key!r} twice')
            if not value:
                problems.add('d', f'{where}: {key!r} has an empty value')
            if key not in keys:
                problems.add('d', f'{where}: {key!r} is no key of the code')
            table[key] = value
            english = rekey.strip_context(key)
            stats['printf'] += bool(PRINTF.findall(english))
            stats['edge'] += english != english.strip(' ')
            if PRINTF.findall(value) != PRINTF.findall(english):
                problems.add('d', f'{where}: printf {PRINTF.findall(value)} for {PRINTF.findall(english)} in {key!r}')
            for edge, english_edge, translated_edge in (
                    ('leading', re.match(r'\s*', english).group(), re.match(r'\s*', value).group()),
                    ('trailing', re.search(r'\s*$', english).group(), re.search(r'\s*$', value).group())):
                if english_edge != translated_edge and edge == 'trailing' and EDGE_ALLOWED.search(value):
                    stats['full-width colon'] += 1
                elif english_edge != translated_edge:
                    problems.add('d', f'{where}: {edge} spaces {english_edge!r} -> {translated_edge!r} in {key!r}')
            if code == 'en' and value != english:
                problems.add('d', f'{where}: en.txt gives {value!r} for {key!r}, not its English')
        lang[code] = table
        entries += len(table)
    summary = (f'(d) files: {len(lang)} language files, {entries} entries: format, escaping, no duplicate, '
               f'unknown or empty entry; printf specifiers checked in {stats["printf"]} entries, edge spaces in '
               f'{stats["edge"]} (allowed: a full-width colon without the space, {stats["full-width colon"]})')
    return lang, summary


# --- (e) the old and the new lookup ------------------------------------------------------------
def old_text(kind, site, language, official, builtin):
    """What the tree before fix 25 shows (fixes 23 and 24: i18n.hpp Tr, launcher.cpp
    FramegenText, canary_game_main.cpp CoreUiText)."""
    code = rekey.CODE_OF[language]
    loaded = official.get(code, {}) if language != 9 else {}
    english_file = official['en']
    pt = site['pt']
    if kind == 'Tr':
        if language == 9:
            return pt
        if pt in loaded:
            return loaded[pt]
        if language == 19:
            return pt
        row = builtin.get(pt)
        if row and language in (5, 18):
            return row[1]
        if language != 1 and pt in english_file:
            return english_file[pt]
        return row[0] if row else pt
    if kind == 'FramegenText':
        if language == 9:
            return pt
        if pt in loaded:
            return loaded[pt]
        if language == 19:
            return pt
        if language in (5, 18):
            return site['es']
        if pt in english_file:
            return english_file[pt]
        return site['en']
    # CoreUiText
    if language in (9, 19):
        return pt
    if pt in loaded:
        return loaded[pt]
    return site['es'] if language in (5, 18) else site['en']


def new_text(key, language, lang):
    """What fix 25's Tr shows (i18n.hpp, reconstructed/xbox360ps5/i18n_ui.hpp)."""
    loaded = lang.get(rekey.CODE_OF[language], {})
    if key in loaded:
        return loaded[key]
    if language == 19 and key in lang['pt-BR']:
        return lang['pt-BR'][key]
    if language == 18 and key in lang['es']:
        return lang['es'][key]
    if language != 1 and key in lang['en']:
        return lang['en'][key]
    return rekey.strip_context(key)


def check_equivalence(data, lang, problems):
    before, official = data.before, data.official
    extra_translations = rekey.load_extra()  # [<code>] Portuguese = translation
    sites = []
    for pt, kinds in sorted(data.used.items()):
        if 'tr' in kinds:
            sites.append(('Tr', {'pt': pt}, data.key[pt]))
    seen = set()
    for site in before.framegen:
        if (site['pt'], site['en'], site['es']) not in seen:
            seen.add((site['pt'], site['en'], site['es']))
            sites.append(('FramegenText', site, data.key[site['pt']]))
    for row in before.core:
        sites.append(('CoreUiText', row, data.core_key[row['core']]))
    kinds = {}
    pairs = same = 0
    for kind, site, key in sites:
        pt = site['pt']
        for language in sorted(rekey.LANGUAGES):
            old = old_text(kind, site, language, official, before.builtin)
            new = new_text(key, language, lang)
            pairs += 1
            if old == new:
                same += 1
                continue
            code = rekey.CODE_OF[language]
            english = new_text(key, 1, lang)
            if old == pt and language not in (9, 19) and pt != english:
                category = ('Portuguese leak -> English' if new == english else
                            'Portuguese leak -> translation')
            elif ((old == english or (language == 19 and old == pt)) and language not in (1, 9)
                  and pt not in official.get(code, {})
                  and new == extra_translations.get(code, {}).get(pt)):
                # The language's file never had the text, so the old build fell back to the
                # English (Portugal's Portuguese: to the Brazilian source); the new files carry the translation written in english/extra.txt
                # (fix 22's network row: Rede, Online – conectado, Online – servidor não alcançado).
                category = 'English fallback -> translation (extra.txt)'
            elif (kind, pt, code) in ALLOWED_DIFFERENCES:
                category = ALLOWED_DIFFERENCES[(kind, pt, code)]
            else:
                category = 'UNEXPECTED'
                problems.add('e', f'{kind} {pt!r} in {code}: {old!r} -> {new!r}')
            kinds.setdefault(category, []).append((code, kind, pt, old, new))
    print(f'(e) display equivalence: {len(sites)} lookups (Tr keys, FramegenText calls, CoreUiText rows) x '
          f'{len(rekey.LANGUAGES)} languages = {pairs} pairs; identical {same}')
    unused = sorted(set(ALLOWED_DIFFERENCES) - {(kind, pt, code) for rows in kinds.values()
                                                  for code, kind, pt, _, _ in rows})
    for item in unused:
        problems.add('e', f'allowed difference that no longer occurs: {item}')
    for category, rows in sorted(kinds.items()):
        by_language = {}
        for code, *_ in rows:
            by_language[code] = by_language.get(code, 0) + 1
        print(f'    {category}: {len(rows)} (' + ', '.join(f'{c} {n}' for c, n in sorted(by_language.items())) + ')')
        for pt in sorted({pt for _, _, pt, _, _ in rows}):
            shown = sorted({f'{code}: {new!r}' for code, _, p, _, new in rows if p == pt})
            print(f'      {pt!r} -> ' + '; '.join(shown[:4]) +
                  (f'; ... ({len(shown)} languages)' if len(shown) > 4 else ''))
    return pairs


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--tree', type=Path, required=True, help='assembled 1.0.2 tree with fix 25')
    parser.add_argument('--lang', type=Path, default=rekey.LANG_DIR, help='the English-keyed language files')
    args = parser.parse_args()
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    problems = Problems()
    tree = args.tree.resolve()
    check_rekey(tree, problems)
    with tempfile.TemporaryDirectory(prefix='check-english-') as temp:
        before_dir, reversed_fix = rekey.before_tree(tree, Path(temp))
        if not reversed_fix:
            sys.exit(f'{args.tree}: fix 25 is not applied (assemble the tree with tools/source-1.0.2/fixes)')
        before = rekey.Before(rekey.read_tree(before_dir))
    after = rekey.read_tree(tree)
    data = rekey.Rekey(before, rekey.load_official(), rekey.load_extra())
    keys = check_code(before, after, data, problems)
    lang, summary = check_files(args.lang, keys, problems)
    check_coverage(keys, lang, problems)
    print(summary)
    check_equivalence(data, lang, problems)
    if problems:
        print(f'FAIL: {len(problems)} problems', file=sys.stderr)
        for line in problems:
            print('  ' + line, file=sys.stderr)
        sys.exit(1)
    print('PASS: the interface is keyed by English and shows what it showed, Portuguese leaks fixed (fix 25)')


if __name__ == '__main__':
    main()
