"""Helpers for the binary patch tests: the base ELF, a record of every patch
patches.apply() makes, and the layout facts of the v1.0.2 executable (segments,
functions from .eh_frame_hdr).

patches.apply() is used unchanged. Its patch_vaddr/patch_cstring helpers are
nested functions, so the record is taken with sys.setprofile() while it runs.
"""
import bisect
import contextlib
import hashlib
import io
import struct
import sys
import zipfile
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

import fself  # noqa: E402
import patches  # noqa: E402

# The official v1.0.2 PPSA50011.zip (same value as build_package.BASE_ZIP_SHA256).
BASE_ZIP_SHA256 = "230359c2638f99add3972147e1690e62d8984a37c00181501680bdda7c1096c8"
EBOOT_IN_ZIP = "PPSA50011/eboot.bin"
FILE_DELTA = 0x4000  # file offset = vaddr + 0x4000 (patches.py)

PT_LOAD, PT_GNU_EH_FRAME = 1, 0x6474E550
PF_X = 1


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def load_base(zip_path=None, elf_path=None):
    """Returns (elf, original eboot.bin or None)."""
    if zip_path:
        data = Path(zip_path).read_bytes()
        if sha256(data) != BASE_ZIP_SHA256:
            raise ValueError(f"{zip_path} is not the official v1.0.2 PPSA50011.zip")
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            eboot = z.read(EBOOT_IN_ZIP)
        elf = fself.extract_fself(eboot)
    else:
        eboot, elf = None, Path(elf_path).read_bytes()
    if sha256(elf) != patches.BASE_ELF_SHA256:
        raise ValueError("base ELF is not the one patches.py was written for")
    return elf, eboot


@dataclass(frozen=True)
class Patch:
    index: int
    kind: str  # "vaddr" (patch_vaddr) or "cstring" (patch_cstring)
    vaddr: int
    expected: bytes  # bytes checked before the write (cstring: with its NUL)
    replacement: bytes  # bytes written (cstring: padded with NULs to len(expected))
    desc: str

    @property
    def end(self) -> int:
        return self.vaddr + len(self.expected)

    def __str__(self):
        return f"#{self.index} {self.vaddr:#x}..{self.end:#x} ({self.desc})"


def record_patches(elf: bytes):
    """Runs patches.apply(elf); returns (patched ELF, [Patch...] in application order)."""
    records = []

    def profiler(frame, event, arg):
        if event != "call":
            return
        name = frame.f_code.co_name
        if name == "patch_vaddr":
            loc = frame.f_locals
            records.append(Patch(len(records), "vaddr", loc["vaddr"], bytes.fromhex(loc["expected_hex"]),
                                 bytes.fromhex(loc["replacement_hex"]), loc["desc"]))
        elif name == "patch_cstring":
            loc = frame.f_locals
            exp = loc["expected"] + b"\x00"
            records.append(Patch(len(records), "cstring", loc["vaddr"], exp,
                                 loc["replacement"].ljust(len(exp), b"\x00"), loc["desc"]))

    old = sys.getprofile()
    sys.setprofile(profiler)
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            out = patches.apply(elf)
    finally:
        sys.setprofile(old)
    return out, records


