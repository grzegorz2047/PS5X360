"""Static checks of the code patches with capstone: whole instructions, code caves
only in inter-function int3 padding, and every relative branch landing on an
instruction boundary of the place it is meant to reach."""
from collections import defaultdict

import pytest

import x86

# Every direct branch in the patched code, by the patch it sits in, in order:
# (mnemonic, target, what the comments in patches.py say it reaches).
# Targets inside the patched code are named after the cave or hook.
STEADY_CLOCK_NOW = 0x19A3690
NANO_SLEEP = 0x57BF50
FSI_WANTED_FOR = 0x1E5F70
GET_NORMALIZED_DEPTH_CONTROL = 0xFC790
READBACK_MODE = 0x9CA10  # called by IssueDrawInner at 0x5bafdf in v1.0.2 (`cmp eax, 2` = full)
OPTION_VECTOR = 0x1BE620  # vector(ptr, count) helper the Options() entries call
OPTIONS_TAIL = 0x1BE780  # Options(): final call of the rebuilt block, as in v1.0.2

INTENDED_BRANCHES = {
    0x121CE1: [("jmp", 0x121D56, "past the 256-byte XMM copy (was jne to the same place)")],
    0x802EA: [("call", STEADY_CLOCK_NOW, "steady_clock::now"), ("call", 0x10111, "cave: add 30000 ms")],
    0x1507108: [
        ("call", READBACK_MODE, "readback mode"),
        ("je", 0x5BAFEA, "IssueDrawInner wait path (readback full)"),
        ("je", 0x5BB01A, "IssueDrawInner no-wait path (no colour)"),
        ("jne", 0x5BB01A, "IssueDrawInner no-wait path (blended RT0)"),
        ("jmp", 0xA98E57, "code cave 2"),
    ],
    0xA98E57: [
        ("ja", 0xA98E6F, "cave 2: pitch check"),
        ("je", 0x5BAFEA, "IssueDrawInner wait path (<= 6 indices)"),
        ("jmp", 0x5BAFE8, "the kept jne at 0x5bafe8"),
    ],
    0x5BAFDF: [("jmp", 0x1507108, "code cave 1"), ("jne", 0x5BB01A, "IssueDrawInner no-wait path")],
    0x150EDC8: [
        ("jb", 0x150EDFB, "cave 3: no boost"),
        ("je", 0x150EDF8, "cave 3: boost (pitch <= 256)"),
        ("ja", 0x150EDFB, "cave 3: no boost (> 6 indices)"),
        ("call", GET_NORMALIZED_DEPTH_CONTROL, "GetNormalizedDepthControl"),
        ("jne", 0x150EDFB, "cave 3: no boost (experimental, depth on)"),
        ("jmp", 0x1500131, "code cave 3b"),
    ],
    0x1500131: [("jmp", 0x5F0B9F, "ConfigurePipeline after the replayed mov")],
    0x5F0B98: [("jmp", 0x150EDC8, "code cave 3")],
    0x7713DE: [("jmp", 0x1509F94, "code cave 4")],
    0x1509F94: [
        ("je", 0x771402, "moved VdSwap epilogue (VSync off)"),
        ("jne", 0x771402, "moved VdSwap epilogue (patch_vsync_off)"),
        ("call", STEADY_CLOCK_NOW, "steady_clock::now"),
        ("jmp", 0x150C919, "code cave 5"),
    ],
    0x150C919: [("jb", 0x150C932, "cave 5: keep the grid"), ("jmp", 0x5B8421, "code cave 6")],
    0x5B8421: [("call", NANO_SLEEP, "xe::threading::NanoSleep"), ("jmp", 0x771402, "moved VdSwap epilogue")],
    0x1BD933: [("call", OPTION_VECTOR, "vector(ptr, 3)")],
    0x5FA5D8: [("jmp", 0x5FA669, "past the old debug log block")],
    0x5FA5DD: [
        ("je", 0x5FA64B, "cave 7: leave the pipeline as it is"),
        ("je", 0x5FA64B, "cave 7: leave the pipeline as it is"),
        ("je", 0x5FA64B, "cave 7: leave the pipeline as it is"),
        ("jmp", 0x5FB0B8, "EnsurePipelineCreated after the subpass store"),
    ],
    0x5FB0AE: [("jmp", 0x5FA5DD, "code cave 7")],
    0x1BE2E1: [("call", OPTION_VECTOR, "vector(ptr, n)")] * 5 + [("call", OPTIONS_TAIL, "as in v1.0.2")],
    0x1502AE1: [("jb", FSI_WANTED_FOR, "FsiWantedFor (tail call, value 1 = Padrão)")],
    0x689FB: [("call", 0x1502AE1, "code cave 8")],
}


