// SPDX-License-Identifier: MIT
// Fix 37 (tools/source-1.0.2/fixes/37-exact-unwatch.patch): exact invalidations (PhysicalHeap
// Alloc, AllocRange, Decommit, Release, Protect) stay exact on the PS5's 16 KiB host pages.
// Drives xe::exact_unwatch (the assembled core's xenia/exact_unwatch.h) and compares it with
// 1.0.2's range, which TriggerCallbacks widened to whole host pages:
//  * a 4 KiB allocation inside a 16 KiB page: 1.0.2 invalidated the three neighbouring 4 KiB
//    units of the GPU copy (a resolve target there lost what the GPU drew), fix 37 only the
//    allocation's, and unprotects/unwatches no page (each is shared with memory outside);
//  * aligned 64 KiB: the same units, the four pages inside unwatched;
//  * the 0xE0000000 view (host pages 4 KiB off): pages wholly inside only;
//  * the heap's end and empty ranges;
//  * the assembled memory.cc uses it for exact requests only.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/exact_unwatch.h"

#include <algorithm>
#include <bitset>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {
namespace eu = xe::exact_unwatch;
constexpr uint32_t kShift = 14;  // 16 KiB host pages
constexpr uint32_t kPage = 1u << kShift;
constexpr uint32_t kUnit = 4096;  // shared_memory.cc's units

// 1.0.2's physical range for the callbacks (TriggerCallbacks: whole host pages).
void OldRange(uint32_t rel, uint32_t length, uint32_t host_offset, uint32_t phys_off, uint32_t heap_size,
              uint32_t& start, uint32_t& len) {
  const uint32_t first = (rel + host_offset) >> kShift;
  const uint32_t last = (rel + length - 1 + host_offset) >> kShift;
  auto sat_sub = [](uint32_t a, uint32_t b) { return a > b ? a - b : 0; };
  start = sat_sub(first << kShift, host_offset) + phys_off;
  len = std::min(sat_sub((last << kShift) + kPage, host_offset) + phys_off - start,
                 heap_size - (start - phys_off));
}

// The GPU copy's valid units of a 1 MiB region, all valid, then the callback's range cleared.
std::bitset<256> Invalidate(uint32_t start, uint32_t length) {
  std::bitset<256> valid;
  valid.set();
  for (uint32_t a = start & ~(kUnit - 1); a < start + length; a += kUnit) valid.reset(a / kUnit);
  return valid;
}

void CheckAllocInsidePage() {
  // A0000000-style heap: physical = heap-relative, host pages from 0.
  const uint32_t rel = 0x5000, length = 0x1000, heap = 0x100000;
  const eu::Range r = eu::Compute(rel, length, 0, kShift, 0, heap >> kShift);
  assert(r.physical_start == 0x5000 && r.physical_length == 0x1000);
  assert(!r.pages);  // page 1 (0x4000-0x7FFF) holds three other units
  uint32_t os, ol;
  OldRange(rel, length, 0, 0, heap, os, ol);
  assert(os == 0x4000 && ol == 0x4000);
  const auto old_valid = Invalidate(os, ol), new_valid = Invalidate(r.physical_start, r.physical_length);
  assert(old_valid.count() == 256 - 4);  // units 4, 6 and 7 lost with 5
  assert(new_valid.count() == 256 - 1 && !new_valid[5] && new_valid[4] && new_valid[6] && new_valid[7]);
  std::puts("4 KiB inside a 16 KiB page: 1.0.2 invalidated 4 units, fix 37 1, no page unwatched");
}

void CheckAligned() {
  const eu::Range r = eu::Compute(0x10000, 0x10000, 0, kShift, 0x20000000, 0x100000 >> kShift);
  assert(r.physical_start == 0x20010000 && r.physical_length == 0x10000);
  assert(r.pages && r.page_first == 4 && r.page_last == 7);
  // 20 KiB from 0x3000: units 3..7; page 1 (0x4000-0x7FFF) wholly inside, page 0 and 2 not.
  const eu::Range s = eu::Compute(0x3000, 0x5000, 0, kShift, 0, 64);
  assert(s.pages && s.page_first == 1 && s.page_last == 1);
  // 32 KiB from 0x6000: no whole page? 0x6000-0xDFFF covers page 2 (0x8000-0xBFFF) only.
  const eu::Range t = eu::Compute(0x6000, 0x8000, 0, kShift, 0, 64);
  assert(t.pages && t.page_first == 2 && t.page_last == 2);
  std::puts("aligned and partial ranges: ok");
}

void CheckOffsetView() {
  // 0xE0000000: host pages start 4 KiB before the heap's 0 (host_address_offset 0x1000).
  const uint32_t off = 0x1000;
  // 16 KiB at heap 0: host 0x1000-0x4FFF, no page wholly inside.
  eu::Range r = eu::Compute(0, 0x4000, off, kShift, 0x1000, 64);
  assert(!r.pages && r.physical_start == 0x1000 && r.physical_length == 0x4000);
  // 16 KiB at heap 0x3000: host 0x4000-0x7FFF, page 1 exactly.
  r = eu::Compute(0x3000, 0x4000, off, kShift, 0x1000, 64);
  assert(r.pages && r.page_first == 1 && r.page_last == 1);
  // 1.0.2 for the first: host pages 0 and 1, i.e. physical 0x1000.. 0x8FFF (from the heap's 0).
  uint32_t os, ol;
  OldRange(0, 0x4000, off, 0x1000, 0x100000, os, ol);
  assert(os == 0x1000 && ol == 0x7000);  // 3 units past the allocation
  std::puts("0xE0000000 view: ok");
}

void CheckEdges() {
  // Up to the heap's last page.
  eu::Range r = eu::Compute(0xFC000, 0x4000, 0, kShift, 0, 0x100000 >> kShift);
  assert(r.pages && r.page_first == 63 && r.page_last == 63);
  // Past it with the 4 KiB offset: clamped to the last page.
  r = eu::Compute(0xFB000, 0x5000, 0x1000, kShift, 0, 64);
  assert(r.pages && r.page_first == 63 && r.page_last == 63);
  // Nothing.
  r = eu::Compute(0x8000, 0, 0, kShift, 0, 64);
  assert(!r.pages && r.physical_length == 0);
  std::puts("edges: ok");
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void CheckTree() {
  const std::string cc = Read(".deps/xenia-canary/src/xenia/memory.cc");
  assert(!cc.empty());
  assert(cc.find("#include \"xenia/exact_unwatch.h\"") != std::string::npos);
  assert(cc.find("const bool caller_exact = unwatch_exact_range;") != std::string::npos);
  assert(cc.find("exact_unwatch::Compute(heap_relative_address, length,") != std::string::npos);
  assert(cc.find("physical_address_start = exact.physical_start;") != std::string::npos);
  assert(cc.find("system_page_first = exact.page_first;") != std::string::npos);
  // The exact range is taken before the callbacks run and the pages before unprotecting.
  assert(cc.find("exact_unwatch::Compute(") < cc.find("memory_->physical_memory_invalidation_callbacks_) {"));
  assert(cc.find("system_page_first = exact.page_first;") < cc.find("// Unprotect ranges that need unprotection."));
  std::puts("assembled memory.cc: ok");
}
}  // namespace

int main() {
  CheckAllocInsidePage();
  CheckAligned();
  CheckOffsetView();
  CheckEdges();
  CheckTree();
  std::puts("check-exact-unwatch: all passed");
  return 0;
}
