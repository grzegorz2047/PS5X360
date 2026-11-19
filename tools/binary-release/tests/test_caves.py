"""Behaviour of the decision caves, run in unicorn from their hook sites on the
base and the patched executable. The expected results are the rules in the
comments of patches.py; the v1.0.2 run shows what is unchanged.

Each scenario also compares the registers the two runs end with: a register the
patch changes that is not the patch's intended output must be dead where the
code continues (written before it is read on every path, see x86.dead_at).
"""
import itertools
from collections import defaultdict

import pytest

import emu as E
import x86

# Executable addresses (comments of patches.py).
EXPERIMENTAL_FLAG = 0x3926582
ASYNC_SHADERS_CVAR = 0x3926581
SETTINGS_CVAR_84 = 0x3955A60
SETTINGS_CVAR_8C = 0x39559E8
READBACK_CVAR = 0x3924528  # std::string readback mode ("none" / "fast" / "full")
DEPTH_CONTROL_CVAR = 0x3955A10  # read by GetNormalizedDepthControl
VSYNC_CVAR = 0x3926570
PATCH_VSYNC_OFF = 0x3955818
SWAP_DEADLINE = 0x395CF10
STEADY_CLOCK_NOW = 0x19A3690
NANO_SLEEP = 0x57BF50
FSI_WANTED_FOR = 0x1E5F70
PERIOD_NS = 16_600_000

# Xenos registers (dword index -> byte offset in the register file).
RB_SURFACE_INFO = 0x2000 * 4  # surface_pitch bits 0..13
VGT_DRAW_INITIATOR = 0x21FC * 4  # num_indices bits 16..31
RB_DEPTHCONTROL = 0x2200 * 4  # z_enable bit 1
RB_BLENDCONTROL0 = 0x2201 * 4  # color_destblend bits 8..12 (0 = Zero)
RB_STENCILREFMASK_BF = 0x2203 * 4
RB_MODECONTROL = 0x2208 * 4  # edram mode bits 0..2 (4 = color+depth)
RB_DEPTH_INFO_BYTE = 0x8A4C

THIS = E.SCRATCH + 0x100
SETTINGS = E.SCRATCH + 0x1000
REGS = E.SCRATCH + 0x10000
FRAME = E.STACK_BASE + 0x10000
TITLE = E.SCRATCH + 0x2000

READBACK = {0: b"none", 1: b"fast", 2: b"full"}
INDICES = (1, 6, 7, 0xFFFF)
PITCHES = (1, 256, 257, 1280)


@pytest.fixture(scope="module")
def emus(base_image, patched_image):
    return {"base": E.Emu(base_image, seed=1), "patched": E.Emu(patched_image, seed=1)}


def set_draw_registers(m, *, indices, pitch, destblend=0, depth=0):
    # Fields around the ones the caves read are set, to show they are masked off.
    m.write_u32(REGS + VGT_DRAW_INITIATOR, (indices << 16) | 0x0084)
    m.write_u32(REGS + RB_SURFACE_INFO, (2 << 16) | pitch)
    m.write_u32(REGS + RB_BLENDCONTROL0, (5 << 24) | (1 << 16) | (destblend << 8) | 1)
    m.write_u32(REGS + RB_DEPTHCONTROL, depth)
    m.write_u32(REGS + RB_MODECONTROL, 4)
    m.write_u32(REGS + RB_STENCILREFMASK_BF, 0)
    m.write_u8(REGS + RB_DEPTH_INFO_BYTE, 0)
    m.write_u8(DEPTH_CONTROL_CVAR, 1)


