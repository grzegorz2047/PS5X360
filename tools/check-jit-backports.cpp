// SPDX-License-Identifier: MIT
// Fix 38 (tools/source-1.0.2/fixes/38-jit-canary-backports.patch): x64 JIT changes from upstream
// Xenia Canary (GitHub issue 5). The emitters run only inside the JIT, so this check replays the
// instruction sequences they emit with the same instructions as intrinsics (SSE4.1/AVX2 on the
// host), against the sequences they replace and the PowerPC definitions:
//  * d59084973, __savegprlr_N / __restgprlr_N inline: the stack slots the inline code uses equal
//    the ones the real helpers' std/ld and stw/lwz instructions address, for N = 14..31, and the
//    0xE0000000 adjustment matches the memory access sequences';
//  * 38a40c513, vavgsb / vavgsh without a loop: equal to (a + b + 1) >> 1 for all 65,536 byte
//    pairs and 2,000,000 halfword pairs (edges included);
//  * b2e6ef10b, vpkuhum / vpkuhus / vpkuwum / vpkuwus inline: equal to the C emulation and the
//    shuffle-and-blend sequences they replace, on 200,000 random vectors and the edges;
//  * a52f5176c (adapted to the core's sites), scalar conversions merging from the source: the low
//    lane is unchanged and the result no longer depends on the destination's old value;
//  * the assembled core has all four.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include <immintrin.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace {
std::mt19937_64 rng(0x5EED);

__m128i Random128() {
  return _mm_set_epi64x(int64_t(rng()), int64_t(rng()));
}
bool Equal(__m128i a, __m128i b) { return _mm_movemask_epi8(_mm_cmpeq_epi8(a, b)) == 0xFFFF; }

// --- d59084973: __savegprlr_N / __restgprlr_N ---------------------------------------------------
// The real helper's instructions: std rN, -((33-N)*8)(r1) ... std r31, -0x10(r1); stw r12, -8(r1).
uint32_t EncodeStd(uint32_t rs, int32_t d) { return (62u << 26) | (rs << 21) | (1u << 16) | (uint32_t(d) & 0xFFFC); }
uint32_t EncodeStw(uint32_t rs, int32_t d) { return (36u << 26) | (rs << 21) | (1u << 16) | (uint32_t(d) & 0xFFFF); }
int32_t Disp16(uint32_t word) { return int16_t(word & 0xFFFF) & (((word >> 26) == 62) ? ~3 : ~0); }

void CheckSaveRestore() {
  for (unsigned first = 14; first <= 31; ++first) {
    // The helper body as the XEX has it (the __savegprlr_N entry point falls through to r31).
    uint32_t words[20];
    unsigned n = 0;
    for (unsigned r = first; r <= 31; ++r) words[n++] = EncodeStd(r, -int32_t((33 - r) * 8));
    words[n++] = EncodeStw(12, -8);
    // The inline code's slots (x64_emitter.cc): disp = -first_slot_offset + (reg - first) * 8.
    const int32_t first_slot_offset = int32_t((33 - first) * 8);
    for (unsigned r = first; r <= 31; ++r) {
      const int32_t disp = -first_slot_offset + int32_t((r - first) * 8);
      const uint32_t word = words[r - first];
      assert(((word >> 21) & 31) == r && ((word >> 16) & 31) == 1);
      assert(Disp16(word) == disp);
    }
    assert(Disp16(words[n - 1]) == -8 && ((words[n - 1] >> 21) & 31) == 12);
  }
  // The stack pointer's host address: +0x1000 at or above 0xE0000000 when the host's allocation
  // granularity is larger than 4 KiB (the PS5's 16 KiB), as ComputeMemoryAddress does.
  auto adjusted = [](uint32_t r1, uint32_t granularity) {
    uint32_t eax = r1;
    if (granularity > 0x1000) {
      const uint32_t edx = r1 + 0x1000;  // lea edx, [rax + 0x1000]
      if (r1 >= 0xE0000000u) eax = edx;   // cmp eax, 0xE0000000; cmovae eax, edx
    }
    return eax;
  };
  assert(adjusted(0x7FFEFF00u, 0x4000) == 0x7FFEFF00u);
  assert(adjusted(0xE0001000u, 0x4000) == 0xE0002000u);
  assert(adjusted(0xDFFFFFF0u, 0x4000) == 0xDFFFFFF0u);
  assert(adjusted(0xE0001000u, 0x1000) == 0xE0001000u);
  std::puts("save/restore helper slots for r14..r31 and LR: ok");
}

