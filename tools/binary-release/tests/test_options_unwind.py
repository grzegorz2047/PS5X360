"""Patch 13 rebuilds Options() entries 41-44 and adds entry 45 ("fsi"); its
exception table (LSDA call sites) and the cleanup loops move with it."""
import x86

OPTIONS = 0x1BCE00
BLOCK = (0x1BE2E1, 0x1BE4EE)  # the rebuilt code
ENTRY_SIZE = 0x48
OPTIONS_VECTOR_CALL = 0x1BE4E9  # vector(ptr, count) of every entry, after `mov edx, count`
DESTROY_LOOPS = (0x1BE4F5, 0x1BE557)  # `mov ebx/r15d, offset of the last entry`
GENERIC_CLEANUP = 0x1BE5CE  # destroys the entries below the current one (r15)


def function_insns(image):
    start, end = image.function_at(OPTIONS)
    insns, bad = x86.sweep(image, start, end)
    return insns, bad


def test_call_sites_are_well_formed(patched_image):
    insns, _ = function_insns(patched_image)
    start, end = patched_image.function_at(OPTIONS)
    sites = patched_image.call_sites(OPTIONS)
    previous_end = start
    for lo, hi, pad, _ in sites:
        assert previous_end <= lo < hi <= end, f"call site {lo:#x}..{hi:#x} out of order"
        assert lo in insns and (hi in insns or hi == end), f"call site {lo:#x}..{hi:#x} not on instructions"
        assert pad == 0 or pad in insns, f"landing pad {pad:#x} is not an instruction"
        previous_end = hi


def covering(sites, addr):
    found = [s for s in sites if s[0] <= addr < s[1]]
    assert len(found) <= 1
    return found[0] if found else None


def test_call_sites_outside_the_block_are_unchanged(base_image, patched_image):
    def outside(sites):
        return [s for s in sites if s[1] <= BLOCK[0] or s[0] >= BLOCK[1]]

    assert outside(patched_image.call_sites(OPTIONS)) == outside(base_image.call_sites(OPTIONS))


def test_every_call_in_the_block_unwinds_into_the_generic_cleanup(base_image, patched_image):
    """In v1.0.2 every call in the block has a landing pad. The rebuilt block keeps
    the entry being built in r15, so its calls must unwind through a landing pad
    that ends in the r15-based cleanup (not 0x1be587, which assumes entry 44)."""
    for image, generic_only in ((base_image, False), (patched_image, True)):
        insns, _ = function_insns(image)
        sites = image.call_sites(OPTIONS)
        calls = [a for a, i in insns.items() if BLOCK[0] <= a < BLOCK[1] and x86.is_call(i)]
        assert len(calls) >= 5
        for a in calls:
            site = covering(sites, a)
            assert site and site[2], f"call at {a:#x} has no landing pad"
            if a == OPTIONS_VECTOR_CALL:
                # The static vector of all entries: its pad runs DESTROY_LOOPS[1].
                assert site[2] == 0x1BE54D and DESTROY_LOOPS[1] in range(site[2], site[2] + 16)
            elif generic_only:
                pad = insns[site[2]]
                assert x86.is_uncond_jmp(pad) and x86.rel_target(pad) == GENERIC_CLEANUP, \
                    f"call at {a:#x} unwinds to {site[2]:#x}"


def entry_count(image):
    insns, _ = function_insns(image)
    count = None
    for a in sorted(insns):
        if a >= OPTIONS_VECTOR_CALL:
            break
        i = insns[a]
        if i.mnemonic == "mov" and i.op_str.startswith("edx, "):
            count = int(i.op_str.split(", ")[1], 0)
    lasts = [int(insns[a].op_str.split(", ")[1], 0) for a in DESTROY_LOOPS]
    return count, lasts


def test_entry_count_matches_cleanup_loops(base_image, patched_image):
    for image, want in ((base_image, 45), (patched_image, 46)):
        count, lasts = entry_count(image)
        assert count == want
        # The loops walk the choices vector at +0x30 of every entry, last one first.
        assert lasts == [(count - 1) * ENTRY_SIZE + 0x30] * 2