def apply_settings(m, *, async_shaders, cvar_84=1, cvar_8c=0, seed=0):
    """Runs the patched part of Settings::Apply (0x1c2126..0x1c2159)."""
    m.write_u32(SETTINGS + 0x84, cvar_84)
    m.write_u32(SETTINGS + 0x88, async_shaders)
    m.write_u32(SETTINGS + 0x8C, cvar_8c)
    for addr in (SETTINGS_CVAR_84, ASYNC_SHADERS_CVAR, EXPERIMENTAL_FLAG, SETTINGS_CVAR_8C):
        m.write_u8(addr, 0xEE)
    m.poison(seed)
    m.set_reg("rbx", SETTINGS)
    return m.run(0x1C2126, {0x1C2159: "next"}, rsp=FRAME)


class Differences:
    """Registers that differ between base and patched, by the address the patched
    run stopped at."""

    def __init__(self):
        self.by_stop = defaultdict(set)

    def add(self, stop_addr, base_regs, patched_regs, ignore=()):
        for reg, value in patched_regs.items():
            if reg not in ignore and base_regs[reg] != value:
                self.by_stop[stop_addr].add(reg)

    def assert_dead(self, image, base_image=None, **kw):
        """Dead in the patched code, or (caller-saved) junk at that point in v1.0.2."""
        live = [f"{r} at {a:#x}" for a, regs in self.by_stop.items() for r in sorted(regs)
                if not x86.dead_at(image, a, r, **kw)
                and not (base_image and x86.junk_at(base_image, a, r))]
        assert not live, "registers changed by the patch and still read: " + ", ".join(live)


# --- Patch 12/13: Settings::Apply ---------------------------------------------------


def test_settings_apply_async_shaders(emus, base_image, patched_image):
    """async_shaders 0/1/2: async_shader_compilation = value != 0 as before, and the
    experimental flag = value == 2. The neighbouring cvars are unchanged."""
    diff = Differences()
    for value, c84, c8c in itertools.product((0, 1, 2, 3), (0, 1, 7), (0, 1)):
        runs = {}
        for name, m in emus.items():
            _, regs = apply_settings(m, async_shaders=value, cvar_84=c84, cvar_8c=c8c, seed=value)
            runs[name] = (regs, m.data_writes(regs["rsp"]))
            assert m.read_u8(SETTINGS_CVAR_84) == (c84 != 0)
            assert m.read_u8(ASYNC_SHADERS_CVAR) == (value != 0)
            assert m.read_u8(SETTINGS_CVAR_8C) == (c8c != 0)
        assert emus["patched"].read_u8(EXPERIMENTAL_FLAG) == (value == 2)
        assert emus["base"].read_u8(EXPERIMENTAL_FLAG) == 0xEE  # v1.0.2 never writes it
        base_writes, patched_writes = runs["base"][1], runs["patched"][1]
        assert set(patched_writes) - set(base_writes) == {EXPERIMENTAL_FLAG}
        diff.add(0x1C2159, runs["base"][0], runs["patched"][0])
    diff.assert_dead(patched_image, base_image)


def test_settings_apply_draw_resolution_scale(emus, patched_image):
    """Field 0x38 now holds the per-game FSI value; Apply loads a constant 1 instead."""
    for field in (0, 1, 2, 3):
        m = emus["patched"]
        m.write_u32(SETTINGS + 0x38, field)
        m.poison(field)
        m.set_reg("rbx", SETTINGS)
        _, regs = m.run(0x1C2005, {0x1C2008: "next"}, rsp=FRAME)
        assert regs["rax"] == 1 and regs["rsp"] == FRAME
        b = emus["base"]
        b.write_u32(SETTINGS + 0x38, field)
        b.poison(field)
        b.set_reg("rbx", SETTINGS)
        _, base_regs = b.run(0x1C2005, {0x1C2008: "next"}, rsp=FRAME)
        assert base_regs["rax"] == field
        diff = Differences()
        diff.add(0x1C2008, base_regs, regs, ignore={"rax"})
        assert not diff.by_stop


# --- Patch 8: IssueDrawInner stand-in decision (code caves 1 and 2) -----------------


