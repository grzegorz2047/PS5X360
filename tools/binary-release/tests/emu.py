"""A small unicorn harness that runs pieces of the v1.0.2 executable (base or
patched) from a hook site to the places it continues at.

Every PT_LOAD segment is mapped at its virtual address with its file bytes, so
code, constants and cvars are the executable's own. Functions that are not run
for real are stubbed: the stub returns a chosen value and scrambles the other
caller-saved registers, as a real call may.
"""
import random

from unicorn import UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_64, Uc
from unicorn import x86_const as R

PAGE = 0x1000
STACK_BASE, STACK_SIZE = 0x7F0000000000, 0x20000
SCRATCH = 0x7E0000000000  # objects the tests build (this, register file, settings)
SCRATCH_SIZE = 0x40000
RETURN_SENTINEL = 0x7D0000000000  # return address of the outermost frame

GPRS = {
    "rax": R.UC_X86_REG_RAX, "rbx": R.UC_X86_REG_RBX, "rcx": R.UC_X86_REG_RCX, "rdx": R.UC_X86_REG_RDX,
    "rsi": R.UC_X86_REG_RSI, "rdi": R.UC_X86_REG_RDI, "rbp": R.UC_X86_REG_RBP, "rsp": R.UC_X86_REG_RSP,
    "r8": R.UC_X86_REG_R8, "r9": R.UC_X86_REG_R9, "r10": R.UC_X86_REG_R10, "r11": R.UC_X86_REG_R11,
    "r12": R.UC_X86_REG_R12, "r13": R.UC_X86_REG_R13, "r14": R.UC_X86_REG_R14, "r15": R.UC_X86_REG_R15,
}
CALLER_SAVED = ["rax", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"]
STATUS_FLAGS = 0x8D5  # CF PF AF ZF SF OF


def _pages(lo, hi):
    return lo & ~(PAGE - 1), (hi + PAGE - 1) & ~(PAGE - 1)


class Stop(Exception):
    pass


class Emu:
    def __init__(self, image, seed=0):
        self.image = image
        self.uc = uc = Uc(UC_ARCH_X86, UC_MODE_64)
        spans = sorted(_pages(p[3], p[3] + p[5]) for p in image.loads if p[5])
        merged = []
        for lo, hi in spans:
            if merged and lo <= merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], hi)
            else:
                merged.append([lo, hi])
        for lo, hi in merged:
            uc.mem_map(lo, hi - lo)
        for p in image.loads:
            if p[4]:
                uc.mem_write(p[3], image.elf[p[2]: p[2] + p[4]])
        uc.mem_map(STACK_BASE, STACK_SIZE)
        uc.mem_map(SCRATCH, SCRATCH_SIZE)
        uc.mem_map(RETURN_SENTINEL, PAGE)
        uc.mem_write(RETURN_SENTINEL, b"\xf4" * PAGE)  # hlt
        self.rng = random.Random(seed)
        self.stubs = {}
        self.stops = {}
        self.calls = []
        self.writes = []
        self.result = None
        uc.hook_add(UC_HOOK_CODE, self._on_code)
        uc.hook_add(UC_HOOK_MEM_WRITE, self._on_write)

    # --- memory ------------------------------------------------------------------

    def write(self, addr, data: bytes):
        self.uc.mem_write(addr, bytes(data))

    def write_u8(self, addr, v):
        self.write(addr, v.to_bytes(1, "little"))

    def write_u32(self, addr, v):
        self.write(addr, (v & 0xFFFFFFFF).to_bytes(4, "little"))

    def write_u64(self, addr, v):
        self.write(addr, (v & (1 << 64) - 1).to_bytes(8, "little"))

    def read(self, addr, size) -> bytes:
        return bytes(self.uc.mem_read(addr, size))

    def read_u8(self, addr):
        return self.read(addr, 1)[0]

    def read_u64(self, addr):
        return int.from_bytes(self.read(addr, 8), "little")

    def clear_scratch(self):
        self.write(SCRATCH, bytes(SCRATCH_SIZE))

    # --- registers ---------------------------------------------------------------

    def reg(self, name):
        return self.uc.reg_read(GPRS[name])

    def set_reg(self, name, value):
        self.uc.reg_write(GPRS[name], value & (1 << 64) - 1)

    def snapshot(self):
        regs = {n: self.uc.reg_read(r) for n, r in GPRS.items()}
        regs["rflags"] = self.uc.reg_read(R.UC_X86_REG_EFLAGS) & STATUS_FLAGS
        return regs

    def poison(self, seed):
        """Distinct junk in every register (the same for the same seed), so a
        changed register shows when base and patched runs are compared."""
        rng = random.Random(seed)
        for i, name in enumerate(GPRS):
            if name != "rsp":
                self.set_reg(name, 0x5A5A000000000000 | (i << 40) | rng.getrandbits(32))
        self.uc.reg_write(R.UC_X86_REG_EFLAGS, 0x202 | (rng.getrandbits(12) & STATUS_FLAGS))

    # --- control -----------------------------------------------------------------

    def stub(self, addr, name, returns=0):
        """Calls to addr return `returns` (an int or a function of the Emu) at once."""
        self.stubs[addr] = (name, returns)

    def _on_code(self, uc, address, size, _):
        if address in self.stops:
            self.result = (self.stops[address], self.snapshot())
            uc.emu_stop()
            return
        if address in self.stubs:
            name, returns = self.stubs[address]
            self.calls.append((name, {n: self.reg(n) for n in ("rdi", "rsi", "rdx", "rcx")}))
            value = returns(self) if callable(returns) else returns
            for reg in CALLER_SAVED:
                self.set_reg(reg, 0x3C3C000000000000 | self.rng.getrandbits(40))
            self.set_reg("rax", value)
            rsp = self.reg("rsp")
            self.set_reg("rsp", rsp + 8)
            uc.reg_write(R.UC_X86_REG_RIP, self.read_u64(rsp))

    def _on_write(self, uc, access, address, size, value, _):
        self.writes.append((address, size, value & (1 << 8 * size) - 1))

    def run(self, start, stops, *, rsp=None, max_insns=5000):
        """Runs from start until an address in stops ({addr: label}); returns
        (label, registers at that point). Calls, memory writes are in .calls/.writes."""
        self.stops = dict(stops)
        self.calls, self.writes, self.result = [], [], None
        if rsp is not None:
            self.set_reg("rsp", rsp)
        self.uc.emu_start(start, RETURN_SENTINEL + PAGE, count=max_insns)
        if self.result is None:
            rip = self.uc.reg_read(R.UC_X86_REG_RIP)
            raise Stop(f"run from {start:#x} did not reach {sorted(map(hex, stops))} (stopped at {rip:#x})")
        return self.result

    def data_writes(self, below_rsp_ignored):
        """Final value of each byte written, ignoring the stack below the given rsp."""
        out = {}
        for addr, size, value in self.writes:
            for k in range(size):
                a = addr + k
                if STACK_BASE <= a < below_rsp_ignored:
                    continue
                out[a] = (value >> 8 * k) & 0xFF
        return out
