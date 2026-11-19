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
    #    Only draws that overwrite render target 0 wait (RB_BLENDCONTROL0
    #    color_destblend == Zero): with alpha, additive or premultiplied
    #    blending the stand-in's transparent black leaves the picture as it is,
    #    and waiting on every new particle, smoke or HUD quad froze the game for
    #    seconds when shooting on a first run (2026-11-19 log: 22-36 waits per
    #    30 s at ~0.3-1.5 s per compile).
    #    Reads this->register_file_ (this at [rsp+0x18], file at +0x38):
    #    RB_BLENDCONTROL0 (reg 0x2201, color_destblend = bits 8..12),
    #    VGT_DRAW_INITIATOR (reg 0x21FC, num_indices = bits 16..31) and
    #    RB_SURFACE_INFO (reg 0x2000, surface_pitch = bits 0..13; IssueDrawInner
    #    has already returned for pitch 0).
    patch_vaddr(
        0x1507108,
        "cc" * 55,
        "e80359b9fe83f8020f84d43e0bff83bc24a0000000000f84f63e0bff488b442418488b4038f680058800001f0f85e03e0bffe9181d59ff",
        "Code cave 1 (int3 padding after ret): readback full -> wait; no colour or blended RT0 -> no wait",
    )
    #    With the experimental async-shader mode on (patch 12, flag byte
    #    0x3926582), a draw with <= 6 indices waits only when the depth test is
    #    off (full-screen passes); depth-tested quads (billboards, decals) use the
    #    stand-in, which patch 12 then draws invisible. ebx holds the normalized
    #    RB_DEPTHCONTROL here (bit 1 z_enable). Cave 2 ends on the original
    #    `jne 0x5bb01a` kept at 0x5bafe8 (ZF=1 <=> 1 <= pitch <= 256 -> wait;
    #    pitch 0 already returned at 0x5ba5df). The cave shifts bl; rbx is dead
    #    on both continuations (redefined at 0x5bb064 or popped).
    patch_vaddr(
        0xA98E57,
        "cc" * 41,
        "6683b8f287000006770ed0eb841d19d7e8020f847b21b2ff8b8000800000ffc8f6c43fe96921b2ffcc",
        "Code cave 2 (int3 padding after jmp): <=6 indices (and depth off in experimental mode) or pitch <= 256 -> wait",
    )
    patch_vaddr(
        0x5BAFDF,
        "67e82b1aaeff83f8027530",
        "e924c1f4000f1f40007530",
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
    #    In the experimental mode (patch 12) a <= 6-index pipeline is boosted
    #    only when its normalized depth test is off, matching patch 8. The hook
    #    sits at 0x5f0b98 (after the queue lock, before `mov r13,[rbp-0xa8]`,
    #    which a 12-byte cave at 0x1500131 replays): at 0x5f0bed rdi is live.
    #    GetNormalizedDepthControl (0xfc790) is a leaf on the register file.
    patch_vaddr(
        0x150EDC8,
        "cc" * 56,
        "4180fc02722d488b7b088b8700800000ffc8f6c43f74196683bff2870000067712e8a2d9befed0e884058c774102750341b404e93113ffff",
        "Code cave 3 (int3 padding after jmp): priority 4 for full-screen / small-target colour pipelines",
    )
    patch_vaddr(
        0x1500131,
        "cc" * 12,
        "4c8bad58ffffffe9620a0fff",
        "Code cave 3b (int3 padding after ret): replay mov r13,[rbp-0xa8], back to ConfigurePipeline",
    )
    patch_vaddr(
        0x5F0B98,
        "4c8bad58ffffff",
        "e92be2f1006690",
        "ConfigurePipeline: queue priority goes through the boost cave before the push",
    )

    # 10. VSync on = at most 60 frames a second. On the PS5 the guest gets 60
    #     vblanks a second whatever the VSync option says, so it changed only the
    #     host presentation. Games unlocked by a 60 FPS patch with a fixed 1/60 s
    #     step (Battlefield: Bad Company) then ran at 80+ frames and too fast.
    #     VdSwap (0x7712a0) now paces swaps on a 16.60 ms grid (about 60.2 Hz, a
    #     little faster than the 60 Hz vblank so vblank-paced games never wait)
    #     when cvars::vsync (0x3926570) is set and no patch asked for VSync off
    #     (patch_vsync_off, 0x3955818). An early swap sleeps to its deadline; a
    #     swap late by less than one period keeps the grid (the next frames may
    #     catch up, so a fixed-step game keeps its average speed); a swap further
    #     behind, or the first one, restarts the grid at now + 16.60 ms.
    #     The deadline is kept in gpu_diag's protect_nanoseconds (0x395cf10):
    #     patch 2 left it receiving only `lock add 0` and the performance reports
    #     only read it, so their "memory watches ... ms/s" figures are meaningless
    #     in this build. Uses steady_clock::now (0x19a3690) and
    #     xe::threading::NanoSleep (0x57bf50), which returns at once for <= 0 ns.
    patch_vaddr(
        0x7713DE,
        "4883c4205b415e5dc5f877c3",
        "c5f877e9ae8bd900cccccccc",
        "VdSwap epilogue: vzeroupper, then the 60 FPS cap",
    )
    patch_vaddr(
        0x1509F94,
        "cc" * 43,
        "803dd5c54102000f84617426fff60570b84402010f85547426ffe8dd964900488b0d562f4502e95a290000",
        "Code cave 4 (int3 after ret): VSync off or patch_vsync_off -> no cap; now(); load deadline",
    )
    patch_vaddr(
        0x150C919,
        "cc" * 37,
        "4889cf4829c7488d91c04bfd004839d07207488d90c04bfd00488915d7054502e9e3ba0aff",
        "Code cave 5 (int3 after ret): sleep = deadline-now; next = deadline+P unless behind by >= P (then now+P)",
    )
    patch_vaddr(
        0x5B8421,
        "cc" * 10,
        "e82a3bfcffe9d78f1b00",
        "Code cave 6 (int3 after ud2): NanoSleep(deadline-now), then the epilogue",
    )
    patch_vaddr(
        0x771402,
        "cc" * 9,
        "4883c4205b415e5dc3",
        "VdSwap epilogue moved into the padding after the function (int3 after jmp)",
    )

    # 11. Frame generation settings on read-only .ffpkg installs. The launcher
    #     writes the choice to /app0/framegen.conf.new and renames it over
    #     /app0/framegen.conf; on a .ffpkg /app0 is read-only, so the choice was
    #     never saved and the presenter kept reading the packaged
    #     "ps5_frame_interpolation=false". Both strings now live in the unused
    #     rest of the RenderDoc help text slot that patch 6 reused (zeroed after
    #     its cache path), and the six lea that named them point there. Missing
    #     file: the menu shows Off and the presenter falls back to
    #     /data/homebrew/PPSA50011/framegen.conf, as v1.0.2 already does.
    patch_vaddr(
        0x1DA8036,
        "00" * 76,
        "2f646f776e6c6f6164302f78626f783336307073352f6672616d6567656e2e636f6e6600"
        "2f646f776e6c6f6164302f78626f783336307073352f6672616d6567656e2e636f6e662e6e657700",
        "strings /download0/xbox360ps5/framegen.conf and .new in the reused help-text slot",
    )
    for vaddr, old, new, desc in (
        (0x1D9851, "488d359b08bb01", "488d35dee7bc01", "launcher FramegenChoice reads framegen.conf"),
        (0x613319, "488d3dd36d7701", "488d3d164d7901", "presenter reads framegen.conf first"),
        (0x1D9FA3, "488d354901bb01", "488d358ce0bc01", "WriteFramegen rename target framegen.conf"),
        (0x1D9F12, "488d35ad34bc01", "488d3541e1bc01", "WriteFramegen writes framegen.conf.new"),
        (0x1D9F9C, "488d3d2334bc01", "488d3db7e0bc01", "WriteFramegen rename source framegen.conf.new"),
        (0x1D9FB4, "488d3d0b34bc01", "488d3d9fe0bc01", "WriteFramegen removes framegen.conf.new"),
    ):
        patch_vaddr(vaddr, old, new, f"framegen path -> /download0/xbox360ps5: {desc}")

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

    # 12. Experimental async-shader mode: "Shaders em segundo plano" gets a third
    #     choice, "Ligado (experimental)" (value 2; still on for every other use,
    #     Apply keeps `async_shader_compilation = value != 0`). Settings::Apply
    #     also stores (value == 2) into the byte 0x3926582, alignment padding
    #     after the bool cvar 0x3926581 that nothing else reads or writes (no
    #     RIP reference, no relocation). Patches 8 and 8b read it; here it also
    #     makes stand-in pipelines with a colour output invisible: no colour
    #     writes, no depth writes, stencil ops KEEP (depth/stencil tests stay,
    #     so occlusion query counts stay close to the old stand-in's). A stand-in
    #     without colour output (depth
    #     prepass, shadow maps) keeps writing depth as before. Values 0 and 1
    #     leave the flag 0 and every path as it was.
    patch_cstring(
        0x1DA8082,
        b"\x00" * 21,
        b"Ligado (experimental)",
        "label of the third async_shaders choice (zeroed tail of the reused help-text slot)",
    )
    patch_vaddr(
        0x1BD933,
        "488d058958c1014c8dbd20f8ffff488d157974ba01488d0d34b4b401488dbd50f8ffff48898520f8ffff48c78528f8ffff"
        "8800000048c78530f8ffff01000000c78538f8ffff01000000c6853cf8ffff0148899540f8ffff48898d48f8ffff488d35"
        "47c87903e8520d0000",
        "904c8dbd20f8ffff488d058158c10149890749c7470888000000488d056d74ba0149894720488d0524b4b40149894728488b"
        "0576c879030f1000410f114748488d0509a7be0149894758b80100000049894710418947184188471c498d7748498d7f30"
        "8d5002e8820c0000",
        "Options(): async_shaders choices = {off_on[0], off_on[1], \"Ligado (experimental)\"} via vector(ptr, 3)",
    )
    patch_vaddr(
        0x1C2126,
        "83bb8400000000488d052c3979030f9500488d054344760383bb88000000000f9500488d059938790383bb8c000000000f9500",
        "488db384000000833e000f9505293979038b460485c00f95053e44760383f8020f940535447603837e08000f95059038790390",
        "Settings::Apply: also store async_shaders == 2 into the experimental flag 0x3926582",
    )
    #     Stand-in look: VulkanPipelineCache::EnsurePipelineCreated (0x5fa580),
    #     right before vkCreateGraphicsPipelines (subpass store at 0x5fb0ae).
    #     [rbp-0x70] is the fragment shader override (non-null only for the
    #     stand-in). The cave reuses the debug-level "Creating graphics pipeline
    #     state with VS ..." log block, which a jmp at 0x5fa5d8 now skips: that
    #     line no longer appears, even with detailed logs (level 3) on.
    patch_vaddr(
        0x5FA5D8,
        "4885c07461",
        "e98c000000",
        "EnsurePipelineCreated: skip the debug-level pipeline log block (cave space)",
    )
    patch_vaddr(
        0x5FA5DD,
        "488b4b08488b4908488b492848898d50fcffff488b4008488b402848898540fdffffbf03000000be0800000067e811b6c5ff"
        "84c07456488d15e9e577014c8d8550fcffff4c8d8d40fdffffb93c000000bf03000000be47000000e8c9900000eb2b488b43"
        "08488b4008488b402848898550fcff",
        "c785b8fdffff00000000803d94bf320300745b48837d900074548b85ecfbffff0b850cfcffff0b852cfcffff0b854cfcffff"
        "743a31c0898528feffff48898538feffff898540feffff48898554feffff89855cfeffff8985ecfbffff89850cfcffff8985"
        "2cfcffff89854cfcffffe9680a0000",
        "Code cave 7 (dead log block): experimental flag + stand-in with colour -> no colour/depth/stencil writes",
    )
    patch_vaddr(
        0x5FB0AE,
        "c785b8fdffff00000000",
        "e92af5ffff0f1f440000",
        "EnsurePipelineCreated: subpass store goes through code cave 7",
    )

    return bytes(elf)


