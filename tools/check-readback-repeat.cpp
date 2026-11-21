// SPDX-License-Identifier: MIT
// Fix 91 (tools/source-1.0.2/fixes/91-readback-repeat.patch): the fast resolve readback gives
// a key resolved again in the same frame its own copy. Tekken Tag Tournament 2 (issue 14)
// resolves its costume colours to one 240 KiB key three times in a row as a match loads and
// reads the last; the fast mode handed it the second step's copy (zeros), so King's and Bob's
// clothes came out cyan and blue, right with the full mode.
//
// Drives xe::gpu::vulkan::readback_slots (the assembled core's readback_slots.h) as the fast
// mode in vulkan_command_processor.cc does (the GPU keeping up, so every earlier copy is done):
//  * three resolves of one key in a frame: each gets its own copy (before: the previous step's);
//  * a key resolved once a frame keeps the fast mode's lateness: the previous frame's copy;
//  * the first resolve of a key gets its own copy (as before);
//  * and the assembled vulkan_command_processor.cc stamps, clears and asks the frame.
// Built against the assembled tree and run from its root (run-fix-checks.sh).
#include "xenia/gpu/vulkan/readback_slots.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace rs = xe::gpu::vulkan::readback_slots;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++failures;                                                  \
    }                                                              \
  } while (0)

// One key's slots in fast mode. Resolve() returns the number of the resolve whose copy the guest
// gets (resolves are numbered from 1).
struct Key {
  uint64_t submissions[rs::kCount] = {};
  uint32_t valid[rs::kCount] = {};
  uint64_t frames[rs::kCount] = {};
  int copy_of[rs::kCount] = {};
  uint64_t submission = 0;
  int resolves = 0;
  bool fix91 = true;
  int Resolve(uint64_t frame, uint32_t length = 245760) {
    const int number = ++resolves;
    ++submission;
    const uint64_t completed = submission - 1;  // the GPU keeps up
    const uint32_t write = rs::ChooseWrite(submissions, valid, rs::kCount, length, completed);
    const int newest = rs::ChooseRead(submissions, valid, rs::kCount, int(write), length, completed, false);
    bool miss = newest < 0;
    if (fix91 && newest >= 0 && rs::RepeatedInFrame(frames[newest], frame)) miss = true;
    submissions[write] = submission;
    valid[write] = length;
    frames[write] = frame;
    copy_of[write] = number;
    return miss ? number : copy_of[newest];
  }
};

std::string ReadText(const char* path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream s;
  s << in.rdbuf();
  return s.str();
}

int main() {
  // The rule itself: frame 0 is "no copy" (frames count from 1).
  CHECK(rs::RepeatedInFrame(5, 5) && !rs::RepeatedInFrame(4, 5) && !rs::RepeatedInFrame(0, 0) &&
        !rs::RepeatedInFrame(0, 5));

  // Tekken's costume colours: three resolves of 0FE7C000 in frame 7, the last one read.
  {
    Key fixed, before;
    before.fix91 = false;
    for (Key* k : {&fixed, &before}) {
      const int a = k->Resolve(7), b = k->Resolve(7), c = k->Resolve(7);
      if (k == &fixed) {
        CHECK(a == 1 && b == 2 && c == 3);  // each step its own copy
      } else {
        CHECK(a == 1 && b == 1 && c == 2);  // the bug: the previous step's copy
      }
    }
    // The next frame keeps the fast mode's lateness: the last copy of frame 7.
    CHECK(fixed.Resolve(8) == 3);
    CHECK(fixed.Resolve(9) == 4);
  }
  // A key resolved once a frame (luminance, bloom): the previous frame's copy, as before.
  {
    Key k;
    CHECK(k.Resolve(1) == 1);  // a key's first resolve: its own copy
    for (uint64_t frame = 2; frame < 12; ++frame) CHECK(k.Resolve(frame) == int(frame) - 1);
  }
  // Every other frame: still the previous resolve's copy, never the same frame's.
  {
    Key k;
    CHECK(k.Resolve(2) == 1);
    CHECK(k.Resolve(4) == 1 && k.Resolve(6) == 2 && k.Resolve(8) == 3);
  }
  // Twice in a frame, every frame: the second gets its own; the first, the previous frame's last.
  {
    Key k;
    CHECK(k.Resolve(1) == 1 && k.Resolve(1) == 2);
    CHECK(k.Resolve(2) == 2 && k.Resolve(2) == 4);
    CHECK(k.Resolve(3) == 4 && k.Resolve(3) == 6);
  }

  const std::string cp = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.cc");
  const std::string h = ReadText(".deps/xenia-canary/src/xenia/gpu/vulkan/vulkan_command_processor.h");
  if (cp.empty() || h.empty()) {
    std::printf("no assembled tree here; the command processor not checked\n");
  } else {
    CHECK(h.find("uint64_t frames[readback_slots::kCount] = {};  // fix 91") != std::string::npos);
    const size_t miss = cp.find("bool read_cache_miss = readback_mode == ReadbackResolveMode::kFast &&");
    const size_t rule = cp.find("readback_slots::RepeatedInFrame(rb.frames[newest_copy], frame_current_)");
    const size_t scaled = cp.find("if (is_scaled) {", miss);
    CHECK(miss != std::string::npos && miss < rule && rule < scaled);
    // Fast mode only, never the automatic copy (it never waits), and only with an earlier copy.
    const std::string guard =
        rule == std::string::npos ? std::string() : cp.substr(rule > 160 ? rule - 160 : 0, 160);
    CHECK(guard.find("readback_mode == ReadbackResolveMode::kFast && !auto_readback && newest_copy >= 0") !=
          std::string::npos);
    CHECK(cp.find("read_cache_miss = true;", rule) < scaled);
    // Stamped with the copy, cleared with a new buffer.
    CHECK(cp.find("rb.frames[write_index] = frame_current_;  // fix 91") != std::string::npos);
    CHECK(cp.find("rb.submissions[write_index] = 0;\n      rb.frames[write_index] = 0;") != std::string::npos);
    // The fast mode's miss path still gives this copy after waiting.
    CHECK(cp.find("} else if (read_cache_miss) {\n      // Delayed sync, but the previous buffer doesn't exist") !=
          std::string::npos);
    std::printf("tree: the fast readback gives a key resolved again in a frame its own copy\n");
  }
  if (failures) {
    std::printf("%d failure(s)\n", failures);
    return 1;
  }
  std::printf("check-readback-repeat: all passed\n");
  return 0;
}