// --- 38a40c513: vavgsb / vavgsh -----------------------------------------------------------------
__m128i NewAvgSB(__m128i a, __m128i b) {
  const __m128i m = _mm_set1_epi8(char(0x80));
  return _mm_xor_si128(_mm_avg_epu8(_mm_xor_si128(a, m), _mm_xor_si128(b, m)), m);
}
__m128i NewAvgSH(__m128i a, __m128i b) {
  const __m128i m = _mm_set1_epi16(short(0x8000));
  return _mm_xor_si128(_mm_avg_epu16(_mm_xor_si128(a, m), _mm_xor_si128(b, m)), m);
}

void CheckAverages() {
  for (int x = -128; x < 128; ++x) {
    alignas(16) int8_t a[16], b[16], r[16];
    for (int y0 = -128; y0 < 128; y0 += 16) {
      for (int k = 0; k < 16; ++k) { a[k] = int8_t(x); b[k] = int8_t(y0 + k); }
      _mm_store_si128(reinterpret_cast<__m128i*>(r),
                      NewAvgSB(_mm_load_si128(reinterpret_cast<__m128i*>(a)), _mm_load_si128(reinterpret_cast<__m128i*>(b))));
      for (int k = 0; k < 16; ++k) assert(r[k] == int8_t((int(a[k]) + int(b[k]) + 1) >> 1));
    }
  }
  const int16_t edges[] = {-32768, -32767, -2, -1, 0, 1, 2, 32766, 32767};
  for (int16_t x : edges)
    for (int16_t y : edges) {
      alignas(16) int16_t r[8];
      _mm_store_si128(reinterpret_cast<__m128i*>(r), NewAvgSH(_mm_set1_epi16(x), _mm_set1_epi16(y)));
      for (int k = 0; k < 8; ++k) assert(r[k] == int16_t((int(x) + int(y) + 1) >> 1));
    }
  for (int n = 0; n < 250000; ++n) {
    alignas(16) int16_t a[8], b[8], r[8];
    const __m128i va = Random128(), vb = Random128();
    _mm_store_si128(reinterpret_cast<__m128i*>(a), va);
    _mm_store_si128(reinterpret_cast<__m128i*>(b), vb);
    _mm_store_si128(reinterpret_cast<__m128i*>(r), NewAvgSH(va, vb));
    for (int k = 0; k < 8; ++k) assert(r[k] == int16_t((int(a[k]) + int(b[k]) + 1) >> 1));
  }
  std::puts("vavgsb (all byte pairs) and vavgsh: ok");
}