def code_patches(image, records):
    return [p for p in records if image.is_code(p.vaddr)]


@pytest.fixture(scope="module")
def sweeps(base_image, patched_image, records):
    """Per function holding a code patch: (base, patched) as ({addr: insn}, [bad]).

    The base is a linear sweep of v1.0.2. The patched decoding is composed: the
    base instructions outside every patch, the patches' own instructions, and an
    operand-only patch's instruction decoded again. A base instruction that a
    patch overwrites only in part (its first bytes) is left out: see
    test_partly_overwritten_instructions_are_unreachable.
    """
    patches = code_patches(base_image, records)
    out = {}
    for p in patches:
        func = base_image.function_at(p.vaddr)
        if not func or func in out:
            continue
        base_insns, bad = x86.sweep(base_image, *func)
        mine = [q for q in patches if q.vaddr < func[1] and q.end > func[0]]
        composed = {}
        for addr, ins in base_insns.items():
            hit = [q for q in mine if addr < q.end and addr + ins.size > q.vaddr]
            if not hit:
                composed[addr] = ins
            elif all(addr < q.vaddr and addr + ins.size >= q.end for q in hit):
                composed[addr] = x86.decode(patched_image.read(addr, ins.size), addr)[0]
        for q in mine:
            if q.vaddr in base_insns:
                composed.update((i.address, i) for i in x86.decode(q.replacement, q.vaddr))
        out[func] = ((base_insns, bad), (composed, []))
    return out


def decoded(p):
    return x86.decode(p.replacement, p.vaddr)


def operand_only(p, sweeps, base_image):
    """A patch that rewrites part of one instruction (an immediate)."""
    func = base_image.function_at(p.vaddr)
    if not func:
        return False
    (base_insns, _), _ = sweeps[func]
    return p.vaddr not in base_insns


def partly_overwritten(p, sweeps, base_image):
    """The base instruction a patch ends inside, if any: (start, insn)."""
    func = base_image.function_at(p.vaddr)
    if not func or operand_only(p, sweeps, base_image):
        return None
    (base_insns, _), _ = sweeps[func]
    for addr, ins in base_insns.items():
        if addr < p.end < addr + ins.size:
            return addr, ins
    return None


def test_every_code_patch_is_listed(base_image, records):
    with_branches = {p.vaddr for p in code_patches(base_image, records)
                     if any(x86.rel_target(i) is not None for i in decoded(p))}
    assert with_branches == set(INTENDED_BRANCHES)


def test_functions_sweep_cleanly(sweeps):
    for func, ((_, base_bad), _) in sweeps.items():
        assert not base_bad, f"function {func[0]:#x}: undecodable bytes at {[hex(a) for a in base_bad[:5]]}"


def test_patches_are_whole_instructions(base_image, records, sweeps):
    """A code patch decodes to whole instructions filling its range and starts on an
    instruction boundary of the original function. Operand-only patches keep the
    instruction (same start, length and mnemonic)."""
    for p in code_patches(base_image, records):
        func = base_image.function_at(p.vaddr)
        if func:
            (base_insns, _), (patched_insns, _) = sweeps[func]
            if operand_only(p, sweeps, base_image):
                start = max(a for a in base_insns if a <= p.vaddr)
                b, n = base_insns[start], patched_insns.get(start)
                assert n is not None and start + b.size >= p.end, p
                assert (n.size, n.mnemonic) == (b.size, b.mnemonic), p
                continue
            assert p.vaddr in base_insns, f"{p} does not start on an instruction"
        insns = decoded(p)
        assert sum(i.size for i in insns) == len(p.replacement), f"{p} does not decode completely"


