// SPDX-License-Identifier: MIT
// Fix 36 (tools/source-1.0.2/fixes/36-unwatched-rewatch.patch): unwatched units (the
// dynamic_buffers option) that stop changing are watched again. Drives
// xe::gpu::unwatched_policy (the assembled core's xenia/gpu/unwatched_policy.h) inside a model of
// shared_memory.cc's units: protected and valid, faulting on a guest write (heat; three faults
// leave a unit unwatched), unwatched units hashed at every request, heat halved every 120 frames.
// For three kinds of unit, each requested by 40 draws a frame for 900 frames:
//  * vertex data written four times while loading (the first before it is protected, then three
//    faults), then never: 1.0.2 hashes it at every request
//    until its heat cools (~600 frames); fix 36 watches it again after 30 frames: far fewer hashes,
//    no more faults;
//  * a ring buffer refilled every frame: never watched again (that would trade sub-microsecond
//    hashes for write faults); hashes and faults as 1.0.2's;
//  * data rewritten every 45 frames: correct, faults bounded;
//  * at every request the GPU copy equals the guest's bytes, in both policies;
//  * the assembled shared memory and command processor use it.
// Built against the assembled 1.0.2 tree and run from its root (run-fix-checks.sh).
#undef NDEBUG
#include "xenia/gpu/unwatched_policy.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>

