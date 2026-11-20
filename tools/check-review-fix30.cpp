// SPDX-License-Identifier: MIT
// Fixes 35-37 against fix 30 (Battlefield 3's memory-export readback). Fix 30 stores into guest
// memory only the dwords an exporting draw's shader wrote: before the draw it invalidates the
// export ranges in the GPU's copy itself (memexport_merge::RefreshRanges, through
// SharedMemory::MemoryInvalidationCallback), requests them without allow_unwatched (so they are
// pinned watched), copies the GPU's copy before and after the draw into its own buffer
// (memexport_readback_buffer_) and merges with memexport_merge::ApplyExports.
//
// 1. A model of the shared memory (4 KiB units: valid, unwatched, pinned; 16 KiB host pages; write
//    faults; guest and GPU copies) runs a Battlefield 3-like level twice, with fix 30's real
//    RefreshRanges and ApplyExports: once as before the review (allocations next to the job arena
//    invalidate whole 16 KiB pages, unwatched units are only hashed) and once with fix 37's
//    exact_unwatch::Compute and fix 36's unwatched_policy::OnRequest. Worker threads write job
//    slots, the game allocates and frees 4 KiB blocks in the arena's first and last host pages, draws
//    read vertex data next to the arena with allow_unwatched, and every frame an exporting draw sets
//    a slot's affinity byte. Checked: at every draw the shader reads the guest's bytes; every
//    export lands; no worker write is undone; the guest memory at the end is the same byte for byte
//    with and without the review fixes.
// 2. In the assembled tree: fix 35's readback slots stay in the resolve readback (ReadbackBuffer),
//    fix 30's memexport code uses none of it; fix 30's RefreshRanges, the export RequestRange
//    without allow_unwatched and ApplyExports are where fix 30 put them; fix 37 does not touch
//    SharedMemory::MemoryInvalidationCallback.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/exact_unwatch.h"
#include "xenia/gpu/memexport_merge.h"
#include "xenia/gpu/unwatched_policy.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace mm = xbox360ps5::memexport_merge;
namespace eu = xe::exact_unwatch;
namespace up = xe::gpu::unwatched_policy;

constexpr uint32_t kUnit = 0x1000, kPageShift = 14, kPage = 1u << kPageShift;
constexpr uint32_t kMemory = 0x100000;                    // 1 MiB of physical memory modelled
constexpr uint32_t kArena = 0x41000, kArenaSize = 0x60000;  // the job arena, 384 KiB, mid-page
constexpr uint32_t kQueue = kArena + 0x2000;              // the job queue: 16 slots of 8 bytes
constexpr uint32_t kVertex = 0xB0000;                     // vertex data the draws read

struct Shared {
  bool review = false;
  std::vector<uint8_t> guest = std::vector<uint8_t>(kMemory), gpu = std::vector<uint8_t>(kMemory);
  std::vector<bool> valid = std::vector<bool>(kMemory / kUnit), unwatched = std::vector<bool>(kMemory / kUnit),
                    pinned = std::vector<bool>(kMemory / kUnit), seen = std::vector<bool>(kMemory / kUnit);
  std::vector<uint8_t> heat = std::vector<uint8_t>(kMemory / kUnit);
  std::vector<uint64_t> hash = std::vector<uint64_t>(kMemory / kUnit);
  std::vector<uint32_t> changed = std::vector<uint32_t>(kMemory / kUnit);
  uint32_t frame = 0;
  int faults = 0, hashes = 0, rewatched = 0;

