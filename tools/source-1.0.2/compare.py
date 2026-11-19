"""Compare a rebuilt eboot.elf with the official v1.0.2 one.

    python -I tools/source-1.0.2/compare.py <official.elf> <rebuilt.elf> [--json out.json]

Both are the ELF inside eboot.bin (the build's eboot.elf; for the official package, the ELF
extracted from PPSA50011/eboot.bin). The images have no section headers, so the comparison is
by program segment (code, read-only data, data, bss), by printable strings, and by marker
strings of features.
"""
import json
import re
import struct
import sys
from pathlib import Path

PT = {1: 'LOAD', 2: 'DYNAMIC', 4: 'NOTE', 7: 'TLS', 0x6474E550: 'GNU_EH_FRAME', 0x6474E552: 'GNU_RELRO',
      0x61000001: 'SCE_DYNLIBDATA', 0x61000000: 'SCE_RELRO', 0x6FFFFF00: 'SCE_COMMENT', 0x6FFFFF01: 'SCE_VERSION'}
FLAGS = {1: 'X', 4: 'R', 5: 'RX', 6: 'RW', 0: '-'}
# Strings that show whether a feature is in the image.
FEATURES = {
    'online play (XLiveAPI, libcurl, wolfSSL, UPnP)': ['XLiveAPI::', 'wolfSSL', 'libminiupnpc'],
    'OpenGL game renderer (ps5-opengl)': ['OpenGL vendor=%s renderer=%s', 'ps5-opengl'],
    'frame generation': ['/app0/framegen.conf', 'framegen: binding '],
    'automatic small-resolve readback': ['readback-auto.off'],
    'phone camera (Kinect over HTTPS)': ['/app0/phone-camera.conf'],
    'title updates / DLC manager': ['title update', 'DLC'],
    'per-game accurate render targets (FSI)': ['render-target-fsi'],
    'disc drive helper': ['disc-helper.elf'],
    'physical USB Kinect and its relay': ['Kinect USB: camera {} opened directly',
                                          'Kinect USB: receiving camera frames from the relay on 127.0.0.1:{}',
                                          '/data/kinect-tracker/calibration.bin'],
    'game information archive (x360db)': ['x360db'],
}


def segments(data):
    assert data[:4] == b'\x7fELF', 'not an ELF'
    phoff = struct.unpack_from('<Q', data, 0x20)[0]
    phentsize, phnum = struct.unpack_from('<HH', data, 0x36)
    out = []
    for i in range(phnum):
        p_type, p_flags, off, vaddr, _, filesz, memsz, _ = struct.unpack_from('<IIQQQQQQ', data, phoff + i * phentsize)
        out.append({'type': PT.get(p_type, hex(p_type)), 'flags': FLAGS.get(p_flags, str(p_flags)),
                    'filesz': filesz, 'memsz': memsz})
    return out


def strings(data, minimum=6):
    return set(m.group().decode('ascii') for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minimum, data))


def summary(path):
    data = Path(path).read_bytes()
    segs = segments(data)
    loads = {}
    for s in segs:
        if s['type'] == 'LOAD':
            key = {'X': 'code', 'RX': 'code', 'R': 'rodata', 'RW': 'data', '-': 'unmapped (SCE dynlib data)'}.get(s['flags'], s['flags'])
            loads.setdefault(key, [0, 0])
            loads[key][0] += s['filesz']
            loads[key][1] += s['memsz']
    return data, segs, loads


def main():
    official, rebuilt = sys.argv[1], sys.argv[2]
    od, oseg, oload = summary(official)
    rd, rseg, rload = summary(rebuilt)
    os_, rs = strings(od), strings(rd)
    result = {
        'size': {'official': len(od), 'rebuilt': len(rd)},
        'load_segments': {k: {'official': oload.get(k), 'rebuilt': rload.get(k)} for k in sorted(set(oload) | set(rload))},
        'segments': {'official': oseg, 'rebuilt': rseg},
        'strings': {'official': len(os_), 'rebuilt': len(rs), 'common': len(os_ & rs),
                    'only_official': len(os_ - rs), 'only_rebuilt': len(rs - os_)},
        'features': {name: {'official': any(any(m in s for s in os_) for m in marks),
                            'rebuilt': any(any(m in s for s in rs) for m in marks)}
                     for name, marks in FEATURES.items()},
    }
    # The port's own messages (not library noise) that only one side has: a sample for the report.
    port = re.compile(r'(Xbox360PS5|PS5X360|LAUNCHER|/app0/|/data/homebrew|xbox360ps5)')
    result['port_strings_only_official'] = sorted(s for s in os_ - rs if port.search(s))[:300]
    result['port_strings_only_rebuilt'] = sorted(s for s in rs - os_ if port.search(s))[:300]
    text = json.dumps(result, indent=1)
    if '--json' in sys.argv:
        Path(sys.argv[sys.argv.index('--json') + 1]).write_text(text + '\n')
    def mib(n):
        return f'{n / 1048576:.2f} MiB' if n is not None else '-'
    print(f"file      official {mib(len(od))}  rebuilt {mib(len(rd))}")
    for k, v in result['load_segments'].items():
        o, r = v['official'] or [None, None], v['rebuilt'] or [None, None]
        print(f"{k:9} official {mib(o[0])} (mem {mib(o[1])})  rebuilt {mib(r[0])} (mem {mib(r[1])})")
    print('strings  ', result['strings'])
    for name, v in result['features'].items():
        print(f"  {'yes' if v['official'] else 'no ':3} / {'yes' if v['rebuilt'] else 'no ':3}  {name}")


if __name__ == '__main__':
    main()