def expected_wait(*, readback, color_mask, destblend, indices, depth, pitch, experimental):
    if readback == 2:
        return True  # readback "full" waited in v1.0.2 already
    if color_mask == 0 or destblend != 0:
        return False  # no colour output, or RT0 blended
    z_enable = depth >> 1 & 1
    if indices <= 6 and not (experimental and z_enable):
        return True  # full-screen pass
    return pitch <= 256  # small render target


def run_issue_draw(m, *, readback, color_mask, destblend, indices, depth, pitch, seed):
    set_draw_registers(m, indices=indices, pitch=pitch, destblend=destblend)
    m.write(READBACK_CVAR, bytes([len(READBACK[readback]) << 1]) + READBACK[readback] + b"\x00")
    m.write_u64(THIS + 0x38, REGS)
    m.write_u64(FRAME + 0x18, THIS)
    m.write_u32(FRAME + 0xA0, color_mask)
    m.poison(seed)
    m.set_reg("rbx", (m.reg("rbx") & ~0xFF) | depth)  # normalized RB_DEPTHCONTROL in bl
    return m.run(0x5BAFDF, {0x5BAFEA: "wait", 0x5BB01A: "no wait"}, rsp=FRAME)


# Normalized RB_DEPTHCONTROL values: off, z on, z+write on, z off with other bits.
DEPTHS = (0x00, 0x02, 0x06, 0x75)


@pytest.mark.parametrize("async_shaders", [0, 1, 2])
def test_issue_draw_wait_rule(emus, base_image, patched_image, async_shaders):
    for m in emus.values():
        apply_settings(m, async_shaders=async_shaders)
    experimental = async_shaders == 2
    diff = Differences()
    wrong = []
    # color_destblend: Zero, One, SrcAlpha... and 16 (SrcAlphaSaturate, bit 12).
    cases = itertools.product(READBACK, (0, 0xF), (0, 1, 4, 16), INDICES, DEPTHS, PITCHES)
    for n, (readback, color_mask, destblend, indices, depth, pitch) in enumerate(cases):
        case = dict(readback=readback, color_mask=color_mask, destblend=destblend,
                    indices=indices, depth=depth, pitch=pitch)
        label, regs = run_issue_draw(emus["patched"], seed=n, **case)
        want = expected_wait(experimental=experimental, **case)
        if (label == "wait") != want:
            wrong.append(f"{case}: {label}")
        base_label, base_regs = run_issue_draw(emus["base"], seed=n, **case)
        assert (base_label == "wait") == (readback == 2), case
        assert regs["rsp"] == FRAME
        diff.add(0x5BAFEA if label == "wait" else 0x5BB01A, base_regs, regs)
    assert not wrong, f"{len(wrong)} wrong decisions, e.g. " + "; ".join(wrong[:5])
    # rbx: the cave shifts bl; patches.py says rbx is dead on both continuations.
    diff.assert_dead(patched_image, base_image)


# --- Patch 8b: ConfigurePipeline queue priority (code caves 3 and 3b) ---------------


def expected_priority(*, priority, indices, depth, pitch, experimental):
    normalized = depth & ~6 if depth & 0x76 == 0x72 else depth  # GetNormalizedDepthControl
    z_enable = normalized >> 1 & 1
    small_or_full = pitch <= 256 or (indices <= 6 and not (experimental and z_enable))
    return 4 if priority >= 2 and small_or_full else priority


def run_configure(m, *, priority, indices, depth, pitch, seed):
    set_draw_registers(m, indices=indices, pitch=pitch, depth=depth)
    m.write_u64(THIS + 8, REGS)
    rbp = FRAME + 0x400
    m.write_u64(rbp - 0xA8, 0x1122334455667788)
    m.poison(seed)
    m.set_reg("rbx", THIS)
    m.set_reg("rbp", rbp)
    m.set_reg("r12", (m.reg("r12") & ~0xFF) | priority)
    m.set_reg("r13", 0)
    before = m.reg("r12")
    label, regs = m.run(0x5F0B98, {0x5F0B9F: "push"}, rsp=FRAME)
    return before, regs