  uint64_t Hash(uint32_t unit) const {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < kUnit; ++i) h = (h ^ guest[unit * kUnit + i]) * 1099511628211ull;
    return h;
  }
  // SharedMemory::MemoryInvalidationCallback (fix 30's RefreshRanges calls it with exact = true).
  void MemoryInvalidationCallback(uint32_t start, uint32_t length, bool) {
    for (uint32_t u = start / kUnit; u <= (start + length - 1) / kUnit; ++u) valid[u] = false;
  }
  // A guest store: a valid (protected) unit faults and is invalidated (NoteCpuWrite's heat).
  void CpuWrite(uint32_t address, const void* data, size_t size) {
    for (uint32_t u = address / kUnit; u <= uint32_t((address + size - 1) / kUnit); ++u) {
      if (valid[u] && !unwatched[u]) {
        ++faults;
        valid[u] = false;
        if (!pinned[u] && ++heat[u] >= 3) {
          unwatched[u] = true;
          seen[u] = false;
          heat[u] = 32;
        }
      }
    }
    std::memcpy(&guest[address], data, size);
  }
  // PhysicalHeap::TriggerCallbacks with an exact range (Alloc/Release): before fix 37 whole host
  // pages, with it the exact range.
  void ExactTrigger(uint32_t address, uint32_t length) {
    if (review) {
      const eu::Range r = eu::Compute(address, length, 0, kPageShift, 0, kMemory >> kPageShift);
      MemoryInvalidationCallback(r.physical_start, r.physical_length, true);
    } else {
      const uint32_t first = address & ~(kPage - 1), end = (address + length + kPage - 1) & ~(kPage - 1);
      MemoryInvalidationCallback(first, end - first, true);
    }
  }
  // RequestRange: invalid units are uploaded; with allow_unwatched an unwatched unit is hashed and
  // uploaded only when changed (fix 36: watched again once unchanged for 30 frames); without it
  // the units are pinned watched (MakeRangeValid's PinWatched).
  void RequestRange(uint32_t start, uint32_t length, bool allow_unwatched) {
    for (uint32_t u = start / kUnit; u <= (start + length - 1) / kUnit; ++u) {
      if (!allow_unwatched) {
        pinned[u] = true;
        unwatched[u] = false;
      }
      if (unwatched[u]) {
        ++hashes;
        const uint64_t h = Hash(u);
        bool upload = !(seen[u] && hash[u] == h);
        if (review) {
          uint32_t c = changed[u];
          const up::Decision d = up::OnRequest(seen[u], hash[u], h, c, frame);
          changed[u] = c;
          upload = d.upload;
          if (d.watch_again) {
            unwatched[u] = false;
            seen[u] = false;
            heat[u] = 0;
            ++rewatched;
          }
        }
        if (upload) {
          std::memcpy(&gpu[u * kUnit], &guest[u * kUnit], kUnit);
          hash[u] = h;
          seen[u] = true;
        }
        if (!unwatched[u]) valid[u] = true;
        continue;
      }
      if (!valid[u]) {
        valid[u] = true;  // protected first, then copied
        std::memcpy(&gpu[u * kUnit], &guest[u * kUnit], kUnit);
      }
    }
  }
};

void Put32(Shared& s, uint32_t address, uint32_t value) { s.CpuWrite(address, &value, 4); }
uint32_t Get32(const std::vector<uint8_t>& m, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, &m[address], 4);
  return v;
}

struct Outcome {
  std::vector<uint8_t> guest;
  int exports = 0, faults = 0, hashes = 0, rewatched = 0;
};