def test_partly_overwritten_instructions_are_unreachable(base_image, records, sweeps):
    """A patch may end inside an original instruction only when it ends on an
    unconditional jmp/ret and nothing branches to the bytes after it up to the next
    instruction that is still reached (code cave 7 reuses most of a log block that
    the jmp at 0x5fa5d8 skips; its last original instruction is cut)."""
    for p in code_patches(base_image, records):
        cut = partly_overwritten(p, sweeps, base_image)
        if not cut:
            continue
        last = decoded(p)[-1]
        assert x86.is_uncond_jmp(last) or x86.is_ret(last), f"{p} falls through into a cut instruction"
        func = base_image.function_at(p.vaddr)
        _, (patched_insns, _) = sweeps[func]
        dead_end = cut[0] + cut[1].size
        for addr, ins in patched_insns.items():
            t = x86.rel_target(ins)
            assert t is None or not p.end <= t < dead_end, f"{addr:#x} branches into the cut instruction after {p}"


def test_caves_are_in_int3_padding(base_image, records):
    for p in code_patches(base_image, records):
        if base_image.function_at(p.vaddr):
            continue
        assert p.expected == b"\xcc" * len(p.expected), f"{p} is outside every function but not int3 padding"
        assert base_image.function_at(p.end - 1) is None, p
        assert p.end <= base_image.next_function_start(p.vaddr), f"{p} runs into the next function"


def test_branches_match_intent(base_image, records):
    problems = []
    for p in code_patches(base_image, records):
        got = [(i.mnemonic, x86.rel_target(i)) for i in decoded(p) if x86.rel_target(i) is not None]
        want = [(m, t) for m, t, _ in INTENDED_BRANCHES.get(p.vaddr, [])]
        if got != want:
            problems.append(f"{p}: {[(m, hex(t)) for m, t in got]} != {[(m, hex(t)) for m, t in want]}")
    assert not problems, "\n".join(problems)


def test_branch_targets_are_instruction_boundaries(base_image, patched_image, records, sweeps):
    """Every direct branch out of patched code lands on an instruction boundary:
    of the patch it targets, of the patched function it returns into (one a hook
    in the chain sits in), or on a function entry (calls and tail calls)."""
    patches = code_patches(base_image, records)
    starts = {}
    for p in patches:
        func = base_image.function_at(p.vaddr)
        if func:
            starts[p.vaddr] = set(sweeps[func][1][0])
        else:
            starts[p.vaddr] = {i.address for i in decoded(p)}

    def patch_at(addr):
        return next((p for p in patches if p.vaddr <= addr < p.end), None)

    # Functions each patch is reached from (hooks sit in one; caves inherit).
    homes = defaultdict(set)
    for p in patches:
        func = base_image.function_at(p.vaddr)
        if func:
            homes[p.vaddr].add(func)
    edges = [(p, patch_at(x86.rel_target(i))) for p in patches for i in decoded(p)
             if x86.rel_target(i) is not None and patch_at(x86.rel_target(i))]
    changed = True
    while changed:
        changed = False
        for src, dst in edges:
            if not homes[src.vaddr] <= homes[dst.vaddr]:
                homes[dst.vaddr] |= homes[src.vaddr]
                changed = True

    problems = []
    for p in patches:
        for ins in decoded(p):
            t = x86.rel_target(ins)
            if t is None:
                continue
            where = f"{ins.address:#x} {ins.mnemonic} {t:#x} ({p.desc})"
            dst = patch_at(t)
            if dst is not None:
                if t not in starts[dst.vaddr]:
                    problems.append(f"{where}: not an instruction of the target patch")
            elif patched_image.is_function_start(t):
                pass  # call, or tail call
            else:
                func = patched_image.function_at(t)
                if func is None or func not in homes[p.vaddr]:
                    problems.append(f"{where}: lands outside the hooked function")
                elif t not in sweeps[func][1][0]:
                    problems.append(f"{where}: not an instruction boundary")
            if x86.is_call(ins) and dst is None and not patched_image.is_function_start(t):
                problems.append(f"{where}: call to a non-function")
    assert not problems, "\n".join(problems)


def test_no_original_branch_into_replaced_code(base_image, records, sweeps):
    """No unpatched instruction of a patched function branches into the middle of a
    patch (where the original instructions it expected are gone)."""
    patches = code_patches(base_image, records)
    problems = []
    for func, (_, (patched_insns, _)) in sweeps.items():
        for addr, ins in patched_insns.items():
            if any(p.vaddr <= addr < p.end for p in patches):
                continue
            t = x86.rel_target(ins)
            if t is None:
                continue
            for p in patches:
                if p.vaddr < t < p.end:
                    problems.append(f"{addr:#x} {ins.mnemonic} {t:#x} lands inside {p}")
    assert not problems, "\n".join(problems)