# Raw RB_DEPTHCONTROL: off, z on, z+write on, z on with zfunc ALWAYS and no write
# (normalized to z off), z off with other bits.
RAW_DEPTHS = (0x00, 0x02, 0x06, 0x72, 0x75)


@pytest.mark.parametrize("async_shaders", [0, 1, 2])
def test_configure_pipeline_priority(emus, base_image, patched_image, async_shaders):
    for m in emus.values():
        apply_settings(m, async_shaders=async_shaders)
    experimental = async_shaders == 2
    diff = Differences()
    wrong = []
    for n, (priority, indices, depth, pitch) in enumerate(itertools.product(range(4), INDICES, RAW_DEPTHS, PITCHES)):
        case = dict(priority=priority, indices=indices, depth=depth, pitch=pitch)
        before, regs = run_configure(emus["patched"], seed=n, **case)
        want = expected_priority(experimental=experimental, **case)
        if regs["r12"] & 0xFF != want:
            wrong.append(f"{case}: priority {regs['r12'] & 0xFF}, expected {want}")
        assert regs["r12"] >> 8 == before >> 8, "only r12b may change"
        assert regs["r13"] == 0x1122334455667788, "the replayed mov r13, [rbp-0xa8]"
        assert regs["rsp"] == FRAME
        base_before, base_regs = run_configure(emus["base"], seed=n, **case)
        assert base_regs["r12"] == base_before
        diff.add(0x5F0B9F, base_regs, regs, ignore={"r12"})
    assert not wrong, f"{len(wrong)} wrong priorities, e.g. " + "; ".join(wrong[:5])
    diff.assert_dead(patched_image, base_image)


# --- Patch 13: launch, per-game FSI (code cave 8) -----------------------------------


@pytest.mark.parametrize("fsi_wanted", [0, 1])
def test_launch_fsi_per_game(emus, fsi_wanted):
    """Settings field 0x38 (fsi): 0 Desligada -> false, 1 Padrão -> FsiWantedFor(title),
    2 Ligada -> true. v1.0.2 always asks FsiWantedFor."""
    rbp = FRAME + 0x1000
    for value in (0, 1, 2):
        results = {}
        for name, m in emus.items():
            m.stub(FSI_WANTED_FOR, "FsiWantedFor", fsi_wanted)
            m.write_u32(rbp - 0xCB0, value)
            m.poison(value)
            m.set_reg("rbp", rbp)
            m.set_reg("rdi", TITLE)
            before = m.snapshot()
            _, regs = m.run(0x689FB, {0x68A01: "returned"}, rsp=FRAME)
            results[name] = (regs, [c for c in m.calls])
            assert regs["rsp"] == FRAME
            for reg in x86.CALLEE_SAVED:
                assert regs[reg] == before[reg], f"{name}: {reg} changed"
        base_regs, base_calls = results["base"]
        assert base_regs["rax"] & 0xFF == fsi_wanted and len(base_calls) == 1
        regs, calls = results["patched"]
        if value == 1:
            assert [c[0] for c in calls] == ["FsiWantedFor"] and calls[0][1]["rdi"] == TITLE
            assert regs["rax"] & 0xFF == fsi_wanted
        else:
            assert calls == []
            assert regs["rax"] & 0xFF == (value == 2)


# --- Patch 7: Vulkan cache exit checkpoint ------------------------------------------


def test_exit_checkpoint_deadline(emus, base_image, patched_image):
    now = 123_456_789_000
    diff = Differences()
    regs = {}
    for name, m in emus.items():
        m.stub(STEADY_CLOCK_NOW, "steady_clock::now", now)
        m.poison(7)
        _, regs[name] = m.run(0x802EA, {0x802F6: "next"}, rsp=FRAME)
    assert regs["base"]["rax"] == now + 1_500_000_000
    assert regs["patched"]["rax"] == now + 30_000_000_000
    # The cave sets rcx (movabs) after the call; the stubs scramble the other
    # caller-saved registers differently in the two runs.
    diff.add(0x802F6, regs["base"], regs["patched"], ignore={"rax"})
    assert "rcx" in diff.by_stop[0x802F6]
    diff.assert_dead(patched_image, base_image)