namespace {
namespace up = xe::gpu::unwatched_policy;

struct Counts {
  int hashes = 0, faults = 0, uploads = 0, rewatched = 0;
};

// One 4 KiB unit as shared_memory.cc keeps it.
struct Unit {
  uint64_t guest = 1, gpu = 0;  // contents (a version number stands for the bytes)
  bool unwatched = false, valid = false, seen = false;
  uint8_t heat = 0;
  uint64_t hash = 0;
  uint32_t changed_frame = 0;
};

Counts Run(bool fix, const std::function<bool(int)>& written_in_frame, int frames = 900, int requests = 40) {
  Unit u;
  Counts c;
  for (int frame = 0; frame < frames; ++frame) {
    // The game writes before the frame's draws.
    if (written_in_frame(frame)) {
      ++u.guest;
      if (!u.unwatched && u.valid) {  // protected: the write faults (NoteCpuWrite)
        ++c.faults;
        u.valid = false;
        if (u.heat < 255) ++u.heat;
        if (u.heat >= 3) {
          u.unwatched = true;
          u.seen = false;
          u.heat = 32;
        }
      }
    }
    for (int r = 0; r < requests; ++r) {
      if (u.unwatched) {
        ++c.hashes;
        const uint64_t hash = u.guest;  // XXH3 of the guest's bytes
        bool upload;
        if (fix) {
          const up::Decision d = up::OnRequest(u.seen, u.hash, hash, u.changed_frame, uint32_t(frame));
          if (d.watch_again) {
            u.unwatched = false;
            u.seen = false;
            u.heat = 0;
            ++c.rewatched;
            u.valid = true;  // MakeRangeValid: protected, then copied
            u.gpu = u.guest;
            ++c.uploads;
            assert(u.gpu == u.guest);
            continue;
          }
          upload = d.upload;
        } else {
          upload = !(u.seen && u.hash == hash);
        }
        if (upload) {
          u.hash = hash;
          u.seen = true;
          u.gpu = u.guest;
          ++c.uploads;
        }
      } else if (!u.valid) {
        u.valid = true;
        u.gpu = u.guest;
        ++c.uploads;
      }
      assert(u.gpu == u.guest);  // what the draw reads is what the guest wrote
    }
    if (frame % 120 == 119) {  // AgeUnwatchedPages
      u.heat >>= 1;
      if (u.heat < 2 && u.unwatched) {
        u.unwatched = false;
        u.seen = false;
      }
    }
  }
  return c;
}

void Print(const char* what, const Counts& c) {
  std::printf("  %-40s %6d hashes %3d faults %5d uploads %2d watched again\n", what, c.hashes, c.faults, c.uploads,
              c.rewatched);
}

void CheckPolicy() {
  uint32_t changed = 0;
  // First sight: upload, remember the frame.
  up::Decision d = up::OnRequest(false, 0, 7, changed, 100);
  assert(d.upload && !d.watch_again && changed == 100);
  // Unchanged within the window: no upload.
  d = up::OnRequest(true, 7, 7, changed, 100 + up::kFramesUnchangedToWatch - 1);
  assert(!d.upload && !d.watch_again && changed == 100);
  // Changed: upload, window restarts.
  d = up::OnRequest(true, 7, 8, changed, 120);
  assert(d.upload && !d.watch_again && changed == 120);
  // Unchanged for the whole window: watched again (and uploaded).
  d = up::OnRequest(true, 8, 8, changed, 120 + up::kFramesUnchangedToWatch);
  assert(d.upload && d.watch_again);
  // A hash of something else than the GPU copy (not seen) is never taken as unchanged.
  changed = 0;
  d = up::OnRequest(false, 8, 8, changed, 1000);
  assert(d.upload && !d.watch_again && changed == 1000);
  // The frame counter wrapping around.
  changed = 0xFFFFFFF0u;
  d = up::OnRequest(true, 1, 1, changed, 0x0Au);  // 26 frames later
  assert(!d.upload && !d.watch_again);
  d = up::OnRequest(true, 1, 1, changed, 0xFFFFFFF0u + up::kFramesUnchangedToWatch);
  assert(d.watch_again);
  std::puts("policy: ok");
}

void CheckUnits() {
  auto loaded = [](int f) { return f < 4; };
  auto ring = [](int f) { (void)f; return true; };
  auto slow = [](int f) { return f < 3 || f % 45 == 0; };
  const Counts old_loaded = Run(false, loaded), new_loaded = Run(true, loaded);
  const Counts old_ring = Run(false, ring), new_ring = Run(true, ring);
  const Counts old_slow = Run(false, slow), new_slow = Run(true, slow);
  std::puts("900 frames, 40 requests a frame:");
  Print("1.0.2, written while loading", old_loaded);
  Print("fix 36, written while loading", new_loaded);
  Print("1.0.2, ring buffer every frame", old_ring);
  Print("fix 36, ring buffer every frame", new_ring);
  Print("1.0.2, rewritten every 45 frames", old_slow);
  Print("fix 36, rewritten every 45 frames", new_slow);
  // Static after loading: watched again after the window, so a fraction of the hashes.
  assert(new_loaded.rewatched == 1);
  assert(new_loaded.hashes <= 40 * int(up::kFramesUnchangedToWatch + 1));
  assert(old_loaded.hashes >= 40 * 500);
  assert(new_loaded.faults == old_loaded.faults && new_loaded.faults == 3);
  // A ring buffer stays unwatched: same hashes and faults as 1.0.2.
  assert(new_ring.rewatched == 0);
  assert(new_ring.hashes == old_ring.hashes && new_ring.faults == old_ring.faults);
  // Slow churn: correct (asserted in Run) and few faults.
  assert(new_slow.faults <= 3 + 900 / 45);
  std::puts("units: ok");
}

std::string Read(const char* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

void CheckTree() {
  const std::string cc = Read(".deps/xenia-canary/src/xenia/gpu/shared_memory.cc");
  const std::string h = Read(".deps/xenia-canary/src/xenia/gpu/shared_memory.h");
  const std::string cp = Read(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  assert(!cc.empty() && !h.empty() && !cp.empty());
  assert(cc.find("#include \"xenia/gpu/unwatched_policy.h\"") != std::string::npos);
  assert(cc.find("unwatched_policy::OnRequest(") != std::string::npos);
  assert(cc.find("unit_changed_frame_[unit], unwatched_frame_);") != std::string::npos);
  assert(cc.find("if (decision.watch_again) {") != std::string::npos);
  assert(h.find("void SetUnwatchedFrame(uint64_t frame)") != std::string::npos);
  assert(cp.find("shared_memory_->SetUnwatchedFrame(frame_current_);") != std::string::npos);
  std::puts("assembled shared memory and command processor: ok");
}
}  // namespace

int main() {
  CheckPolicy();
  CheckUnits();
  CheckTree();
  std::puts("check-unwatched-policy: all passed");
  return 0;
}
