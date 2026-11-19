// SPDX-License-Identifier: MIT
// Fix 35 (tools/source-1.0.2/fixes/35-readback-slots.patch): which buffer a resolve readback
// writes and which earlier copy the guest gets. Drives xe::gpu::vulkan::readback_slots (the
// assembled core's xenia/gpu/vulkan/readback_slots.h) through simulated frames (one submission
// per frame; the GPU `lag` frames behind) and compares it with 1.0.2's two buffers flipped once
// per frame:
//  * a key resolved every frame: both deliver while the GPU keeps up; with the GPU one or two
//    frames behind 1.0.2's automatic copy never delivers, fix 35 always does;
//  * a key resolved every other frame (luminance, HDR adaptation): 1.0.2 never delivers, fix 35
//    always does (after the first);
//  * every delivered copy is completed (automatic) and older than the copy just made, and is the
//    newest such copy; no copy in flight is overwritten while a superseded one is free; a key
//    uses two buffers while the GPU keeps up and at most four;
//  * the fast mode (it may wait) reads the newest earlier copy even if not completed;
//  * the assembled vulkan_command_processor.cc uses it and no longer flips per frame.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/gpu/vulkan/readback_slots.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace rs = xe::gpu::vulkan::readback_slots;
constexpr uint32_t kLength = 4096;

struct Result {
  int uses = 0, delivered = 0, max_age = 0;
  uint32_t slots_used = 0;
};

// 1.0.2: two slots, current flipped at every frame's start; write current, read the other one,
// only when completed (the automatic copy never waits).
Result Old(int frames, int every, int lag) {
  uint64_t subs[2] = {0, 0};
  uint32_t current = 0;
  Result r;
  for (int f = 0; f < frames; ++f) {
    current = 1 - current;
    const uint64_t s = uint64_t(f) + 1;
    const uint64_t completed = s > uint64_t(lag) + 1 ? s - uint64_t(lag) - 1 : 0;
    if (f % every) continue;
    ++r.uses;
    const uint32_t w = current, rd = 1 - w;
    subs[w] = s;
    if (subs[rd] && subs[rd] <= completed) {
      ++r.delivered;
      r.max_age = std::max(r.max_age, int(s - subs[rd]));
    }
  }
  return r;
}

Result New(int frames, int every, int lag, bool completed_only = true) {
  uint64_t subs[rs::kCount] = {};
  uint32_t valid[rs::kCount] = {};
  Result r;
  bool used[rs::kCount] = {};
  for (int f = 0; f < frames; ++f) {
    const uint64_t s = uint64_t(f) + 1;
    const uint64_t completed = s > uint64_t(lag) + 1 ? s - uint64_t(lag) - 1 : 0;
    if (f % every) continue;
    ++r.uses;
    const int keep = rs::ChooseRead(subs, valid, rs::kCount, -1, kLength, completed, true);
    const uint32_t w = rs::ChooseWrite(subs, valid, rs::kCount, kLength, completed);
    assert(w < rs::kCount);
    assert(int(w) != keep);  // the copy the guest may get is never overwritten
    // A copy still in flight is overwritten only when no slot is free or superseded.
    if (subs[w] > completed) {
      for (uint32_t i = 0; i < rs::kCount; ++i)
        assert(int(i) == keep || (subs[i] && (keep < 0 || subs[i] >= subs[keep])));
    }
    const int rd = rs::ChooseRead(subs, valid, rs::kCount, int(w), kLength, completed, completed_only);
    subs[w] = s;
    valid[w] = kLength;
    used[w] = true;
    if (rd >= 0) {
      assert(rd != int(w) && subs[rd] < s);
      if (completed_only) assert(subs[rd] <= completed);
      // The newest such copy.
      for (uint32_t i = 0; i < rs::kCount; ++i)
        if (int(i) != int(w) && subs[i] && (!completed_only || subs[i] <= completed)) assert(subs[i] <= subs[rd]);
      ++r.delivered;
      r.max_age = std::max(r.max_age, int(s - subs[rd]));
    }
  }
  for (bool u : used) r.slots_used += u;
  return r;
}

void Report(const char* what, const Result& r) {
  std::printf("  %-44s %3d of %3d delivered, oldest %d frame(s), %u buffer(s)\n", what, r.delivered, r.uses,
              r.max_age, r.slots_used);
}