Outcome Level(bool review) {
  Shared s;
  s.review = review;
  for (uint32_t a = 0; a < kMemory; a += 4) {
    const uint32_t v = a * 2654435761u;
    std::memcpy(&s.guest[a], &v, 4);
  }
  s.RequestRange(kArena, kArenaSize, false);  // the arena reaches the GPU once at the start
  Outcome o;
  uint32_t worker_counter = 0;
  for (uint32_t frame = 1; frame <= 240; ++frame) {
    s.frame = frame;
    // Workers take and push jobs: slot job pointers (the first dword of each slot).
    const uint32_t slot = frame % 16;
    Put32(s, kQueue + slot * 8, 0x40000000u | ++worker_counter);
    Put32(s, kArena + 0x30000 + (frame % 64) * 4, worker_counter);  // a dependency link elsewhere
    // The game allocates and frees 4 KiB blocks in the arena's first and last host pages.
    if (frame % 3 == 0) s.ExactTrigger(kArena & ~(kPage - 1), kUnit);
    if (frame % 5 == 0) s.ExactTrigger(kArena + kArenaSize, kUnit);
    // Vertex data: written for 3 frames at the start (leaves it unwatched), then static.
    if (frame <= 4) Put32(s, kVertex + frame * 4, frame);
    for (int draw = 0; draw < 20; ++draw) {
      s.RequestRange(kVertex, 0x2000, true);
      for (uint32_t a = kVertex; a < kVertex + 0x2000; a += 4) assert(Get32(s.gpu, a) == Get32(s.guest, a));
    }
    // The exporting draw (fix 30): refresh, request, before copy, shader export, merge.
    std::vector<mm::Range> ranges = {{kQueue, 16 * 8}};
    mm::RefreshRanges(s, ranges);
    s.RequestRange(kQueue, 16 * 8, false);
    std::vector<uint8_t> before(&s.gpu[kQueue], &s.gpu[kQueue] + 128);
    for (uint32_t a = kQueue; a < kQueue + 128; a += 4) assert(Get32(s.gpu, a) == Get32(s.guest, a));
    const uint32_t target = kQueue + ((frame * 7) % 16) * 8 + 4;  // a slot's affinity word
    const uint32_t exported = 0x3F000000u | frame;
    std::memcpy(&s.gpu[target], &exported, 4);
    std::vector<uint8_t> after(&s.gpu[kQueue], &s.gpu[kQueue] + 128);
    const uint32_t worker_value = Get32(s.guest, kQueue + slot * 8);
    const mm::Merged merged = mm::ApplyExports(&s.guest[kQueue], before.data(), after.data(), 128);
    assert(merged.written == 1);                                 // only the shader's dword
    assert(Get32(s.guest, target) == exported);                  // the export landed
    assert(Get32(s.guest, kQueue + slot * 8) == worker_value);   // the worker's slot kept
    o.exports += Get32(s.guest, target) == exported ? 1 : 0;
  }
  o.guest = s.guest;
  o.faults = s.faults;
  o.hashes = s.hashes;
  o.rewatched = s.rewatched;
  return o;
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void CheckTree() {
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string mem = Read(".deps/xenia-canary/src/xenia/memory.cc");
  const std::string sm = Read(".deps/xenia-canary/src/xenia/gpu/shared_memory.cc");
  assert(!cp.empty() && !mem.empty() && !sm.empty());
  // Fix 30's memexport code: from its ShouldReadBack to the end of its readback.
  const size_t ex_begin = cp.find("xbox360ps5::memexport_merge::ShouldReadBack(");
  const size_t ex_end = cp.find("dfn.vkUnmapMemory(device, memexport_readback_buffer_memory_);");
  assert(ex_begin != std::string::npos && ex_end != std::string::npos && ex_begin < ex_end);
  const std::string exports = cp.substr(ex_begin, ex_end - ex_begin);
  assert(exports.find("memexport_merge::RefreshRanges(*shared_memory_, ranges);") != std::string::npos);
  assert(exports.find("readback_slots::") == std::string::npos);
  assert(exports.find("readback_buffers_") == std::string::npos);
  assert(exports.find("ReadbackBuffer& ") == std::string::npos);  // the resolve map's entries
  assert(exports.find("RequestReadbackBuffer(") != std::string::npos);
  // The export ranges are requested without allow_unwatched (pinned, so fix 36 never filters them).
  const size_t request = exports.find("shared_memory_->RequestRange(memexport_range_base_bytes,");
  assert(request != std::string::npos);
  const std::string call = exports.substr(request, exports.find(')', request) - request);
  assert(call.find("true") == std::string::npos);
  // Fix 35's slots: only in the resolve readback, after fix 30's code.
  assert(cp.find("readback_slots::ChooseWrite(") > ex_end);
  // Fix 36 filters only requests that allow unwatched units; fix 37 changes PhysicalHeap only.
  assert(sm.find("if (allow_unwatched && UnwatchedPagesEnabled()) {") != std::string::npos);
  const size_t callback = sm.find("SharedMemory::MemoryInvalidationCallback(");
  assert(callback != std::string::npos);
  assert(sm.find("exact_unwatch", 0) == std::string::npos);
  assert(mem.find("exact_unwatch::Compute(") > mem.find("bool PhysicalHeap::TriggerCallbacks("));
  std::puts("assembled tree: fix 30's export path untouched by fixes 35-37");
}
}  // namespace

int main() {
  const Outcome before = Level(false), after = Level(true);
  std::printf("before the review: %d exports, %d faults, %d hashes; with fixes 36/37: %d exports, %d faults, %d hashes, "
              "%d units watched again\n",
              before.exports, before.faults, before.hashes, after.exports, after.faults, after.hashes, after.rewatched);
  assert(before.exports == 240 && after.exports == 240);
  assert(before.guest == after.guest);  // the same guest memory byte for byte
  assert(after.rewatched >= 1 && after.hashes < before.hashes);
  CheckTree();
  std::puts("check-review-fix30: all passed");
  return 0;
}