def test_called_functions_are_the_ones_v102_calls(base_image, records, sweeps):
    """Calls the patches add go to functions v1.0.2 already calls the same way."""
    for p in code_patches(base_image, records):
        for ins in decoded(p):
            t = x86.rel_target(ins)
            if not x86.is_call(ins) or not base_image.is_function_start(t):
                continue
            callers = [a for (_, ((insns, _), _)) in sweeps.items() for a, i in insns.items()
                       if x86.is_call(i) and x86.rel_target(i) == t]
            assert callers or t in (GET_NORMALIZED_DEPTH_CONTROL, NANO_SLEEP, STEADY_CLOCK_NOW), \
                f"{ins.address:#x}: call {t:#x} is not called elsewhere in the patched functions"


# RIP-relative data the patched code reads or writes.
EXPERIMENTAL_FLAG = 0x3926582  # padding byte after the async_shader_compilation bool (patch 12)
INTENDED_DATA = {
    0x3926582: "experimental async-shader flag",
    0x3926581: "cvars::async_shader_compilation",
    0x3926570: "cvars::vsync",
    0x3955818: "patch_vsync_off",
    0x395CF10: "gpu_diag protect_nanoseconds (60 FPS cap deadline)",
    0x3955A60: "Settings::Apply cvar (+0x84), as in v1.0.2",
    0x39559E8: "Settings::Apply cvar (+0x8c), as in v1.0.2",
    0x1D079D8: "double 2.0 (banner delay)",
}
STRING_REFS = {
    0x65D74: b"/data/homebrew/PPSA50011/cache",
    0x1D9851: b"/download0/xbox360ps5/framegen.conf",
    0x613319: b"/download0/xbox360ps5/framegen.conf",
    0x1D9FA3: b"/download0/xbox360ps5/framegen.conf",
    0x1D9F12: b"/download0/xbox360ps5/framegen.conf.new",
    0x1D9F9C: b"/download0/xbox360ps5/framegen.conf.new",
    0x1D9FB4: b"/download0/xbox360ps5/framegen.conf.new",
}


# Options() blocks rebuilt by patches 12 and 13: the data they add to what the
# v1.0.2 bytes referenced.
OPTIONS_NEW_DATA = {
    0x1BD933: {0x1DA8082: "Ligado (experimental)".encode()},
    0x1BE2E1: {
        0x1DDC53C: b"fsi",
        0x1DA8098: b"FSI por jogo",
        0x1D78C03: "Padrão: preset do jogo ou FSI geral. As outras forçam.".encode(),
        0x1D52B9E: "Padrão".encode(),
    },
}


def test_rip_relative_references(base_image, patched_image, records):
    import struct

    problems = []
    for p in code_patches(base_image, records):
        refs = {(i.address, i.mnemonic, a) for i in decoded(p) for a in x86.rip_refs(i)}
        if p.vaddr in OPTIONS_NEW_DATA:
            before = {a for i in x86.decode(p.expected, p.vaddr) for a in x86.rip_refs(i)}
            added = {a for _, _, a in refs} - before
            want = OPTIONS_NEW_DATA[p.vaddr]
            if added != set(want):
                problems.append(f"{p}: adds references {sorted(map(hex, added))}, expected {sorted(map(hex, want))}")
            for addr, text in want.items():
                if patched_image.read(addr, len(text) + 1) != text + b"\x00":
                    problems.append(f"{p}: {addr:#x} is not {text!r}")
            continue
        for at, mnemonic, addr in refs:
            if p.vaddr in STRING_REFS:
                want = STRING_REFS[p.vaddr] + b"\x00"
                if mnemonic != "lea" or patched_image.read(addr, len(want)) != want:
                    problems.append(f"{at:#x}: {mnemonic} {addr:#x} is not {want!r}")
            elif addr not in INTENDED_DATA:
                problems.append(f"{at:#x} {mnemonic}: unexpected data {addr:#x}")
    assert not problems, "\n".join(problems)
    assert struct.unpack("<d", patched_image.read(0x1D079D8, 8))[0] == 2.0
    for addr in INTENDED_DATA:
        seg = patched_image.load_of(addr) or next(
            (q for q in patched_image.loads if q[3] <= addr < q[3] + q[5]), None)
        assert seg is not None, f"{addr:#x} is not mapped"
