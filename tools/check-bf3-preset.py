"""Host check of fix 30's title side on an assembled 1.0.2 tree.

    python3 -I tools/check-bf3-preset.py --tree <assembled tree>

Battlefield 3 (45410950) hangs once a level starts unless memory exports are read back
(research/bf3/ANALYSIS.md), and the official presets.txt has no entry for it, so the launcher's
built-in preset table (GamePreset in src/launcher.cpp, below the player's own per-game choices)
must give it memexport=1, and the "memexport" option must be one a preset may set. Its heavy
diagnostics (the job-manager probes, the per-dword export lines, the shader dump: about 2,500
lines in a level's first seconds) must only be switched on with detailed logs.
"""
import argparse
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--tree', type=Path, required=True)
    args = parser.parse_args()
    launcher = (args.tree / 'src/launcher.cpp').read_text(encoding='utf-8', errors='replace')
    game_main = (args.tree / 'src/canary_game_main.cpp').read_text(encoding='utf-8', errors='replace')

    # The built-in preset row, inside GamePreset's table.
    start = launcher.index('built_in[] = {', launcher.index('GameOverrides GamePreset(const std::string& title_id)'))
    table = launcher[start:launcher.index('\n  };', start)]
    rows = dict(re.findall(r'\{"([0-9A-Fa-f]{8})", \{([^}]*\}(?:, \{[^}]*\})*)\}\}', table))
    assert '45410950' in rows, sorted(rows)
    values = dict((k, int(v)) for k, v in re.findall(r'\{"(\w+)", (\d+)', rows['45410950']))
    assert values == {'memexport': 1}, values
    # The option exists, may be set per game (the field after the label's default), and maps to
    # the core's readback_memexport.
    option = re.search(r'\{"memexport", &S::memexport, (\d+), W::(\w+), (\d+), (true|false),', launcher)
    assert option and option.group(4) == 'true', option and option.groups()
    assert 'cvars::readback_memexport = memexport != 0;' in launcher
    # The presets are applied below the player's own choices.
    assert 'WithOverrides(WithOverrides(general, GamePreset(title_id)), own)' in launcher

    # The diagnostics: Battlefield 3 and detailed logs only, for both switches.
    gate = re.search(r'const bool bf3_diagnostics =\s*std::strtoul\(log_id\.c_str\(\), nullptr, 16\) == '
                     r'0x45410950ul && cvars::log_level >= 3;', game_main)
    assert gate, 'the BF3 diagnostics are not gated on detailed logs'
    assert 'xbox360ps5::entry_probe::Global().Enable(bf3_diagnostics);' in game_main
    assert 'xbox360ps5::memexport_merge::DetailEnabled().store(bf3_diagnostics);' in game_main
    assert game_main.count('entry_probe::Global().Enable(') == 1
    assert game_main.count('DetailEnabled().store(') == 1
    print('PASS: Battlefield 3 gets memory export readback from the built-in presets, its diagnostics only '
          'with detailed logs (fix 30)')


if __name__ == '__main__':
    main()