// --- b2e6ef10b: packs ---------------------------------------------------------------------------
// 1.0.2's C emulation (before the common vpshufb, which both keep).
__m128i OldPack8In16Sat(__m128i src1, __m128i src2) {
  alignas(16) uint16_t a[8], b[8];
  alignas(16) uint8_t c[16];
  _mm_store_si128(reinterpret_cast<__m128i*>(a), src1);
  _mm_store_si128(reinterpret_cast<__m128i*>(b), src2);
  for (int i = 0; i < 8; ++i) {
    c[i] = uint8_t(a[i] < 255 ? a[i] : 255);
    c[i + 8] = uint8_t(b[i] < 255 ? b[i] : 255);
  }
  return _mm_load_si128(reinterpret_cast<__m128i*>(c));
}
__m128i OldPack8In16(__m128i src1, __m128i src2) {
  alignas(16) uint8_t a[16], b[16], c[16];
  _mm_store_si128(reinterpret_cast<__m128i*>(a), src1);
  _mm_store_si128(reinterpret_cast<__m128i*>(b), src2);
  for (int i = 0; i < 8; ++i) {
    c[i] = a[i * 2];
    c[i + 8] = b[i * 2];
  }
  return _mm_load_si128(reinterpret_cast<__m128i*>(c));
}
__m128i NewPack8In16(__m128i src1, __m128i src2, bool saturate) {
  const __m128i ff = _mm_srli_epi16(_mm_cmpeq_epi16(src1, src1), 8);  // vpcmpeqw; vpsrlw 8
  const __m128i x0 = saturate ? _mm_min_epu16(src1, ff) : _mm_and_si128(src1, ff);
  const __m128i x1 = saturate ? _mm_min_epu16(src2, ff) : _mm_and_si128(src2, ff);
  return _mm_packus_epi16(x0, x1);
}
// 1.0.2's 16-in-32 unsigned sequences (shuffles and a blend).
__m128i OldPack16In32(__m128i src1, __m128i src2, bool saturate) {
  const __m128i max = _mm_set1_epi32(0xFFFF);
  __m128i x1 = saturate ? _mm_min_epu32(src1, max) : src1;
  x1 = _mm_shufflelo_epi16(x1, 0b00100010);
  x1 = _mm_shufflehi_epi16(x1, 0b00100010);
  x1 = _mm_shuffle_epi32(x1, 0b00001000);
  __m128i d = saturate ? _mm_min_epu32(src2, max) : src2;
  d = _mm_shufflelo_epi16(d, 0b00100010);
  d = _mm_shufflehi_epi16(d, 0b00100010);
  d = _mm_shuffle_epi32(d, 0b10000000);
  return _mm_blend_epi16(d, x1, 0b00001111);
}
__m128i NewPack16In32(__m128i src1, __m128i src2, bool saturate) {
  const __m128i ffff = _mm_srli_epi32(_mm_cmpeq_epi32(src1, src1), 16);  // vpcmpeqd; vpsrld 16
  const __m128i x0 = saturate ? _mm_min_epu32(src1, ffff) : _mm_and_si128(src1, ffff);
  const __m128i x1 = saturate ? _mm_min_epu32(src2, ffff) : _mm_and_si128(src2, ffff);
  __m128i d = _mm_packus_epi32(x0, x1);
  d = _mm_shufflelo_epi16(d, 0b10110001);
  return _mm_shufflehi_epi16(d, 0b10110001);
}

void CheckPacks() {
  auto check = [](__m128i a, __m128i b) {
    assert(Equal(NewPack8In16(a, b, true), OldPack8In16Sat(a, b)));
    assert(Equal(NewPack8In16(a, b, false), OldPack8In16(a, b)));
    assert(Equal(NewPack16In32(a, b, true), OldPack16In32(a, b, true)));
    assert(Equal(NewPack16In32(a, b, false), OldPack16In32(a, b, false)));
  };
  const uint32_t edges[] = {0, 1, 0xFE, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu};
  for (uint32_t x : edges)
    for (uint32_t y : edges) {
      check(_mm_set1_epi32(int(x)), _mm_set1_epi32(int(y)));
      check(_mm_set1_epi16(short(x)), _mm_set1_epi16(short(y)));
    }
  for (int n = 0; n < 200000; ++n) check(Random128(), Random128());
  std::puts("vpkuhum/vpkuhus/vpkuwum/vpkuwus against 1.0.2's sequences: ok");
}

