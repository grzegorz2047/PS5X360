"""Reads the ELF out of a PS5 fake-signed executable (FSELF) and signs a patched
ELF of the same layout again, reusing the original FSELF's header and metadata.

Only uncompressed blocked segments can be written back; build_package.py checks
an exact round trip on the unmodified executable before it patches anything.
"""
import hashlib
import struct
import zlib

FSELF_MAGICS = (0x1D3D154F, 0x5414F5EE)


def _layout(data: bytes):
    seg_count = struct.unpack_from("<H", data, 0x18)[0]
    table_end = 0x20 + seg_count * 0x20
    phnum = struct.unpack_from("<H", data, table_end + 0x38)[0]
    elf_hdrs_size = 0x40 + phnum * 0x38
    ext_at = (table_end + elf_hdrs_size + 0xF) & ~0xF
    return seg_count, table_end, phnum, elf_hdrs_size, ext_at


def extract_fself(data: bytes) -> bytes:
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic not in FSELF_MAGICS:
        raise ValueError(f"not an FSELF (magic {magic:#x})")
    seg_count, table_end, phnum, elf_hdrs_size, _ = _layout(data)
    headers = data[table_end : table_end + elf_hdrs_size]

    output_size = len(headers)
    for i in range(phnum):
        offset, _, _, filesz = struct.unpack_from("<4Q", headers, 0x40 + i * 0x38 + 0x08)
        output_size = max(output_size, offset + filesz)
    output = bytearray(output_size)

    segments = [struct.unpack_from("<4Q", data, 0x20 + i * 0x20) for i in range(seg_count)]
    for flags, file_offset, file_size, _ in segments:
        if not flags & 0x800:  # Not a blocked (data-carrying) segment.
            continue
        seg_id = (flags >> 20) & 0xFFF
        if seg_id >= phnum:
            continue
        ph_at = 0x40 + seg_id * 0x38
        out_off = struct.unpack_from("<Q", headers, ph_at + 0x08)[0]
        out_len = struct.unpack_from("<Q", headers, ph_at + 0x20)[0]
        stored = data[file_offset : file_offset + file_size]
        if flags & 0x8:  # Compressed.
            try:
                payload = zlib.decompress(stored)
            except zlib.error:
                payload = zlib.decompress(stored, -zlib.MAX_WBITS)
        else:
            payload = stored
        size = min(len(payload), out_len)
        output[out_off : out_off + size] = payload[:size]

    # The version record (PT 0x6fffff01) is stored after the last segment.
    for i in range(phnum):
        ph_at = 0x40 + i * 0x38
        if struct.unpack_from("<I", headers, ph_at)[0] == 0x6FFFFF01:
            ver_off = struct.unpack_from("<Q", headers, ph_at + 0x08)[0]
            ver_sz = struct.unpack_from("<Q", headers, ph_at + 0x20)[0]
            if ver_sz:
                tail = max(fo + fs for _, fo, fs, _ in segments)
                output[ver_off : ver_off + ver_sz] = data[tail : tail + ver_sz]
            break

    output[:elf_hdrs_size] = headers
    return bytes(output)


def sign_from_template(elf: bytes, template: bytes) -> bytes:
    """Writes elf into a copy of template (same layout) and updates its digest."""
    out = bytearray(template)
    seg_count, table_end, _, elf_hdrs_size, ext_at = _layout(out)
    out[table_end : table_end + elf_hdrs_size] = elf[:elf_hdrs_size]
    for i in range(seg_count):
        flags, file_offset, file_size, _ = struct.unpack_from("<4Q", out, 0x20 + i * 0x20)
        if not flags & 0x800:
            continue
        if flags & 0x8:
            raise ValueError("compressed FSELF segments cannot be re-signed by this tool")
        seg_id = (flags >> 20) & 0xFFF
        ph_at = 0x40 + seg_id * 0x38
        elf_off = struct.unpack_from("<Q", elf, ph_at + 0x08)[0]
        elf_filesz = struct.unpack_from("<Q", elf, ph_at + 0x20)[0]
        if file_size != elf_filesz:
            raise ValueError(f"segment {seg_id} size differs from the template")
        out[file_offset : file_offset + file_size] = elf[elf_off : elf_off + elf_filesz]
    if extract_fself(bytes(out)) != elf:
        raise ValueError("re-signed FSELF does not reproduce the input ELF")
    out[ext_at + 0x20 : ext_at + 0x40] = hashlib.sha256(elf).digest()
    return bytes(out)