void CheckFrames() {
  constexpr int kFrames = 120;
  for (int lag = 0; lag <= 2; ++lag) {
    std::printf("GPU %d frame(s) behind:\n", lag);
    const Result old1 = Old(kFrames, 1, lag), new1 = New(kFrames, 1, lag);
    const Result old2 = Old(kFrames, 2, lag), new2 = New(kFrames, 2, lag);
    const Result old3 = Old(kFrames, 3, lag), new3 = New(kFrames, 3, lag);
    Report("1.0.2, key every frame", old1);
    Report("fix 35, key every frame", new1);
    Report("1.0.2, key every other frame", old2);
    Report("fix 35, key every other frame", new2);
    Report("1.0.2, key every third frame", old3);
    Report("fix 35, key every third frame", new3);
    // Fix 35 delivers at every use but the first few (until a copy has completed).
    assert(new1.delivered >= new1.uses - (lag + 1));
    assert(new2.delivered >= new2.uses - 1 - lag / 2);
    assert(new3.delivered >= new3.uses - 1);
    // ... and never an older copy than needed.
    assert(new1.max_age == lag + 1);
    assert(new2.max_age <= 2 + (lag > 1 ? 2 : 0));
    // Buffers: two while the GPU keeps up, at most kCount.
    if (lag == 0) assert(new1.slots_used == 2 && new2.slots_used == 2);
    assert(new1.slots_used <= rs::kCount);
    // 1.0.2: every other frame never delivers; behind, the automatic copy never does.
    assert(old2.delivered == 0);
    if (lag == 0) assert(old1.delivered == old1.uses - 1);
    else assert(old1.delivered == 0);
  }
  // The fast mode (waits when needed): the newest earlier copy, completed or not.
  const Result fast = New(kFrames, 1, 2, false);
  assert(fast.delivered == fast.uses - 1 && fast.max_age == 1);
  std::puts("frames: ok");
}

void CheckEdges() {
  uint64_t subs[rs::kCount] = {};
  uint32_t valid[rs::kCount] = {};
  // Nothing written: no read, the first slot written.
  assert(rs::ChooseRead(subs, valid, rs::kCount, -1, kLength, 100, true) == -1);
  assert(rs::ChooseWrite(subs, valid, rs::kCount, kLength, 100) == 0);
  // A copy too short for this length is not read.
  subs[0] = 5;
  valid[0] = kLength / 2;
  assert(rs::ChooseRead(subs, valid, rs::kCount, -1, kLength, 100, true) == -1);
  assert(rs::ChooseWrite(subs, valid, rs::kCount, kLength, 100) == 1);  // unused before an unreadable one
  // All in flight: the oldest is reused.
  uint64_t busy[rs::kCount] = {10, 11, 12, 13};
  uint32_t full[rs::kCount] = {kLength, kLength, kLength, kLength};
  assert(rs::ChooseWrite(busy, full, rs::kCount, kLength, 5) == 0);
  // The newest completed one is kept, a superseded one taken before an unused one.
  uint64_t mixed[rs::kCount] = {7, 9, 0, 12};
  assert(rs::ChooseRead(mixed, full, rs::kCount, -1, kLength, 10, true) == 1);
  assert(rs::ChooseWrite(mixed, full, rs::kCount, kLength, 10) == 0);
  // Excluding the write slot.
  assert(rs::ChooseRead(mixed, full, rs::kCount, 1, kLength, 10, true) == 0);
  assert(rs::ChooseRead(mixed, full, rs::kCount, 1, kLength, 10, false) == 3);
  std::puts("edges: ok");
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void CheckTree() {
  const std::string cc = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string h = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
  assert(!cc.empty() && !h.empty());
  assert(h.find("#include \"xenia/gpu/vulkan/readback_slots.h\"") != std::string::npos);
  assert(h.find("VkBuffer buffers[readback_slots::kCount]") != std::string::npos);
  assert(h.find("current_index") == std::string::npos);
  assert(cc.find("readback_slots::ChooseWrite(") != std::string::npos);
  assert(cc.find("readback_slots::ChooseRead(") != std::string::npos);
  assert(cc.find("1 - write_index") == std::string::npos);
  assert(cc.find("current_index") == std::string::npos);
  assert(cc.find("buffers[1]") == std::string::npos);
  std::puts("assembled vulkan_command_processor: ok");
}
}  // namespace

int main() {
  CheckEdges();
  CheckFrames();
  CheckTree();
  std::puts("check-readback-slots: all passed");
  return 0;
}
