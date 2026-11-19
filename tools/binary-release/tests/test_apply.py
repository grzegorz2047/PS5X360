"""patches.apply() on the official v1.0.2 ELF: every check passes, the ranges do
not collide, and nothing outside the declared ranges changes."""
import pytest

import elfpatch
import fself
import patches


def test_records_cover_apply(base_elf, applied):
    """Replaying the recorded patches on the base gives exactly apply()'s output,
    and each expected value matches the bytes present when it is applied."""
    patched, records = applied
    assert len(records) >= 50
    replay = bytearray(base_elf)
    for p in records:
        off = p.vaddr + elfpatch.FILE_DELTA
        assert len(p.expected) == len(p.replacement), p
        assert bytes(replay[off: off + len(p.expected)]) == p.expected, p
        replay[off: off + len(p.replacement)] = p.replacement
    assert bytes(replay) == patched
    assert len(patched) == len(base_elf)


def test_apply_is_deterministic(base_elf, patched_elf):
    assert elfpatch.record_patches(base_elf)[0] == patched_elf


def test_apply_refuses_a_different_executable(patched_elf):
    """The expected-bytes checks fire: applying twice fails on the first patch."""
    with pytest.raises(AssertionError, match="mismatch"):
        elfpatch.record_patches(patched_elf)


def test_every_patch_changes_something(records):
    unchanged = [str(p) for p in records if p.expected == p.replacement]
    assert not unchanged


def test_no_overlapping_patches(records):
    """No two patches write the same bytes. The one exception is a string slot whose
    tail an earlier patch_cstring zeroed: a later patch may then fill those NULs
    (patches 11, 12 and 13 reuse the tail of the RenderDoc help text of patch 6)."""
    problems = []
    for i, a in enumerate(records):
        for b in records[i + 1:]:
            lo, hi = max(a.vaddr, b.vaddr), min(a.end, b.end)
            if lo >= hi:
                continue
            overlap = a.replacement[lo - a.vaddr: hi - a.vaddr]
            text_end = a.vaddr + a.replacement.index(b"\x00") + 1 if a.kind == "cstring" else None
            reuses_zeroed_tail = (
                a.kind == "cstring"
                and overlap == b"\x00" * len(overlap)
                and lo >= text_end  # after the new string's own terminator
                and b.expected[lo - b.vaddr: hi - b.vaddr] == overlap
            )
            if not reuses_zeroed_tail:
                problems.append(f"{a} overlaps {b} at {lo:#x}..{hi:#x}")
    assert not problems, "\n".join(problems)


def test_diff_is_only_declared_ranges(base_elf, patched_elf, records):
    """Putting the base bytes back into every declared range gives the base again,
    so nothing outside the declared ranges changed."""
    restored = bytearray(patched_elf)
    for p in records:
        off = p.vaddr + elfpatch.FILE_DELTA
        restored[off: off + len(p.expected)] = base_elf[off: off + len(p.expected)]
    assert bytes(restored) == base_elf


def test_ranges_map_to_file_image(base_image, records):
    """patches.py writes at file offset vaddr + 0x4000; that must be where the ELF
    program headers load each range from, and a range must not cross segments."""
    for p in records:
        seg = base_image.load_of(p.vaddr)
        assert seg is not None, p
        assert base_image.offset(p.vaddr) == p.vaddr + elfpatch.FILE_DELTA, p
        assert p.end <= seg[3] + seg[4], f"{p} runs past its segment"


def test_strings_are_data_and_code_is_code(base_image, records):
    for p in records:
        if p.kind == "cstring":
            assert not base_image.is_code(p.vaddr), p
            text = p.replacement.rstrip(b"\x00")
            assert b"\x00" not in text, p
            text.decode("utf-8")


def test_base_hash_constant_matches(base_elf):
    assert elfpatch.sha256(base_elf) == patches.BASE_ELF_SHA256


def test_fself_round_trip(base, patched_elf):
    eboot = base["eboot"]
    if eboot is None:
        pytest.skip("needs the official zip (PS5X360_BASE_ZIP) for the original eboot.bin")
    assert fself.sign_from_template(base["elf"], eboot) == eboot
    signed = fself.sign_from_template(patched_elf, eboot)
    assert len(signed) == len(eboot)
    assert fself.extract_fself(signed) == patched_elf