class Image:
    """Virtual-address view of the executable's ELF."""

    def __init__(self, elf: bytes):
        self.elf = elf
        phoff = struct.unpack_from("<Q", elf, 0x20)[0]
        phnum = struct.unpack_from("<H", elf, 0x38)[0]
        self.phdrs = []
        for i in range(phnum):
            p_type, p_flags, off, vaddr, _, filesz, memsz, _ = struct.unpack_from("<IIQQQQQQ", elf, phoff + i * 56)
            self.phdrs.append((p_type, p_flags, off, vaddr, filesz, memsz))
        self.loads = [p for p in self.phdrs if p[0] == PT_LOAD]
        text = [p for p in self.loads if p[1] & PF_X]
        assert len(text) == 1, "expected one executable PT_LOAD"
        _, _, self.text_off, self.text_start, self.text_size, _ = text[0]
        self.text_end = self.text_start + self.text_size
        self._functions = None

    def load_of(self, vaddr: int):
        for p in self.loads:
            if p[3] <= vaddr < p[3] + p[4]:
                return p
        return None

    def offset(self, vaddr: int) -> int:
        p = self.load_of(vaddr)
        assert p is not None, f"{vaddr:#x} is not in a PT_LOAD file image"
        return p[2] + vaddr - p[3]

    def read(self, vaddr: int, size: int) -> bytes:
        off = self.offset(vaddr)
        return self.elf[off: off + size]

    def is_code(self, vaddr: int) -> bool:
        return self.text_start <= vaddr < self.text_end

    @property
    def functions(self):
        """Sorted [(start, end)] of every FDE listed in .eh_frame_hdr."""
        if self._functions is None:
            hdr = next(p for p in self.phdrs if p[0] == PT_GNU_EH_FRAME)[3]
            version, ptr_enc, count_enc, table_enc = self.read(hdr, 4)
            # pcrel|sdata4 eh_frame_ptr, udata4 count, datarel|sdata4 table.
            assert (version, ptr_enc, count_enc, table_enc) == (1, 0x1B, 0x03, 0x3B)
            count = struct.unpack("<I", self.read(hdr + 8, 4))[0]
            table = self.read(hdr + 12, count * 8)
            funcs = []
            for i in range(count):
                loc, fde = struct.unpack_from("<ii", table, i * 8)
                fde += hdr
                # FDE: length, CIE pointer, pc_begin (pcrel sdata4), pc_range (sdata4).
                begin = struct.unpack("<i", self.read(fde + 8, 4))[0] + fde + 8
                size = struct.unpack("<I", self.read(fde + 12, 4))[0]
                assert begin == hdr + loc, f"FDE {fde:#x} does not match its .eh_frame_hdr entry"
                funcs.append((begin, begin + size))
            funcs.sort()
            self._functions = funcs
            self._starts = [f[0] for f in funcs]
        return self._functions

    def function_at(self, vaddr: int):
        funcs = self.functions
        i = bisect.bisect_right(self._starts, vaddr) - 1
        if i >= 0 and funcs[i][0] <= vaddr < funcs[i][1]:
            return funcs[i]
        return None

    def is_function_start(self, vaddr: int) -> bool:
        self.functions
        i = bisect.bisect_left(self._starts, vaddr)
        return i < len(self._starts) and self._starts[i] == vaddr

    def next_function_start(self, vaddr: int):
        self.functions
        i = bisect.bisect_right(self._starts, vaddr)
        return self._starts[i] if i < len(self._starts) else self.text_end

    # --- Exception tables -------------------------------------------------------

    def _uleb(self, a):
        value = shift = 0
        while True:
            b = self.read(a, 1)[0]
            a += 1
            value |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return value, a

    def _pointer(self, a, enc):
        fmt, app = enc & 0x0F, enc & 0x70
        if fmt == 0x0B:
            value, end = struct.unpack("<i", self.read(a, 4))[0], a + 4
        elif fmt == 0x03:
            value, end = struct.unpack("<I", self.read(a, 4))[0], a + 4
        elif fmt == 0x01:
            value, end = self._uleb(a)
        else:
            raise ValueError(f"pointer encoding {enc:#x}")
        if app == 0x10:
            value += a
        elif app:
            raise ValueError(f"pointer encoding {enc:#x}")
        return value, end

    def lsda_of(self, func_start: int) -> int:
        """Address of the function's LSDA (from its FDE's 'zPLR' augmentation)."""
        hdr = next(p for p in self.phdrs if p[0] == PT_GNU_EH_FRAME)[3]
        count = struct.unpack("<I", self.read(hdr + 8, 4))[0]
        table = self.read(hdr + 12, count * 8)
        fde = next(hdr + f for loc, f in struct.iter_unpack("<ii", table) if hdr + loc == func_start)
        cie = fde + 4 - struct.unpack("<I", self.read(fde + 4, 4))[0]
        a = cie + 9
        aug = bytearray()
        while self.read(a, 1) != b"\x00":
            aug += self.read(a, 1)
            a += 1
        assert aug == b"zPLR", aug
        _, a = self._uleb(a + 1)  # code alignment
        _, a = self._uleb(a)  # data alignment (small negative sleb)
        _, a = self._uleb(a + 1)  # return register byte, augmentation length
        personality_enc = self.read(a, 1)[0]
        _, a = self._pointer(a + 1, personality_enc)
        lsda_enc = self.read(a, 1)[0]
        _, a = self._uleb(fde + 16)  # FDE augmentation length
        return self._pointer(a, lsda_enc)[0]

    def call_sites(self, func_start: int):
        """[(start, end, landing_pad or 0, action)] of the function's LSDA."""
        a = self.lsda_of(func_start)
        assert self.read(a, 1) == b"\xff", "landing pad base other than the function start"
        ttype_enc = self.read(a + 1, 1)[0]
        a += 2
        if ttype_enc != 0xFF:
            _, a = self._uleb(a)
        cs_enc = self.read(a, 1)[0]
        size, a = self._uleb(a + 1)
        end, out = a + size, []
        while a < end:
            start, a = self._pointer(a, cs_enc)
            length, a = self._pointer(a, cs_enc)
            pad, a = self._pointer(a, cs_enc)
            action, a = self._uleb(a)
            out.append((func_start + start, func_start + start + length, func_start + pad if pad else 0, action))
        return out
