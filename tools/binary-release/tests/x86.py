"""Capstone helpers: linear sweeps, relative branches and a small register
liveness search used to show that registers a patch changes are dead."""
import capstone
from capstone import x86_const as X

_md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
_md.detail = True


def decode(code: bytes, addr: int):
    """Instructions of code at addr, stopping at the first undecodable byte."""
    return list(_md.disasm(code, addr))


def sweep(image, start: int, end: int):
    """Linear sweep of [start, end): {address: instruction}; undecodable bytes are
    skipped one at a time and listed in the second result."""
    code = image.read(start, end - start)
    insns, bad, pos = {}, [], 0
    while pos < len(code):
        for ins in _md.disasm(code[pos:], start + pos):
            insns[ins.address] = ins
            pos += ins.size
        if pos < len(code):
            bad.append(start + pos)
            pos += 1
    return insns, bad


def is_branch(ins) -> bool:
    return ins.group(X.X86_GRP_JUMP) or ins.group(X.X86_GRP_CALL)


def is_call(ins) -> bool:
    return ins.group(X.X86_GRP_CALL)


def is_ret(ins) -> bool:
    return ins.group(X.X86_GRP_RET)


def is_uncond_jmp(ins) -> bool:
    return ins.id == X.X86_INS_JMP


def rel_target(ins):
    """Target of a direct (relative) jmp/jcc/call, else None."""
    if not is_branch(ins) or len(ins.operands) != 1:
        return None
    op = ins.operands[0]
    return op.imm if op.type == X.X86_OP_IMM else None


def rip_refs(ins):
    """Absolute addresses of the instruction's RIP-relative memory operands."""
    out = []
    for op in ins.operands:
        if op.type == X.X86_OP_MEM and op.mem.base == X.X86_REG_RIP:
            out.append(ins.address + ins.size + op.mem.disp)
    return out


# --- Register liveness -----------------------------------------------------------

GPRS = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"]
CALLEE_SAVED = {"rbx", "rbp", "rsp", "r12", "r13", "r14", "r15"}
ARG_REGS = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"}
_ALIASES = {
    "rax": "rax eax ax al ah", "rbx": "rbx ebx bx bl bh", "rcx": "rcx ecx cx cl ch",
    "rdx": "rdx edx dx dl dh", "rsi": "rsi esi si sil", "rdi": "rdi edi di dil",
    "rbp": "rbp ebp bp bpl", "rsp": "rsp esp sp spl",
}
for _n in range(8, 16):
    _ALIASES[f"r{_n}"] = f"r{_n} r{_n}d r{_n}w r{_n}b"
_ALIASES["rflags"] = "rflags"
FULL = {alias: reg for reg, names in _ALIASES.items() for alias in names.split()}
# Writing these (64- and 32-bit) aliases replaces the whole register.
_FULL_WRITE = {alias for alias in FULL if alias.startswith("e") or alias.endswith("d")} | set(_ALIASES)


def _access(ins):
    regs_read, regs_write = ins.regs_access()
    reads = {FULL[_md.reg_name(r)] for r in regs_read if _md.reg_name(r) in FULL}
    writes, partial = set(), set()
    for r in regs_write:
        name = _md.reg_name(r)
        if name in FULL:
            (writes if name in _FULL_WRITE else partial).add(FULL[name])
    # A partial write (e.g. `shr bl, 1`) keeps the rest of the register: a read.
    reads |= partial
    # inc/dec keep CF: count their flag write as partial.
    if ins.id in (X.X86_INS_INC, X.X86_INS_DEC) and "rflags" in writes:
        writes.discard("rflags")
        reads.add("rflags")
    # `xor eax, eax` style zeroing reads nothing.
    if ins.id in (X.X86_INS_XOR, X.X86_INS_SUB) and len(ins.operands) == 2 \
            and ins.operands[0].type == ins.operands[1].type == X.X86_OP_REG \
            and ins.operands[0].reg == ins.operands[1].reg:
        reads -= writes
    return reads, writes


def junk_at(image, addr: int, reg: str) -> bool:
    """True if, in the function holding addr, every path that reaches addr last set
    the caller-saved register by a call (so it holds junk no code may read).

    Walks predecessors backwards over the function's linear sweep: fall-through
    and direct branches. An address with no known predecessor (function entry,
    jump-table target) counts as a meaningful value.
    """
    if reg in CALLEE_SAVED:
        return False
    func = image.function_at(addr)
    if func is None:
        return False
    insns, _ = sweep(image, *func)
    preds = {}
    for a, ins in insns.items():
        stops = is_ret(ins) or is_uncond_jmp(ins) or ins.id in (X.X86_INS_UD2, X.X86_INS_HLT)
        if not stops:
            preds.setdefault(a + ins.size, []).append(a)
        t = rel_target(ins)
        if t is not None and ins.group(X.X86_GRP_JUMP) and func[0] <= t < func[1]:
            preds.setdefault(t, []).append(a)
    seen, todo = set(), [addr]
    while todo:
        node = todo.pop()
        if not preds.get(node):
            return False
        for p in preds[node]:
            if p in seen:
                continue
            seen.add(p)
            ins = insns[p]
            if is_call(ins):
                continue  # junk on this path
            if reg in _access(ins)[1]:
                return False
            todo.append(p)
    return True


def dead_at(image, addr: int, reg: str, *, max_insns=600, void_return=False, depth=3):
    """True if on every path from addr the register is written before it is read.

    Follows direct jumps and both ways of conditional jumps. A call clobbers the
    caller-saved registers; it reads an argument register if the called function
    does (looked at the same way, `depth` calls deep; an indirect call counts as
    reading every argument register). A return reads the callee-saved registers
    and rax/rdx (unless void_return). "rflags" is tracked like a register (calls
    clobber it, returns do not read it). Indirect jumps and running out of budget
    count as a read (not provably dead).
    """
    return _dead(image, addr, reg, max_insns, void_return, depth, callee=False)


_callee_reads = {}


def _callee_reads_arg(image, target, reg, max_insns, depth):
    if target is None or depth <= 0:
        return True
    key = (id(image), target, reg)
    if key not in _callee_reads:
        _callee_reads[key] = not _dead(image, target, reg, max_insns, True, depth - 1, callee=True)
    return _callee_reads[key]


def _dead(image, addr, reg, max_insns, void_return, depth, callee):
    seen = set()
    stack = [addr]
    budget = max_insns
    while stack:
        pc = stack.pop()
        while True:
            if pc in seen:
                break
            seen.add(pc)
            budget -= 1
            if budget < 0:
                return False
            ins = decode(image.read(pc, 16), pc)[0]
            reads, writes = _access(ins)
            if is_call(ins):
                if reg in reads - {"rsp"}:
                    return False  # call through the register
                if reg in ARG_REGS and _callee_reads_arg(image, rel_target(ins), reg, max_insns, depth):
                    return False
                if reg not in CALLEE_SAVED:
                    break  # clobbered by the call: dead on this path
                pc += ins.size
                continue
            if is_ret(ins):
                if callee:
                    break  # the caller-saved register was not read by this function
                if reg in CALLEE_SAVED or (reg in ("rax", "rdx") and not void_return):
                    return False
                break
            if reg in reads:
                return False
            if reg in writes:
                break
            if ins.group(X.X86_GRP_JUMP):
                target = rel_target(ins)
                if target is None:
                    return False
                if is_uncond_jmp(ins):
                    pc = target
                    continue
                stack.append(target)
            if ins.id in (X.X86_INS_UD2, X.X86_INS_INT3, X.X86_INS_HLT):
                break
            pc += ins.size
    return True
