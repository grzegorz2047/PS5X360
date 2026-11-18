"""Binary patches applied to the official PS5X360 v1.0.2 eboot.bin ELF.

Each patch states the bytes it expects, so a different base executable fails
loudly instead of being patched in the wrong place. Addresses are virtual
addresses; the ELF file offset is 0x4000 + vaddr for this executable.
"""

# SHA-256 of the ELF inside the official v1.0.2 eboot.bin these patches target.
BASE_ELF_SHA256 = "bde8a5c6c289c4e2f4c120a0b867ebc086602b30ed4a3ee9ec5b5a82fc98f42e"


def apply(elf_bytes: bytes) -> bytes:
    elf = bytearray(elf_bytes)

    def patch_vaddr(vaddr: int, expected_hex: str, replacement_hex: str, desc: str):
        off = 0x4000 + vaddr
        exp = bytes.fromhex(expected_hex)
        rep = bytes.fromhex(replacement_hex)
        assert len(exp) == len(rep), f"Length mismatch for {desc}"
        actual = bytes(elf[off : off + len(exp)])
        assert actual == exp, f"Byte mismatch at {vaddr:#x} ({desc}): expected {exp.hex()}, got {actual.hex()}"
        elf[off : off + len(rep)] = rep
        print(f"[PATCHED] {vaddr:#x}: {exp.hex()} -> {rep.hex()} ({desc})")

    # 1. CPU JIT (c110e294 & c2795e9c): Drop redundant mfence barriers around ldarx/lwarx/stdcx/stwcx
    #    and use INT32_TYPE (2) instead of INT64_TYPE (3) in stwcx StoreWithReserve
    patch_vaddr(0xA13640, "67e8dabafeff", "660f1f440000", "InstrEmit_ldarx: drop f.MemoryBarrier()")
    patch_vaddr(0xA13751, "67e8c9b9feff", "660f1f440000", "InstrEmit_lwarx: drop f.MemoryBarrier()")
    patch_vaddr(0xA138C2, "67e858b8feff", "660f1f440000", "InstrEmit_stdcx: drop f.MemoryBarrier()")
    patch_vaddr(0xA139AA, "b903000000",   "b902000000",   "InstrEmit_stwcx: INT32_TYPE for StoreWithReserve")
    patch_vaddr(0xA13A03, "67e817b7feff", "660f1f440000", "InstrEmit_stwcx: drop f.MemoryBarrier()")

    # 2. PS5 Memory (memory_ps5.cpp::Apply): Remove 2x unconditional clock_gettime(CLOCK_MONOTONIC) syscalls per mprotect
    patch_vaddr(0x29CDAE, "ffd0",         "6690",         "memory_ps5::Apply: skip pre-mprotect clock_gettime")
    patch_vaddr(0x29CDEA, "ff15709a5203", "660f1f440000", "memory_ps5::Apply: skip post-mprotect clock_gettime")

    # 3. PS5 Exception Handler (exception_handler.cpp::callback): Skip 256B XMM state copy on SIGSEGV faults
    patch_vaddr(0x121CE1, "7573",         "eb73",         "exception_handler::callback: skip 256B XMM copy")

    # 4. PS5 Zen 2 Thread Placement (thread_place.cpp): Enable dedicated core (CPU 2/3) for GPU Commands thread
    patch_vaddr(0x7CCB72, "7454",         "6690",         "thread_place::PlaceCallingThread: dedicated GPU core enabled")
    patch_vaddr(0x7CCBE0, "0fb60599301a032401", "b001660f1f44000090", "thread_place::GpuCoreDedicated: return true")

    # 5. Redirect FSI marker/config paths from read-only /app0 (.ffpkg UFS) to writable /download0/xbox360ps5
    def patch_cstring(vaddr: int, expected: bytes, replacement: bytes, desc: str):
        assert len(replacement) <= len(expected), f"Replacement too long for {desc}"
        off = 0x4000 + vaddr
        exp_nul = expected + b"\x00"
        rep_nul = replacement.ljust(len(exp_nul), b"\x00")
        actual = bytes(elf[off : off + len(exp_nul)])
        assert actual == exp_nul, f"String mismatch at {vaddr:#x} ({desc}): expected {exp_nul!r}, got {actual!r}"
        elf[off : off + len(rep_nul)] = rep_nul
        print(f"[PATCHED] {vaddr:#x}: {expected.decode()} -> {replacement.decode()} ({desc})")

    fsi_redirects = [
        (0x1D6E396, b"/app0/render-target-fsi.once", b"/download0/xbox360ps5/fsi.1", "FsiAtStart/FsiForNextStart once path"),
        (0x1D9D4BF, b"/app0/render-target-fsi.conf", b"/download0/xbox360ps5/fsi.c", "FsiChoice/SetFsiRequested conf path"),
        (0x1E12FF1, b"/app0/render-target-fsi.conf.new", b"/download0/xbox360ps5/fsi.c.new", "SetFsiRequested temp conf path"),
    ]
    for vaddr, exp_s, rep_s, desc in fsi_redirects:
        patch_cstring(vaddr, exp_s, rep_s, desc)

    # 6. Redirect bulk cache/utility-cache candidate from read-only /app0/cache (.ffpkg UFS)
    #    to /data/homebrew/PPSA50011/cache (~16+ GiB internal SSD) so Halo Reach utility-cache
    #    and shader/module caches never exhaust the 500 MiB /download0 sandbox partition.
    patch_cstring(
        0x1DA8016,
        b"Disable RoundingModeRTE capability in SPIR-V shaders. Enable this to allow shader debugging in RenderDoc, which doesn't support this capability.",
        b"/data/homebrew/PPSA50011/cache",
        "Repurpose RenderDoc cvar help slot for /data/homebrew/PPSA50011/cache",
    )
    patch_vaddr(
        0x65D74,
        "488d354bebd701",
        "488d359b22d401",
        "canary_game_main: redirect bulk cache candidate /app0/cache -> /data/homebrew/PPSA50011/cache",
    )

    # 7. Extend Vulkan cache exit checkpoint timeout from 1500 ms (0x59682f00 ns)
    #    to 30000 ms (0x6fc23ac00 ns = 30,000,000,000 ns) so ~30 MB .vk.bin cache
    #    saves complete cleanly on exit instead of stopping as .vk.bin.pending-0.
    patch_vaddr(
        0x010111,
        "cccccccccccccccccccccccccccc",
        "48b900ac23fc060000004801c8c3",
        "Code cave (inter-function int3 padding): add 30000 ms (30e9 ns) to rax",
    )
    patch_vaddr(
        0x0802EA,
        "67e8a03392014805002f6859",
        "e8a1339201e81dfef8ff6690",
        "exit checkpoint: call steady_clock::now() then add 30000 ms via cave instead of 1500 ms",
    )
    patch_cstring(
        0x1DC1867,
        b"Vulkan cache: exit checkpoint timed out after 1500 ms",
        b"Vulkan cache: exit checkpoint timed out after 30000ms",
        "exit checkpoint timeout log message (1500 ms -> 30000ms)",
    )

    # 8. Async shaders: wait for the real pipeline on the draws whose stand-in
    #    output is visible or kept, keep the stand-in for the rest.
    #    v1.0.2 waits only with readback "full". Now IssueDrawInner also waits
    #    (the existing wait path at 0x5bafea) when the draw writes colour
    #    (normalized_color_mask at [rsp+0xa0] != 0; colourless kill/alpha-test
    #    and occlusion draws have the lowest queue priority and gain nothing)
    #    and it has <= 6 indices (full-screen composite/post-processing/tone-
    #    mapping quads) or renders into a target at most 256 pixels wide
    #    (reflection cube faces, the HDR luminance chain). Battlefield: Bad Company showed purple surfaces and
    #    baked a purple reflection cube while ~470 pipelines compiled on a first
    #    run; skipping stand-in draws instead did not help (tested 2026-11-18).
    #    Reads this->register_file_ (this at [rsp+0x18], file at +0x38):
    #    VGT_DRAW_INITIATOR (reg 0x21FC, num_indices = bits 16..31) and
    #    RB_SURFACE_INFO (reg 0x2000, surface_pitch = bits 0..13).
    patch_vaddr(
        0x1507108,
        "cc" * 56,
        "e80359b9fe83f8020f84d43e0bff83bc24a0000000000f84f63e0bff488b442418488b40386683b8f2870000060f86af3e0bffe9171d59ff",
        "Code cave 1 (int3 padding after ret): readback full -> wait; no colour -> no wait; <=6 indices -> wait",
    )
    patch_vaddr(
        0xA98E57,
        "cc" * 33,
        "8b800080000025ff3f00000f84b221b2ff3d000100000f867721b2ffe9a221b2ff",
        "Code cave 2 (int3 padding after jmp): render target pitch <= 256 -> wait",
    )
    patch_vaddr(
        0x5BAFDF,
        "67e82b1aaeff83f8027530",
        "e924c1f400660f1f440000",
        "IssueDrawInner: stand-in decision goes through the selective wait caves",
    )

    # 8b. Compile the pipelines that patch 8 waits for first. The creation
    #    queue is a max-heap on PipelineCreationArguments::priority (0..3), and
    #    equal priorities come out in no particular order, so a waited
    #    pipeline could sit behind hundreds of others: Battlefield: Bad Company
    #    froze for 15 s and more on a first run (2026-11-18 log). In
    #    VulkanPipelineCache::ConfigurePipeline (this in rbx, register_file_ at
    #    [this+8], priority in r12b), a pipeline that writes a visible colour
    #    target (priority >= 2) and matches patch 8's rule (<= 6 indices, or a
    #    render target at most 256 pixels wide) gets priority 4, ahead of all.
    patch_vaddr(
        0x150EDC8,
        "cc" * 55,
        "4180fc027225488b43086683b8f28700000676148b800080000025ff3f0000740a3d00010000770341b4044488a508ffffffe9f51d0eff",
        "Code cave 3 (int3 padding after jmp): priority 4 for full-screen / small-target colour pipelines",
    )
    patch_vaddr(
        0x5F0BED,
        "4488a508ffffff",
        "e9d6e1f1006690",
        "ConfigurePipeline: queue priority goes through the boost cave before the push",
    )

    # 9. On-screen notice while the picture stops (shader compilation, loading):
    #    FrameWatch::Notice() in the title's main loop shows the "no new frames"
    #    banner after 45 s without a refreshed output. Point its compare at an
    #    existing 2.0 constant instead (the 45.0 constant itself is shared with
    #    FrameWatch::Observe and stays). The banner text gains "(compiling
    #    shaders or loading)" through assets/lang-overrides.
    patch_vaddr(
        0x06B3D6,
        "c5f92e053ac7c901",
        "c5f92e05fac5c901",
        "main loop: no-new-frames banner after 2 s (vucomisd vs 2.0 at 0x1d079d8, was 45.0 at 0x1d07b18)",
    )

    return bytes(elf)