# --- Patch 10: 60 FPS cap in VdSwap (code caves 4, 5 and 6) -------------------------


def run_vdswap(m, *, vsync, vsync_off, deadline, now, seed):
    m.stub(STEADY_CLOCK_NOW, "steady_clock::now", now)
    m.stub(NANO_SLEEP, "NanoSleep", 0)
    m.write_u8(VSYNC_CVAR, vsync)
    m.write_u8(PATCH_VSYNC_OFF, vsync_off)
    m.write_u64(SWAP_DEADLINE, deadline)
    saved = {"rbx": 0x1111, "r14": 0x2222, "rbp": 0x3333}
    m.write_u64(FRAME + 0x20, saved["rbx"])
    m.write_u64(FRAME + 0x28, saved["r14"])
    m.write_u64(FRAME + 0x30, saved["rbp"])
    m.write_u64(FRAME + 0x38, E.RETURN_SENTINEL)
    m.poison(seed)
    _, regs = m.run(0x7713DE, {E.RETURN_SENTINEL: "returned"}, rsp=FRAME)
    assert regs["rsp"] == FRAME + 0x40
    for reg, value in saved.items():
        assert regs[reg] == value, f"{reg} not restored"
    for reg in ("r12", "r13", "r15"):
        assert regs[reg] == m.snapshot()[reg]
    return regs, [c for c in m.calls], m.read_u64(SWAP_DEADLINE)


def test_vdswap_frame_cap(emus):
    mask = (1 << 64) - 1
    now = 50_000_000_000
    deadlines = (0, now + 5_000_000, now - 5_000_000, now - PERIOD_NS + 1, now - PERIOD_NS, now + PERIOD_NS)
    for n, (vsync, vsync_off, deadline) in enumerate(itertools.product((0, 1), (0, 1, 2), deadlines)):
        case = dict(vsync=vsync, vsync_off=vsync_off, deadline=deadline, now=now)
        _, base_calls, base_deadline = run_vdswap(emus["base"], seed=n, **case)
        assert base_calls == [] and base_deadline == deadline
        _, calls, new_deadline = run_vdswap(emus["patched"], seed=n, **case)
        if not vsync or vsync_off & 1:
            assert calls == [] and new_deadline == deadline, case
            continue
        assert [c[0] for c in calls] == ["steady_clock::now", "NanoSleep"], case
        assert calls[1][1]["rdi"] == (deadline - now) & mask, case  # <= 0 returns at once
        on_grid = (deadline + PERIOD_NS) & mask
        want = on_grid if now < on_grid else now + PERIOD_NS
        assert new_deadline == want, case


# --- The liveness analysis itself ----------------------------------------------------


def test_liveness_analysis_sees_live_registers(base_image):
    """At 0x5f0b9f (after cave 3b) ConfigurePipeline stores r12b and r13 into the
    creation arguments and loads rax/rcx/rdx/rdi first: the analysis must say so."""
    for reg in ("r12", "r13", "rbx", "rbp", "r14", "r15"):
        assert not x86.dead_at(base_image, 0x5F0B9F, reg), reg
    for reg in ("rax", "rcx", "rdx", "rdi"):
        assert x86.dead_at(base_image, 0x5F0B9F, reg), reg
    # 0x1c2126 in Settings::Apply is reached only after calls: rsi holds junk there.
    assert x86.junk_at(base_image, 0x1C2126, "rsi")
    assert not x86.junk_at(base_image, 0x1C2126, "rbx")
    assert not x86.junk_at(base_image, 0x1C2107, "rdi")  # just loaded from r15