// --- a52f5176c: merge source of the scalar conversions --------------------------------------------
void CheckMergeSource() {
  for (int n = 0; n < 100000; ++n) {
    const double v = double(int64_t(rng())) / double(1ull << (rng() % 60));
    const __m128d src = _mm_set_pd(1234.5, v);
    const __m128 dest_old = _mm_castsi128_ps(Random128());
    // vcvtsd2ss dest, dest, src (two-operand) against vcvtsd2ss dest, src, src: same low lane.
    const __m128 before = _mm_cvtsd_ss(dest_old, src);
    const __m128 after = _mm_cvtsd_ss(_mm_castpd_ps(src), src);
    assert(_mm_cvtss_f32(before) == _mm_cvtss_f32(after) ||
           (std::isnan(_mm_cvtss_f32(before)) && std::isnan(_mm_cvtss_f32(after))));
    // vcvtsi2sd with a zeroed merge source: low lane as before, upper lane 0 whatever dest held.
    const int64_t i = int64_t(rng());
    const __m128d with_dest = _mm_cvtsi64_sd(_mm_castps_pd(dest_old), i);
    const __m128d with_zero = _mm_cvtsi64_sd(_mm_setzero_pd(), i);
    assert(_mm_cvtsd_f64(with_dest) == _mm_cvtsd_f64(with_zero));
    alignas(16) double hi[2];
    _mm_store_pd(hi, with_zero);
    assert(hi[1] == 0.0);
    // vsqrtsd dest, src, src.
    const __m128d s = _mm_sqrt_sd(src, src), t = _mm_sqrt_sd(_mm_castps_pd(dest_old), src);
    assert(_mm_cvtsd_f64(s) == _mm_cvtsd_f64(t) ||
           (std::isnan(_mm_cvtsd_f64(s)) && std::isnan(_mm_cvtsd_f64(t))));
  }
  std::puts("scalar conversions merging from the source: ok");
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}
size_t Count(const std::string& text, const std::string& what) {
  size_t n = 0;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
  return n;
}

void CheckTree() {
  const std::string em = Read(".deps/xenia-canary/src/xenia/cpu/backend/x64/x64_emitter.cc");
  const std::string emh = Read(".deps/xenia-canary/src/xenia/cpu/backend/x64/x64_emitter.h");
  const std::string vec = Read(".deps/xenia-canary/src/xenia/cpu/backend/x64/x64_seq_vector.cc");
  const std::string seq = Read(".deps/xenia-canary/src/xenia/cpu/backend/x64/x64_sequences.cc");
  assert(!em.empty() && !emh.empty() && !vec.empty() && !seq.empty());
  // d59084973
  assert(emh.find("bool TryInlinePPCGprLrSaveRestore(const hir::Instr* instr,") != std::string::npos);
  assert(em.find("if (TryInlinePPCGprLrSaveRestore(instr, function)) {") != std::string::npos);
  assert(em.find("const int32_t first_slot_offset = static_cast<int32_t>((33 - first_gpr) * 8);") != std::string::npos);
  assert(em.find("const int32_t lr_slot_offset = -8;") != std::string::npos);
  // 38a40c513
  assert(vec.find("e.vpavgb(e.xmm1, e.xmm1, e.xmm2);") != std::string::npos);
  assert(vec.find("e.vpavgw(e.xmm1, e.xmm1, e.xmm2);") != std::string::npos);
  // b2e6ef10b
  assert(vec.find("EmulatePack8_IN_16_UN_UN") == std::string::npos);
  assert(vec.find("e.vpackuswb(i.dest, e.xmm0, e.xmm1);") != std::string::npos);
  assert(vec.find("e.vpackusdw(i.dest, e.xmm0, e.xmm1);") != std::string::npos);
  // a52f5176c, at every site the core has
  assert(Count(seq, "e.vroundsd(i.dest, src1, src1, 0b") == 4);
  assert(seq.find("e.vcvtsi2sd(i.dest, e.xmm0, input);") != std::string::npos);
  assert(seq.find("e.vcvtsd2ss(i.dest, src, src);") != std::string::npos);
  assert(seq.find("e.vcvtss2sd(i.dest, src, src);") != std::string::npos);
  assert(seq.find("e.vsqrtss(i.dest, src1, src1);") != std::string::npos);
  assert(seq.find("e.vsqrtsd(i.dest, src1, src1);") != std::string::npos);
  assert(Count(seq, "e.vcvtsd2ss(e.xmm1, e.xmm2, e.xmm2);") == 2);
  for (const char* two : {"e.vcvtsd2ss(i.dest, src);", "e.vcvtss2sd(i.dest, src);", "e.vcvtsi2sd(i.dest, input);",
                          "e.vcvtsd2ss(e.xmm1, e.xmm2);", "e.vcvtsd2ss(e.xmm0, srcreg);"})
    assert(seq.find(two) == std::string::npos);
  std::puts("assembled x64 backend: ok");
}
}  // namespace

int main() {
  CheckSaveRestore();
  CheckAverages();
  CheckPacks();
  CheckMergeSource();
  CheckTree();
  std::puts("check-jit-backports: all passed");
  return 0;
}
